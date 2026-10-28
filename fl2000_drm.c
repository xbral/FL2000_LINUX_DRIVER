// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2012, Red Hat
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

#define DRM_DRIVER_NAME "fl2000_drm"
#define DRM_DRIVER_DESC "USB-HDMI"
#define DRM_DRIVER_DATE "20181001"

#define DRM_DRIVER_MAJOR      0
#define DRM_DRIVER_MINOR      0
#define DRM_DRIVER_PATCHLEVEL 1

/* Maximum supported resolution, out-of-the-blue numbers */
#define FL20000_MAX_WIDTH  4000
#define FL20000_MAX_HEIGHT 4000

/* Force using 32-bit XRGB8888 on input for simplicity */
#define FL2000_FB_BPP 32
static const u32 fl2000_pixel_formats[] = {
	DRM_FORMAT_XRGB8888,
};

/* Maximum pixel clock set to 500MHz. It is hard to get more or less precise PLL configuration for
 * higher clock
 */
#define FL2000_MAX_PIXCLOCK 500000000

/* PLL computing precision is 6 digits after comma */
#define FL2000_PLL_PRECISION 1000000

/* Input xtal clock, Hz */
#define FL2000_XTAL 10000000 /* 10 MHz */

/* Internal vco clock min/max, Hz */
#define FL2000_VCOCLOCK_MIN 62500000 /* 62.5 MHz */
#define FL2000_VCOCLOCK_MAX 1000000000 /* 1GHz */

/* Maximum acceptable ppm error */
#define FL2000_PPM_ERR_MAX 500

/* Sustainable bulk bandwidth per USB speed, bytes per second. The chip's VGA line buffer
 * drains at bytes_per_pixel * pixclock, and when USB cannot refill it at that rate the DAC
 * output timing becomes irregular - visible as shimmer/tremor at high resolutions. So pick
 * the per-mode bytes/pixel against a budget the bus can actually sustain.
 *
 * SuperSpeed: the nominal payload rate is 500 MB/s (5 Gbps after 8b/10b), but real-world
 * xHCI bulk transfers top out around 350-400 MB/s. 1920x1080@60 in RGB888 drains 445 MB/s,
 * above the fill rate; the FL2000DX was designed for 16-bit colour at 1080p, where the drain
 * is a comfortable 297 MB/s. The 350 MB/s budget therefore selects RGB565 exactly for the
 * modes that need it and RGB888 below.
 *
 * USB 2.0 High Speed and SuperSpeed+ are handled in fl2000_get_bytes_pix() directly: no
 * honest HS budget allows any real mode (so all modes stay allowed with RGB888 as before),
 * and the 10 Gbps SS+ budget covers RGB888 even at the pixel clock ceiling.
 */
#define FL2000_BULK_BW_SUPER_SPEED	 350000000ull

static unsigned int fl2000_get_bytes_pix(enum usb_device_speed speed, unsigned int pixclock)
{
	unsigned int max_bw;

	/* Diagnostics/testing override: force a pixel format on every mode */
	if (fl2000_force_bpp == 2 || fl2000_force_bpp == 3)
		return fl2000_force_bpp;

	switch (speed) {
	case USB_SPEED_HIGH:
		/* USB 2.0 cannot honestly sustain the drain of any real mode; the chip stalls
		 * its DAC gracefully when starved, and historically every mode was offered
		 * with RGB888. Keep that so USB 2.0 mode filtering does not regress.
		 */
		return 3;
	case USB_SPEED_SUPER:
		max_bw = FL2000_BULK_BW_SUPER_SPEED;
		break;
	case USB_SPEED_SUPER_PLUS:
		/* 10 Gbps budget covers RGB888 even at the 500 MHz pixel clock ceiling */
		return 3;
	default:
		return 0;
	}

	/* pixclock arrives in kHz (DRM convention), max_bw is bytes/second, so the drain is
	 * bytes_pix * pixclock * 1000. Prefer RGB888, fall back to RGB565, reject if even
	 * RGB565 overruns the budget. Overflow-safe: pixclock is capped at
	 * FL2000_MAX_PIXCLOCK (500 MHz) by the caller, so *3000 fits in u32*1000 arithmetic
	 */
	if ((u64)pixclock * 3000 <= max_bw)
		return 3;
	if ((u64)pixclock * 2000 <= max_bw)
		return 2;

	return 0;
}

struct fl2000_drm_if {
	struct usb_device *usb_dev;
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct fl2000_stream *stream;
	struct fl2000_intr *intr;

	/* Only used when driving the integrated VGA DAC, i.e. when there is no bridge component */
	struct drm_connector connector;
	const struct drm_edid *drm_edid;

	/* Last mode programmed into the chip, kept so post_reset can reprogram it after a
	 * USB device reset (chip app-reset + register replay). Filled by
	 * fl2000_output_mode_set()
	 */
	struct fl2000_pll last_pll;
	struct fl2000_timings last_timings;
	unsigned int last_bytes_pix;
	bool mode_config_valid;

	/* Serializes the chip register programming sequence between a real modeset
	 * (fl2000_output_mode_set) and the post_reset reprogram (fl2000_stream_reprogram).
	 * Without it the two sequences could interleave register writes; both run in process
	 * context (atomic commit worker vs. watchdog workqueue), so a mutex is enough
	 */
	struct mutex mode_lock;

	/* Sticky presence latch (incident #3): once the sense register has seen the monitor
	 * this session, a later sense==0/error no longer reports a disconnect - the detect
	 * circuit can die mid-session with the cable still attached, and a false disconnect
	 * makes the compositor kill the output (black screen until the dongle is replugged)
	 */
	bool monitor_seen;

	/* Detect-recovery guard (incident #3): one re-arm attempt per loss episode, so a
	 * genuinely unplugged monitor does not get a control write every poll. Cleared when
	 * the sense circuit reports the monitor again
	 */
	bool detect_rearm_tried;
};

/* Read one EDID block over the FL2000's own I2C master.
 *
 * This deliberately bypasses the i2c_adapter the driver registers. That adapter describes the
 * hardware honestly - fl2000_i2c_quirks advertises max_read_len = 1 - so drm_edid_read_ddc() and
 * friends are rejected by i2c_check_quirks() before any transfer happens. It is also wasteful:
 * fl2000_i2c_xfer() reads a full dword from the chip and then throws three of the four bytes away.
 * Going straight to fl2000_i2c_dword() keeps all four, turning a 128 byte block into 32 USB
 * round trips rather than 128
 */
static int fl2000_get_edid_block(void *data, u8 *buf, unsigned int block, size_t len)
{
	struct fl2000_drm_if *drm_if = data;
	struct usb_device *usb_dev = drm_if->usb_dev;
	unsigned int base = block * EDID_LENGTH;

	/* i2c_offset in FL2000_VGA_I2C_SC_REG is 8 bits wide and there is no E-DDC segment
	 * pointer, so only the base block and one extension are reachable
	 */
	if (base + len > 256)
		return -E2BIG;

	for (size_t i = 0; i < len; i += sizeof(u32)) {
		union {
			u32 w;
			u8 b[4];
		} dword;
		int ret;

		ret = fl2000_i2c_dword(usb_dev, true, FL2000_DDC_ADDR, base + i, &dword.w);
		if (ret)
			return ret;

		memcpy(&buf[i], dword.b, min_t(size_t, sizeof(dword.b), len - i));
		/* 32 round-trips USB seguidos sem ceder a CPU travavam o
		 * output_poll; cede a cada dword.
		 */
		cond_resched();
	}

	return 0;
}

static int fl2000_connector_get_modes(struct drm_connector *connector)
{
	struct fl2000_drm_if *drm_if = container_of(connector, struct fl2000_drm_if, connector);
	struct drm_display_mode *mode;
	bool have_1080p = false;
	int count;

	/* Re-read rather than cache: a VGA sink can be swapped without us being told */
	drm_edid_free(drm_if->drm_edid);
	drm_if->drm_edid = drm_edid_read_custom(connector, fl2000_get_edid_block, drm_if);

	/* A replug or a just-revived detect block can NAK the first EDID read; give it one
	 * bounded retry before falling back to the VESA mode list
	 */
	if (!drm_if->drm_edid) {
		fl2000_detect_rearm(drm_if->usb_dev);
		msleep(20);
		drm_if->drm_edid = drm_edid_read_custom(connector, fl2000_get_edid_block, drm_if);
	}

	drm_edid_connector_update(connector, drm_if->drm_edid);

	if (drm_if->drm_edid)
		return drm_edid_connector_add_modes(connector);

	/* No EDID. VGA monitors without usable DDC are common (missing pins 12/15 in the cable,
	 * adapters, gated chip paths), and are exactly why DRM carries standard fallback modes.
	 * Offer VESA modes up to a conservative analog ceiling and prefer the old safe default;
	 * the user can force something bigger explicitly
	 */
	count = drm_add_modes_noedid(connector, 1920, 1200);

	/* The kernel's fallback tables do not reliably carry 1920x1080@60 - the one detailed
	 * timing most FHD monitors ship with. This dongle's DDC path often reads nothing on a
	 * cold boot, so without this the native resolution would vanish after every reboot.
	 * mode_valid still filters it through the PLL/bandwidth checks
	 */
	list_for_each_entry(mode, &connector->modes, head) {
		if (!strcmp(mode->name, "1920x1080")) {
			have_1080p = true;
			break;
		}
	}

	if (!have_1080p) {
		static const struct drm_display_mode fhd_60 = {
			DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2008,
				 2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
				 DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
		};

		mode = drm_mode_duplicate(connector->dev, &fhd_60);
		if (mode) {
			drm_mode_probed_add(connector, mode);
			count++;
		}
	}

	drm_set_preferred_mode(connector, 1024, 768);

	return count;
}

/* VGA has no hotplug detect line, so probe the sink by talking to its DDC EEPROM */
static enum drm_connector_status fl2000_connector_detect(struct drm_connector *connector, bool force)
{
	struct fl2000_drm_if *drm_if = container_of(connector, struct fl2000_drm_if, connector);
	u32 header;
	int sense;

	UNUSED(force);

	/* Caminho quente do output_poll (10 s): sense primeiro. É 1 control
	 * read rápido (0x805C); DDC é 1 dword com poll-loop de até 100 ms e
	 * NAKa mesmo com monitor Windows-provado (NOTES.md Round 2-4) — fazia
	 * o poll travar centenas de ms e era a "travadinha" periódica.
	 *
	 * Incident #3: o bloco de detect do chip morreu no meio da sessão
	 * (mon_det=0, i2c pendurado) com o cabo VGA preso. O sense==0/erro virava
	 * "disconnected", o compositor desligava a saída e a tela ficava preta
	 * até replugar o dongle. Conclusão: o sense NÃO prova ausência de monitor
	 * (só o sentido 1 é confiável), então depois de ver o monitor uma vez na
	 * sessão, a saída fica ligada por insistência (sticky) — um monitor
	 * realmente removido apenas continua recebendo sinal, inofensivo.
	 */
	sense = fl2000_connector_sense(drm_if->usb_dev);
	if (sense == 1) {
		drm_if->monitor_seen = true;
		drm_if->detect_rearm_tried = false;
		return connector_status_connected;
	}

	/* Sense falhou ou diz "sem monitor" depois de já ter visto um. Antes de acreditar
	 * (ou de apelar para o latch sticky), tenta rearmar o bloco de detect uma vez por
	 * episódio: a causa da incident #3 foi exatamente esse bloco morrer, não o cabo sair.
	 */
	if (drm_if->monitor_seen) {
		if (!drm_if->detect_rearm_tried) {
			drm_if->detect_rearm_tried = true;

			if (!fl2000_detect_rearm(drm_if->usb_dev)) {
				sense = fl2000_connector_sense(drm_if->usb_dev);
				if (sense == 1) {
					dev_info_ratelimited(&drm_if->usb_dev->dev,
						"detect: sense circuit recovered after re-arm");
					return connector_status_connected;
				}
			}
		}

		/* Mantenha a saída viva. sticky_connect=0 restaura o comportamento antigo. */
		if (fl2000_sticky_connect) {
			dev_warn_ratelimited(&drm_if->usb_dev->dev,
				"detect: sense says no monitor (ret %d) but monitor was seen; staying "
				"connected (sticky_connect=0 to trust the sense register)", sense);
			return connector_status_connected;
		}
	}

	if (sense == 0)
		return connector_status_disconnected;

	/* Sense falhou (erro USB): último recurso é o DDC. Mantido só como
	 * fallback porque um dia o dongle pode responder EDID.
	 */
	if (!fl2000_i2c_dword(drm_if->usb_dev, true, FL2000_DDC_ADDR, 0, &header))
		return connector_status_connected;

	/* A DDC NAK does not prove absence: this dongle NAKs the DDC EEPROM even with a
	 * Windows-proven monitor attached and awake. The chip's analog sense register is the
	 * reliable presence signal here (tracks physical cable attach/detach exactly), and a
	 * desktop needs a definitive 'connected' to enable the output - compositors ignore
	 * 'unknown'. Without EDID, get_modes() serves the VESA fallback list
	 */
	return connector_status_unknown;
}

static const struct drm_connector_helper_funcs fl2000_connector_helper_funcs = {
	.get_modes = fl2000_connector_get_modes,
};

static const struct drm_connector_funcs fl2000_connector_funcs = {
	.detect = fl2000_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

DEFINE_DRM_GEM_FOPS(fl2000_drm_driver_fops);

/* No .release callback: it runs after the refcount hits zero, and the old one called
 * drm_atomic_helper_shutdown() from there - which takes a fresh drm_dev_get() on the dying
 * device (refcount saturation, "use-after-free" WARN when the compositor dropped the last fd
 * after hot-unplug). Shutdown now happens in fl2000_drm_if_release() while the device is
 * alive. The old drm_mode_config_cleanup() call was also redundant-to-harmful: mode config
 * comes from drmm_mode_config_init() and is cleaned up by the managed release path
 */
static struct drm_driver fl2000_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.ioctls = NULL,
	.fops = &fl2000_drm_driver_fops,

	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,

	.name = DRM_DRIVER_NAME,
	.desc = DRM_DRIVER_DESC,
	.major = DRM_DRIVER_MAJOR,
	.minor = DRM_DRIVER_MINOR,
	.patchlevel = DRM_DRIVER_PATCHLEVEL,
};

static const struct drm_mode_config_funcs fl2000_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Integer division compute of ppm error */
static u64 fl2000_pll_ppm_err(u64 clock_mil, u32 vco_clk, u32 divisor)
{
	u64 pll_clk_mil = (u64)vco_clk * FL2000_PLL_PRECISION / divisor;
	u64 pll_clk_err;

	/* Not using abs() here to avoid possible overflow */
	if (pll_clk_mil > clock_mil)
		pll_clk_err = pll_clk_mil - clock_mil;
	else
		pll_clk_err = clock_mil - pll_clk_mil;

	return pll_clk_err / (clock_mil / FL2000_PLL_PRECISION);
}

static inline u32 fl2000_pll_get_divisor(u64 clock_mil, u32 vco_clk, u64 *min_ppm_err)
{
	static const u32 divisor_arr[] = {
		2,   4,	  6,   7,   8,	 9,   10,  11,	12,  13,  14,  15,  16,	 17,  18,  19,
		20,  21,  22,  23,  24,	 25,  26,  27,	28,  29,  30,  31,  32,	 33,  34,  35,
		36,  37,  38,  39,  40,	 41,  42,  43,	44,  45,  46,  47,  48,	 49,  50,  51,
		52,  53,  54,  55,  56,	 57,  58,  59,	60,  61,  62,  63,  64,	 65,  66,  67,
		68,  69,  70,  71,  72,	 73,  74,  75,	76,  77,  78,  79,  80,	 81,  82,  83,
		84,  85,  86,  87,  88,	 89,  90,  91,	92,  93,  94,  95,  96,	 97,  98,  99,
		100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115,
		116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128
	};
	u32 best_divisor = 0;

	/* Iterate over array */
	for (int i = 0; i < ARRAY_SIZE(divisor_arr); i++) {
		u32 divisor = divisor_arr[i];
		u64 ppm_err = fl2000_pll_ppm_err(clock_mil, vco_clk, divisor);

		if (ppm_err < *min_ppm_err) {
			*min_ppm_err = ppm_err;
			best_divisor = divisor;
		}
	}

	return best_divisor;
}

/* Try to match pixel clock - find parameters with minimal PLL error */
static u64 fl2000_pll_calc(u64 clock_mil, struct fl2000_pll *pll, u32 *clock_calculated)
{
	static const u32 prescaler_max = 2;
	static const u32 multiplier_max = 128;
	u64 min_ppm_err = (u64)(-1);

	for (u32 prescaler = 1; prescaler <= prescaler_max; prescaler++)
		for (u32 multiplier = 1; multiplier <= multiplier_max; multiplier++) {
			/* Do not need precision here yet, no 10^6 multiply */
			u32 vco_clk = FL2000_XTAL / prescaler * multiplier;
			u32 divisor;

			if (vco_clk < FL2000_VCOCLOCK_MIN || vco_clk > FL2000_VCOCLOCK_MAX)
				continue;

			divisor = fl2000_pll_get_divisor(clock_mil, vco_clk, &min_ppm_err);
			if (divisor == 0)
				continue;

			pll->prescaler = prescaler;
			pll->multiplier = multiplier;
			pll->divisor = divisor;
			if (vco_clk < 125000000)
				pll->function = 0;
			else if (vco_clk < 250000000)
				pll->function = 1;
			else if (vco_clk < 500000000)
				pll->function = 2;
			else
				pll->function = 3;
			*clock_calculated = vco_clk / divisor;
		}

	return min_ppm_err;
}

static int fl2000_mode_calc(const struct drm_display_mode *mode,
			    struct drm_display_mode *adjusted_mode, struct fl2000_pll *pll)
{
	u64 ppm_err;
	u32 clock_calculated;
	u64 clock_mil_adjusted;
	const u64 clock_mil = (u64)mode->clock * 1000 * FL2000_PLL_PRECISION;
	const int max_h_adjustment = 10;

	if (mode->clock * 1000 > FL2000_MAX_PIXCLOCK)
		return -1;

	/* Try to match pixel clock slightly adjusting htotal value, sequence is:
	 * 0, -1, 1, -2, 2, -3, 3, -3, 4, -4, 5, -5, ...
	 * Here, 's' is used for sign, 'm' is used for modulo, and 'd' is the adjustment value
	 */
	for (int m = 0, s = 1, d = 0; m <= max_h_adjustment * 2; m++, s = -s, d += m * s) {
		/* Maximum pixel clock 1GHz, or 10^9Hz. Multiply by 10^6 we get 10^15Hz. Assume
		 * maximum htotal is 10000 pix (no way) we get 10^19 max value and using u64 which
		 * is 1.8*10^19 no overflow can occur. Assume all this was checked before
		 */
		clock_mil_adjusted = clock_mil * (mode->htotal + d) / mode->htotal;

		/* To keep precision use clock multiplied by 10^6 */
		ppm_err = fl2000_pll_calc(clock_mil_adjusted, pll, &clock_calculated);

		/* Stop searching as soon as the first valid option found */
		if (ppm_err < FL2000_PPM_ERR_MAX) {
			if (adjusted_mode) {
				drm_mode_copy(adjusted_mode, mode);
				adjusted_mode->htotal += d;
				adjusted_mode->clock = clock_calculated / 1000;
			}

			return 0;
		}
	}

	/* Cannot find PLL configuration that satisfy requirements */
	return -1;
}

static enum drm_mode_status fl2000_display_mode_valid(struct drm_simple_display_pipe *pipe,
						      const struct drm_display_mode *mode)
{
	struct drm_device *drm = pipe->crtc.dev;
	struct drm_display_mode adjusted_mode;
	struct fl2000_pll pll;
	struct fl2000_drm_if *drm_if = drm->dev_private;
	struct usb_device *usb_dev = drm_if->usb_dev;

	/* Get PLL configuration and check if mode adjustments needed */
	if (fl2000_mode_calc(mode, &adjusted_mode, &pll)) {
		dev_dbg(drm->dev, "%s: no PLL setting within tolerance\n", mode->name);
		return MODE_BAD;
	}

	if (fl2000_get_bytes_pix(usb_dev->speed, adjusted_mode.clock) == 0) {
		dev_dbg(drm->dev,
			"%s: pixel drain exceeds USB bandwidth budget\n", mode->name);
		return MODE_BAD;
	}

	return MODE_OK;
}

static void fl2000_display_enable(struct drm_simple_display_pipe *pipe,
				  struct drm_crtc_state *cstate,
				  struct drm_plane_state *plane_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = pipe->crtc.dev;
	struct fl2000_drm_if *drm_if = drm->dev_private;

	/* TODO: check cstate/pstate? */
	UNUSED(cstate);
	UNUSED(plane_state);

	/* The stall watchdog is armed here and nowhere else, so this log is what
	 * distinguishes "the watchdog never ran" from "the watchdog ran and died":
	 * without it a black screen with no watchdog thread is indistinguishable
	 * from a black screen whose atomic commit never reached this point
	 * (incident 2026-10-28: the CRTC was active, the mode was configured, and
	 * no fl2000_stall worker existed anywhere in /proc).
	 */
	dev_info(drm->dev, "display enable: arming the stream");

	fl2000_stream_enable(drm_if->stream);

	drm_crtc_vblank_on(crtc);
}

static void fl2000_display_disable(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = pipe->crtc.dev;
	struct fl2000_drm_if *drm_if = drm->dev_private;

	dev_info(drm->dev, "display disable: stopping the stream");

	/* Vblanks on this device ARE stream URB completions, so they stop the moment the
	 * stream does (and never come at all after hot-unplug). Turn vblanks off first: it
	 * wakes every waiter and flushes pending events, instead of letting the atomic helper
	 * wait out a timeout against a stream that can no longer tick
	 */
	drm_crtc_vblank_off(crtc);

	fl2000_stream_disable(drm_if->stream);
}

/* The fb can be larger than the mode the CRTC is running (an fbdev restore commits its
 * old 1920x1080 fb onto the 1024x768 preferred mode, and atomic check clips the plane
 * without complaining). Compressing with the raw fb dimensions then walks past the end
 * of the stream buffers - the OOB page fault in fl2000_stream_compress of kdump
 * 202609040702. Clip the copy to the active mode instead.
 */
static void fb2000_dirty(struct drm_framebuffer *fb, unsigned int clip_width,
			 unsigned int clip_height)
{
	int idx;
	struct drm_device *drm = fb->dev;
	struct fl2000_drm_if *drm_if = drm->dev_private;
	struct drm_gem_object *gem_obj = drm_gem_fb_get_obj(fb, 0);
	struct iosys_map map;
	unsigned int width = min(fb->width, clip_width);
	unsigned int height = min(fb->height, clip_height);

	if (!drm_dev_enter(fb->dev, &idx)) {
		dev_err(drm->dev, "DRM enter failed!");
		return;
	}

	if (drm_gem_vmap(gem_obj, &map)) {
		dev_err(drm->dev, "Cannot map framebuffer for CPU access!");
		drm_dev_exit(idx);
		return;
	}

	fl2000_stream_compress(drm_if->stream, map.vaddr, height, width, fb->pitches[0]);

	drm_gem_vunmap(gem_obj, &map);

	drm_dev_exit(idx);
}

static void fl2000_display_update(struct drm_simple_display_pipe *pipe,
				  struct drm_plane_state *old_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct drm_pending_vblank_event *event = crtc->state->event;
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_rect rect;

	if (drm_atomic_helper_damage_merged(old_state, state, &rect)) {
		struct drm_crtc_state *cstate = crtc->state;
		unsigned int clip_width = state->fb->width;
		unsigned int clip_height = state->fb->height;

		if (cstate) {
			if (cstate->adjusted_mode.hdisplay)
				clip_width = min_t(unsigned int, clip_width,
						   cstate->adjusted_mode.hdisplay);
			if (cstate->adjusted_mode.vdisplay)
				clip_height = min_t(unsigned int, clip_height,
						    cstate->adjusted_mode.vdisplay);
		}

		fb2000_dirty(state->fb, clip_width, clip_height);
	}

	if (event) {
		crtc->state->event = NULL;

		spin_lock_irq(&drm->event_lock);
		if (crtc->state->active && drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, event);
		else
			drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irq(&drm->event_lock);
	}
}

/* Logical pipe management (no HW configuration here) */
static const struct drm_simple_display_pipe_funcs fl2000_display_funcs = {
	.mode_valid = fl2000_display_mode_valid,
	.enable = fl2000_display_enable,
	.disable = fl2000_display_disable,
	.update = fl2000_display_update
};

static void fl2000_output_mode_set(struct drm_encoder *encoder, struct drm_display_mode *mode,
				   struct drm_display_mode *adjusted_mode)
{
	struct drm_device *drm = encoder->dev;
	struct fl2000_drm_if *drm_if = drm->dev_private;
	struct usb_device *usb_dev = drm_if->usb_dev;
	struct fl2000_timings timings;
	struct fl2000_pll pll;
	unsigned int bytes_pix;

	/* Get PLL configuration and cehc if mode adjustments needed */
	if (fl2000_mode_calc(mode, adjusted_mode, &pll))
		return;

	/* Check how many bytes per pixel shall be used with adjusted clock */
	bytes_pix = fl2000_get_bytes_pix(usb_dev->speed, adjusted_mode->clock);
	if (!bytes_pix)
		return;

	/* Serialize the register sequence against the watchdog's hard recovery */
	mutex_lock(&drm_if->mode_lock);

	dev_info(drm->dev, "Mode requested:  " DRM_MODE_FMT, DRM_MODE_ARG(mode));
	dev_info(drm->dev, "Mode configured: " DRM_MODE_FMT, DRM_MODE_ARG(adjusted_mode));
	dev_info(drm->dev, "Pixel format: %u bytes/pixel (%s)", bytes_pix,
		 bytes_pix == 2 ? "RGB565" : "RGB888");

	/* Prepare timing configuration */
	timings.hactive = adjusted_mode->hdisplay;
	timings.htotal = adjusted_mode->htotal;
	timings.hsync_width = adjusted_mode->hsync_end - adjusted_mode->hsync_start;
	timings.hstart = adjusted_mode->htotal - adjusted_mode->hsync_start + 1;
	timings.vactive = adjusted_mode->vdisplay;
	timings.vtotal = adjusted_mode->vtotal;
	timings.vsync_width = adjusted_mode->vsync_end - adjusted_mode->vsync_start;
	timings.vstart = adjusted_mode->vtotal - adjusted_mode->vsync_start + 1;

	/* Set PLL settings */
	fl2000_set_pll(usb_dev, &pll);

	/* Reset FL2000 & confirm PLL settings */
	fl2000_reset(usb_dev);

	/* Set timings settings */
	fl2000_set_timings(usb_dev, &timings);

	/* Pixel format according to number of bytes per pixel */
	fl2000_set_pixfmt(usb_dev, bytes_pix);

	/* Configure frame transfers */
	fl2000_set_transfers(usb_dev);

	/* Enable interrupts */
	fl2000_enable_interrupts(usb_dev);

	fl2000_afe_magic(usb_dev);

	fl2000_stream_mode_set(drm_if->stream, mode->hdisplay * mode->vdisplay, bytes_pix);

	/* Remember the working configuration so post_reset can reprogram the chip after a
	 * USB device reset without a full modeset. mode_lock keeps this snapshot
	 * consistent with a concurrent post_reset reprogram
	 */
	drm_if->last_pll = pll;
	drm_if->last_timings = timings;
	drm_if->last_bytes_pix = bytes_pix;
	drm_if->mode_config_valid = true;

	mutex_unlock(&drm_if->mode_lock);
}

/* Chip reprogramming after a USB device reset: same register sequence as
 * fl2000_output_mode_set(), preceded by the streaming altsetting re-select and followed by
 * fl2000_usb_magic() - both required because the reset restores altsetting 0 and wipes every
 * register. Control transfers with a bounded timeout, so it cannot hang even against a
 * wedged chip. Called from fl2000_drm_post_reset(), i.e. the USB core's post_reset path.
 *
 * Every step is named on failure. The whole sequence used to be one dev_err() printed
 * *before* any register was touched, and the post_reset caller threw the return value away,
 * so a run that died on the very first usb_set_interface() was indistinguishable in dmesg
 * from one that programmed the chip perfectly: "USB device reset: reprogramming chip mode"
 * was simply the last line the driver ever printed (incident 2026-10-16 09:33). "Which
 * register write did not answer" is the whole difference between blaming the chip and
 * blaming this code.
 */
static int fl2000_stream_reprogram(void *ctx)
{
	struct fl2000_drm_if *drm_if = ctx;
	struct usb_device *usb_dev = drm_if->usb_dev;
	const char *step;
	int ret;

	mutex_lock(&drm_if->mode_lock);

	if (!drm_if->mode_config_valid) {
		mutex_unlock(&drm_if->mode_lock);
		return -ENODEV;
	}

	dev_err(drm_if->drm.dev, "USB device reset: reprogramming chip mode");

	step = "select streaming altsetting";
	ret = usb_set_interface(usb_dev, FL2000_USBIF_AVCONTROL, 1);
	if (ret)
		goto failed;

	step = "set PLL";
	ret = fl2000_set_pll(usb_dev, &drm_if->last_pll);
	if (ret)
		goto failed;

	step = "chip reset";
	ret = fl2000_reset(usb_dev);
	if (ret)
		goto failed;

	step = "set timings";
	ret = fl2000_set_timings(usb_dev, &drm_if->last_timings);
	if (ret)
		goto failed;

	step = "set pixel format";
	ret = fl2000_set_pixfmt(usb_dev, drm_if->last_bytes_pix);
	if (ret)
		goto failed;

	step = "set transfers";
	ret = fl2000_set_transfers(usb_dev);
	if (ret)
		goto failed;

	step = "enable interrupts";
	ret = fl2000_enable_interrupts(usb_dev);
	if (ret)
		goto failed;

	step = "AFE magic";
	ret = fl2000_afe_magic(usb_dev);
	if (ret)
		goto failed;

	step = "USB magic";
	ret = fl2000_usb_magic(usb_dev);
	if (ret)
		goto failed;

	dev_err(drm_if->drm.dev,
		"chip reprogram OK: %ux%u at %u bytes/px, streaming restart is up to the stall watchdog",
		drm_if->last_timings.hactive, drm_if->last_timings.vactive,
		drm_if->last_bytes_pix);

	mutex_unlock(&drm_if->mode_lock);

	return 0;

failed:
	dev_err(drm_if->drm.dev,
		"chip reprogram FAILED (%d) at \"%s\": registers left as the reset found them, display stays black until a replug",
		ret, step);

	mutex_unlock(&drm_if->mode_lock);

	return ret;
}

/* FL2000 HW control functions: mode configuration, turn on/off */
static const struct drm_encoder_helper_funcs fl2000_encoder_funcs = {
	.mode_set = fl2000_output_mode_set,
};

static void fl2000_drm_if_release(struct fl2000_drm_if *drm_if, struct device *master)
{
	struct drm_device *drm = &drm_if->drm;
	struct usb_device *usb_dev = drm_if->usb_dev;

	dev_info(master, "Unbinding FL2000 master");

	/* Detach bridge. Nothing was bound in VGA mode */
	if (!fl2000_vga_mode)
		component_unbind_all(master, drm);

	drm_kms_helper_poll_fini(drm);

	/* Shutdown BEFORE tearing down the stream/interrupt contexts. The atomic shutdown
	 * runs a disable-all commit that calls the pipe's .disable callback, i.e.
	 * fl2000_display_disable() -> fl2000_stream_disable(drm_if->stream). The old order
	 * destroyed the stream first (devres free) and then ran shutdown, so the pipe
	 * callbacks dereferenced freed memory - the hot-unplug kernel panic of kdump
	 * 202609040716 and 202609041234 (machine "reboots by itself" on unplug). The null
	 * checks in fl2000_stream_disable() do not help against a dangling pointer, only
	 * ordering does
	 */
	drm_atomic_helper_shutdown(drm);

	/* From here on drm_dev_enter() fails, so no new commit or fbdev client restore can
	 * reach the stream (this also closes the use-after-free of kdump 202609040702,
	 * where a last-fd-close fbdev restore committed into a torn-down stream)
	 */
	drm_dev_unplug(drm);

	/* Stop streaming interface */
	fl2000_stream_destroy(usb_dev);
	drm_if->stream = NULL;

	/* Stop interrupts interface */
	fl2000_intr_destroy(usb_dev);
	drm_if->intr = NULL;

	drm_edid_free(drm_if->drm_edid);
	drm_if->drm_edid = NULL;

	/* No drm_dev_put() here - the drm_device came from devm_drm_dev_alloc(master) and is
	 * released with the master device
	 */
}

/* TODO: release on errors! */
int fl2000_drm_bind(struct device *master)
{
	int ret = 0;
	struct usb_device *usb_dev = to_usb_device(master->parent);
	struct fl2000_drm_if *drm_if;
	struct drm_device *drm;
	struct drm_mode_config *mode_config;
	u64 dma_mask;
	bool bridge_bound = false;
	bool poll_initialized = false;

	dev_info(master, "Binding FL2000 master");

	drm_if = devm_drm_dev_alloc(master, &fl2000_drm_driver, struct fl2000_drm_if, drm);
	if (IS_ERR(drm_if)) {
		dev_err(master, "Cannot allocate DRM structure (%ld)", PTR_ERR(drm_if));
		return (int)PTR_ERR(drm_if);
	}
	drm = &drm_if->drm;
	drm_if->usb_dev = usb_dev;
	drm->dev_private = drm_if;
	mutex_init(&drm_if->mode_lock);

	/* Let fl2000_drm_unbind() find us again. master is the i2c adapter device, whose drvdata
	 * is otherwise unused - this driver never calls i2c_set_adapdata()
	 */
	dev_set_drvdata(master, drm_if);

	ret = drmm_mode_config_init(drm);
	if (ret) {
		dev_err(master, "Cannot initialize DRM mode (%d)", ret);
		goto error;
	}

	mode_config = &drm->mode_config;
	mode_config->funcs = &fl2000_mode_config_funcs;
	mode_config->min_width = 1;
	mode_config->max_width = FL20000_MAX_WIDTH;
	mode_config->min_height = 1;
	mode_config->max_height = FL20000_MAX_HEIGHT;

	/* Set DMA mask for DRM device from mask of the 'parent' USB device */
	dma_mask = dma_get_mask(&usb_dev->dev);
	ret = dma_set_coherent_mask(drm->dev, dma_mask);
	if (ret) {
		dev_err(drm->dev, "Cannot set DRM device DMA mask (%d)", ret);
		goto error;
	}

/* Pinned connector name.
 *
 * Left to itself the kernel builds the name as "<TYPE>-<id>" where the id comes from
 * connector_ida, an IDA shared by *every* DRM device on the machine, allocated with
 * ida_alloc_min(). The number is therefore just the lowest free slot at bind time, and
 * it moves as other devices come and go: this dongle has been VGA-1, VGA-2, VGA-3, VGA-4
 * and VGA-5 on this machine across re-enumerations.
 *
 * That matters because Mutter identifies a monitor by (connector name, vendor, product,
 * serial) and stores its layout under that key in ~/.config/monitors.xml. Every login
 * that lands on a different number silently fails to match the stored layout, so the
 * connector comes up connected but disabled and the display stays black until the config
 * is reapplied by hand. A fixed name makes the key stable.
 */
#define FL2000_VGA_CONNECTOR_NAME "VGA-1"

/* On a VGA dongle there is no bridge chip, so stand up our own connector for the chip's
 * integrated DAC and hand it to the pipe. Must happen before the pipe is initialised,
 * which is what attaches the connector to the encoder
 */
	if (fl2000_vga_mode) {
		drm_connector_helper_add(&drm_if->connector, &fl2000_connector_helper_funcs);

		/* No HPD line on VGA, so rely on periodic polling of the DDC bus */
		drm_if->connector.polled =
			DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;

		ret = drm_connector_init(drm, &drm_if->connector, &fl2000_connector_funcs,
					 DRM_MODE_CONNECTOR_VGA);
		if (ret) {
			dev_err(drm->dev, "Cannot initialize VGA connector (%d)", ret);
			goto error;
		}

		/* Pin the name drm_connector_init() just generated. It is freed with
		 * kfree() by drm_connector_cleanup(), so kfree/kstrdup is the pairing
		 * this needs
		 */
		if (strcmp(drm_if->connector.name, FL2000_VGA_CONNECTOR_NAME)) {
			dev_info(drm->dev, "VGA connector: kernel named it %s, pinning to %s",
				 drm_if->connector.name, FL2000_VGA_CONNECTOR_NAME);

			kfree(drm_if->connector.name);
			drm_if->connector.name = kstrdup(FL2000_VGA_CONNECTOR_NAME,
							GFP_KERNEL);
			if (!drm_if->connector.name) {
				dev_err(drm->dev, "Cannot pin VGA connector name");
				ret = -ENOMEM;
				goto error;
			}
		}
	}

	ret = drm_simple_display_pipe_init(drm, &drm_if->pipe, &fl2000_display_funcs,
					   fl2000_pixel_formats, ARRAY_SIZE(fl2000_pixel_formats),
					   NULL, fl2000_vga_mode ? &drm_if->connector : NULL);
	if (ret) {
		dev_err(drm->dev, "Cannot configure simple display pipe (%d)", ret);
		goto error;
	}

	/* Register 'mode_set' function to operate prior to bridge */
	drm_encoder_helper_add(&drm_if->pipe.encoder, &fl2000_encoder_funcs);

	/* Start streaming interface */
	drm_if->stream = fl2000_stream_create(usb_dev, &drm_if->pipe.crtc);
	if (IS_ERR(drm_if->stream)) {
		ret = PTR_ERR(drm_if->stream);
		drm_if->stream = NULL;
		dev_err(drm->dev, "Cannot create streaming interface (%d)", ret);
		goto error;
	}

	/* Start interrupts interface */
	drm_if->intr = fl2000_intr_create(usb_dev, drm);
	if (IS_ERR(drm_if->intr)) {
		ret = PTR_ERR(drm_if->intr);
		drm_if->intr = NULL;
		dev_err(drm->dev, "Cannot create interrupt interface (%d)", ret);
		goto error;
	}

	/* Attach bridge. In VGA mode there is no component to bind - the connector created above
	 * stands in for it
	 */
	if (!fl2000_vga_mode) {
		ret = component_bind_all(master, &drm_if->pipe);
		if (ret) {
			dev_err(drm->dev, "Cannot attach bridge (%d)", ret);
			goto error;
		}
		bridge_bound = true;
	}

	drm_mode_config_reset(drm);

	ret = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (ret) {
		dev_err(drm->dev, "Failed to initialize %d VBLANK(s) (%d)",
			drm->mode_config.num_crtc, ret);
		goto error;
	}

	drm_kms_helper_poll_init(drm);
	poll_initialized = true;

	drm_plane_enable_fb_damage_clips(&drm_if->pipe.plane);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		dev_err(drm->dev, "Cannot register DRM device (%d)", ret);
		goto error;
	}

	fl2000_reset(usb_dev);
	fl2000_usb_magic(usb_dev);

	drm_client_setup_with_fourcc(drm, DRM_FORMAT_XRGB8888);

	return 0;

error:
	/* The stream and interrupt contexts are devres-managed on &usb_dev->dev, which outlives
	 * this module: a bind failure must release them explicitly or they leak until replug
	 * (and keep the next probe from re-initialising the chip). The DRM device and its
	 * mode config are devm/drmm-managed on master and released by the driver core
	 */
	if (poll_initialized)
		drm_kms_helper_poll_fini(drm);
	if (bridge_bound)
		component_unbind_all(master, drm);
	if (drm_if->intr) {
		fl2000_intr_destroy(usb_dev);
		drm_if->intr = NULL;
	}
	if (drm_if->stream) {
		fl2000_stream_destroy(usb_dev);
		drm_if->stream = NULL;
	}
	dev_set_drvdata(master, NULL);

	return ret;
}

void fl2000_drm_unbind(struct device *master)
{
	struct fl2000_drm_if *drm_if = dev_get_drvdata(master);

	/* This used to devres_release() fl2000_drm_if_release from &usb_dev->dev, but that
	 * callback was never registered as a devres, so the release silently did nothing and the
	 * DRM device was never torn down
	 */
	if (!drm_if)
		return;

	dev_set_drvdata(master, NULL);

	fl2000_drm_if_release(drm_if, master);
}

/* System sleep. drm_mode_config_helper_suspend() runs a disable-all atomic commit, which
 * calls the pipe's .disable -> fl2000_stream_disable(), so streaming and the stall watchdog
 * stop cleanly instead of fighting the frozen USB device
 */
int fl2000_drm_suspend(struct device *master)
{
	struct fl2000_drm_if *drm_if = dev_get_drvdata(master);

	if (!drm_if)
		return 0;

	return drm_mode_config_helper_suspend(&drm_if->drm);
}

int fl2000_drm_resume(struct device *master)
{
	struct fl2000_drm_if *drm_if = dev_get_drvdata(master);
	int ret;

	if (!drm_if)
		return 0;

	/* The device may have been reset across suspend, which restores altsetting 0: the
	 * streaming interface needs altsetting 1 back before any bulk URB is submitted
	 */
	ret = usb_set_interface(drm_if->usb_dev, FL2000_USBIF_AVCONTROL, 1);
	if (ret) {
		dev_err(master, "Cannot restore streaming altsetting on resume (%d)", ret);
		return ret;
	}

	/* Reprogram the saved mode (the helper re-runs mode_set + enable), then restore the
	 * detect/LPM magic that fl2000_reset() inside mode_set wipes
	 */
	ret = drm_mode_config_helper_resume(&drm_if->drm);

	fl2000_usb_magic(drm_if->usb_dev);

	/* A device reset across suspend aborts the interrupt URB; restart it if so */
	fl2000_intr_restart(drm_if->usb_dev);

	return ret;
}

/* In-place USB device reset hooks, wired to the USB driver's pre_reset/post_reset.
 *
 * They exist because of a hard freeze (2026-09-10): usb_reset_device() calls
 * usb_forced_unbind_intf() on every bound interface unless the driver provides BOTH
 * pre_reset and post_reset. The forced unbind ran fl2000_disconnect() ->
 * fl2000_drm_unbind() -> drm_atomic_helper_shutdown() -> fl2000_display_disable() ->
 * fl2000_stream_disable() from inside the stall watchdog's own work item, and that
 * function's cancel_delayed_work_sync(&watchdog_work) then waited on the work item
 * currently executing it. The worker never returned, kept holding the USB device lock,
 * blocked hub_event in usb_disconnect(), and the machine froze with no panic and no
 * kdump. With both callbacks the reset is performed in place instead of force-unbinding.
 */
int fl2000_drm_pre_reset(struct device *master)
{
	struct fl2000_drm_if *drm_if = dev_get_drvdata(master);

	if (!drm_if)
		return 0;

	/* No URB may be submitted while the device is being reset */
	fl2000_stream_disable(drm_if->stream);

	return 0;
}

int fl2000_drm_post_reset(struct device *master)
{
	struct fl2000_drm_if *drm_if = dev_get_drvdata(master);

	if (!drm_if)
		return 0;

	/* The reset wiped every register and restored altsetting 0: reprogram the saved
	 * mode and restart the interrupt URB the reset aborted. The streaming session
	 * itself is re-kicked by the stall watchdog that requested the reset.
	 */
	if (drm_if->mode_config_valid)
		fl2000_stream_reprogram(drm_if);

	fl2000_intr_restart(drm_if->usb_dev);

	return 0;
}
