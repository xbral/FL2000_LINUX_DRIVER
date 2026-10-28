/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#ifndef __FL2000_DRM_H__
#define __FL2000_DRM_H__

#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/i2c.h>
#include <linux/component.h>
#include <linux/regmap.h>
#include <linux/vmalloc.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/time.h>
#include <linux/device.h>
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <drm/drm_vblank.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_damage_helper.h>

#include "fl2000_registers.h"

#define UNUSED(x) ((void)(x))

/* Known USB interfaces of FL2000 */
enum fl2000_interface {
	FL2000_USBIF_AVCONTROL = 0,
	FL2000_USBIF_STREAMING = 1,
	FL2000_USBIF_INTERRUPT = 2,
};

/**
 * fl2000_add_bitmask - Set bitmask for structure field
 *
 * @__mask: Variable to set mask to (assumed u32)
 * @__type: Structure type to use with bitfield (assumed size equal to u32)
 * @__field: Field to set mask for in the '__type' structure
 *
 * Sets bits to 1 in '__mask' variable that correspond to field '__field' of
 * structure type '__type'. Tested only with u32 data types
 */
#define fl2000_add_bitmask(__mask, __type, __field) \
	({                                          \
		union {                             \
			__type __umask;             \
			typeof(__mask) __val;       \
		} __aligned(4) __data;              \
		__data.__umask.__field = ~0;        \
		(__mask) |= __data.__val;           \
	})

static inline int fl2000_submit_urb(struct urb *urb, gfp_t mem_flags)
{
	int ret;
	int attempts = 10;

	do {
		ret = usb_submit_urb(urb, mem_flags);
		switch (ret) {
		case -ENXIO:
		case -ENOMEM:
			/* Retry, but never yield from interrupt context: URB completion
			 * callbacks resubmit through here in hard IRQ, where cond_resched()
			 * would BUG ("scheduling while atomic")
			 */
			if (attempts-- && !in_interrupt()) {
				cond_resched();
				ret = -EAGAIN;
			}
			break;
		default:
			break;
		}
	} while (ret == -EAGAIN);

	return ret;
}

static inline int fl2000_urb_status(struct usb_device *usb_dev, int status, int pipe)
{
	int ret = status;

	switch (status) {
	case 0: /* Success - not an error, do not log it as one */
		break;
	/* Stalled endpoint */
	case -EPIPE:
		ret = usb_clear_halt(usb_dev, pipe);
		break;
	case -ECONNRESET:
		fallthrough;
	case -ENOENT:
		fallthrough;
	case -ESHUTDOWN:
		/* Not an error */
		break;
	default:
		/* Ratelimited: a wedged streaming engine can complete thousands of
		 * URBs per second with -EPROTO, and a synchronous dev_err() per
		 * completion floods the log while the watchdog is trying to run.
		 */
		dev_err_ratelimited(&usb_dev->dev, "Nonzero urb status, %d\n", status);
		break;
	}

	return ret;
}

struct fl2000_timings {
	u32 hactive;
	u32 htotal;
	u32 hsync_width;
	u32 hstart;
	u32 vactive;
	u32 vtotal;
	u32 vsync_width;
	u32 vstart;
};

struct fl2000_pll {
	u32 prescaler;
	u32 multiplier;
	u32 divisor;
	u32 function;
};

/* Timeout in us for I2C read/write operations. Cada poll é um USB control
 * transfer (~1 ms); 200 us de intervalo inundava o endpoint de controle e
 * travava a máquina durante EDID (32 dwords x até 1280 polls). 1 ms / 100 ms
 * corta o pior caso em ~5x e continua folgado p/ EEPROM (ACK em <10 ms).
 */
#define I2C_RDWR_INTERVAL (1000)
#define I2C_RDWR_TIMEOUT  (100 * 1000)

/* Streaming transfer task */
struct fl2000_stream;
struct fl2000_stream *fl2000_stream_create(struct usb_device *usb_dev, struct drm_crtc *crtc);
void fl2000_stream_destroy(struct usb_device *usb_dev);

/* Streaming interface */
int fl2000_stream_mode_set(struct fl2000_stream *stream, int pixels, u32 bytes_pix);
void fl2000_stream_compress(struct fl2000_stream *stream, void *src, unsigned int height,
			    unsigned int width, unsigned int pitch);
int fl2000_stream_enable(struct fl2000_stream *stream);
void fl2000_stream_disable(struct fl2000_stream *stream);

/* Interrupt polling task */
struct fl2000_intr;
struct fl2000_intr *fl2000_intr_create(struct usb_device *usb_dev, struct drm_device *drm);
void fl2000_intr_destroy(struct usb_device *usb_dev);
void fl2000_intr_restart(struct usb_device *usb_dev);

/* I2C adapter interface creation */
struct i2c_adapter *fl2000_i2c_init(struct usb_device *usb_dev);
void fl2000_i2c_cleanup(struct i2c_adapter *adapter);

/* Register map creation */
struct regmap *fl2000_regmap_init(struct usb_device *usb_dev);
void fl2000_regmap_cleanup(struct regmap *regmap);

/* Re-arm the chip's EDID/monitor detect circuit. Used by the VGA connector detect path to try
 * to revive a detect block that died mid-session before falling back to the sticky latch.
 * Defined in fl2000_registers.c
 */
int fl2000_detect_rearm(struct usb_device *usb_dev);

/* Registers interface */
int fl2000_reset(struct usb_device *usb_dev);
int fl2000_usb_magic(struct usb_device *usb_dev);
int fl2000_afe_magic(struct usb_device *usb_dev);
int fl2000_set_transfers(struct usb_device *usb_dev);
int fl2000_set_pixfmt(struct usb_device *usb_dev, u32 bytes_pix);
int fl2000_set_timings(struct usb_device *usb_dev, struct fl2000_timings *timings);
int fl2000_set_pll(struct usb_device *usb_dev, struct fl2000_pll *pll);
int fl2000_connector_sense(struct usb_device *usb_dev);
int fl2000_ddc_detect_enable_staged(struct usb_device *usb_dev);
int fl2000_ddc_step_aclk(struct usb_device *usb_dev);
int fl2000_ddc_step_modeset(struct usb_device *usb_dev);
int fl2000_log_vga_status(struct usb_device *usb_dev);
void fl2000_dump_regs(struct usb_device *usb_dev);
int fl2000_enable_interrupts(struct usb_device *usb_dev);
int fl2000_check_interrupt(struct usb_device *usb_dev);
int fl2000_i2c_dword(struct usb_device *usb_dev, bool read, u16 addr, u8 offset, u32 *data);

/* I2C address of the monitor's DDC/EDID EEPROM, reached over the FL2000's own I2C master */
#define FL2000_DDC_ADDR 0x50

/* Drive the integrated VGA DAC directly instead of waiting for an external bridge component.
 * Defined in fl2000_drv.c
 */
extern bool fl2000_vga_mode;

/* Diagnostic: stream solid white instead of framebuffer content. Defined in fl2000_drv.c */
extern bool fl2000_test_fill;

/* Force bytes-per-pixel for every mode: 0 = auto (default), 2 = RGB565, 3 = RGB888.
 * Defined in fl2000_drv.c
 */
extern int fl2000_force_bpp;

/* Once a monitor has been seen, never report the VGA connector as disconnected.
 * Defined in fl2000_drv.c
 */
extern bool fl2000_sticky_connect;

/* DRM device creation */
int fl2000_drm_bind(struct device *master);
void fl2000_drm_unbind(struct device *master);

/* System sleep hooks for the DRM pipeline. Called from the USB interface suspend/resume
 * callbacks (fl2000_drv.c). Defined in fl2000_drm.c
 */
int fl2000_drm_suspend(struct device *master);
int fl2000_drm_resume(struct device *master);

/* In-place USB device reset hooks, wired to the USB driver's pre_reset/post_reset.
 * Keeping both callbacks stops usb_reset_device() from force-unbinding the interface
 * (whose DRM shutdown deadlocks inside the stall watchdog) and lets the reset be
 * recovered in place. Defined in fl2000_drm.c
 */
int fl2000_drm_pre_reset(struct device *master);
int fl2000_drm_post_reset(struct device *master);

#endif /* __FL2000_DRM_H__ */
