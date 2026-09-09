// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2019, Artem Mygaiev
 */

#include "fl2000.h"

#define CONTROL_MSG_READ  64
#define CONTROL_MSG_WRITE 65

#define FL2000_HW_RST_MDELAY 10

static bool fl2000_reg_precious(struct device *dev, unsigned int reg)
{
	UNUSED(dev);
	return FL2000_REG_PRECIOUS(reg);
}

static bool fl2000_reg_volatile(struct device *dev, unsigned int reg)
{
	UNUSED(dev);
	return FL2000_REG_VOLATILE(reg);
}

/* Protected by internal regmap mutex */
static int fl2000_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	int ret;
	u32 *usb_rw_data;
	struct usb_device *usb_dev = context;
	u16 offset = (u16)reg;

	usb_rw_data = kmalloc(sizeof(*usb_rw_data), GFP_KERNEL | GFP_DMA);
	if (!usb_rw_data)
		return -ENOMEM;

	ret = usb_control_msg(usb_dev, usb_rcvctrlpipe(usb_dev, 0), CONTROL_MSG_READ,
			      (USB_DIR_IN | USB_TYPE_VENDOR), 0, offset, usb_rw_data, sizeof(u32),
			      USB_CTRL_GET_TIMEOUT);
	if (ret > 0) {
		if (ret != sizeof(u32))
			ret = -1;
		else
			ret = 0;
	}

	*val = *usb_rw_data;

	kfree(usb_rw_data);
	return ret;
}

/* Protected by internal regmap mutex */
static int fl2000_reg_write(void *context, unsigned int reg, unsigned int val)
{
	int ret;
	u32 *usb_rw_data;
	struct usb_device *usb_dev = context;
	u16 offset = (u16)reg;

	usb_rw_data = kmalloc(sizeof(*usb_rw_data), GFP_KERNEL | GFP_DMA);
	if (!usb_rw_data)
		return -ENOMEM;

	*usb_rw_data = val;

	ret = usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0), CONTROL_MSG_WRITE,
			      (USB_DIR_OUT | USB_TYPE_VENDOR), 0, offset, usb_rw_data, sizeof(u32),
			      USB_CTRL_SET_TIMEOUT);
	if (ret > 0) {
		if (ret != sizeof(u32))
			ret = -1;
		else
			ret = 0;
	}

	kfree(usb_rw_data);
	return ret;
}

/* We do not use default values as per documentation because:
 *  a) somehow they differ from real HW
 *  b) on SW reset not all of them are cleared
 */
static const struct regmap_config fl2000_regmap_config = {
	.val_bits = 32,
	.reg_bits = 16,
	.reg_stride = 4,
	.max_register = 0xFFFF,

	.cache_type = REGCACHE_RBTREE,

	.precious_reg = fl2000_reg_precious,
	.volatile_reg = fl2000_reg_volatile,

	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,

	.reg_read = fl2000_reg_read,
	.reg_write = fl2000_reg_write,

	.use_single_read = true,
	.use_single_write = true,
};

int fl2000_set_pll(struct usb_device *usb_dev, struct fl2000_pll *pll)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_pll_reg pll_reg = { .val = 0 };
	union fl2000_vga_ctrl_reg_aclk aclk = { .val = 0 };
	u32 mask = 0;

	pll_reg.prescaler = pll->prescaler;
	pll_reg.multiplier = pll->multiplier;
	pll_reg.divisor = pll->divisor;
	pll_reg.function = pll->function;
	regmap_write(regmap, FL2000_VGA_PLL_REG, pll_reg.val);

	aclk.force_pll_up = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, force_pll_up);
	aclk.force_vga_connect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, force_vga_connect);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);

	return 0;
}

int fl2000_set_timings(struct usb_device *usb_dev, struct fl2000_timings *timings)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_hsync_reg1 hsync1 = { .val = 0 };
	union fl2000_vga_hsync_reg2 hsync2 = { .val = 0 };
	union fl2000_vga_vsync_reg1 vsync1 = { .val = 0 };
	union fl2000_vga_vsync_reg2 vsync2 = { .val = 0 };

	hsync1.hactive = timings->hactive;
	hsync1.htotal = timings->htotal;
	regmap_write(regmap, FL2000_VGA_HSYNC_REG1, hsync1.val);

	hsync2.hsync_width = timings->hsync_width;
	hsync2.hstart = timings->hstart;
	regmap_write(regmap, FL2000_VGA_HSYNC_REG2, hsync2.val);

	vsync1.vactive = timings->vactive;
	vsync1.vtotal = timings->vtotal;
	regmap_write(regmap, FL2000_VGA_VSYNC_REG1, vsync1.val);

	vsync2.vsync_width = timings->vsync_width;
	vsync2.vstart = timings->vstart;
	vsync2.start_latency = timings->vstart;
	regmap_write(regmap, FL2000_VGA_VSYNC_REG2, vsync2.val);

	return 0;
}

int fl2000_set_pixfmt(struct usb_device *usb_dev, u32 bytes_pix)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_cntrl_reg_pxclk pxclk = { .val = 0 };
	u32 mask = 0;

	pxclk.dac_output_en = false;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, dac_output_en);
	pxclk.drop_cnt = false;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, drop_cnt);
	pxclk.vga565_mode = (bytes_pix == 2);
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, vga565_mode);
	pxclk.vga332_mode = false;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, vga332_mode);
	pxclk.vga555_mode = false;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, vga555_mode);
	pxclk.vga_compress = false;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, vga_compress);
	pxclk.dac_output_en = true;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, dac_output_en);
	pxclk.clear_watermark = true;
	fl2000_add_bitmask(mask, union fl2000_vga_cntrl_reg_pxclk, clear_watermark);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_PXCLK, mask, pxclk.val);

	return 0;
}

int fl2000_set_transfers(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_ctrl_reg_aclk aclk = { .val = 0 };
	union fl2000_vga_isoch_reg isoch = { .val = 0 };
	u32 mask;

	mask = 0;
	aclk.use_pkt_pending = false;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, use_pkt_pending);
	aclk.use_zero_td = false;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, use_zero_td);
	aclk.use_zero_pkt_len = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, use_zero_pkt_len);
	aclk.vga_err_int_en = true;
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);

	mask = 0;
	isoch.mframe_cnt = 0;
	fl2000_add_bitmask(mask, union fl2000_vga_isoch_reg, mframe_cnt);
	regmap_write_bits(regmap, FL2000_VGA_ISOCH_REG, mask, isoch.val);

	return 0;
}

int fl2000_reset(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_rst_cntrl_reg rst_cntrl_reg = { .val = 0 };
	u32 mask = 0;

	rst_cntrl_reg.sw_reset = true;
	fl2000_add_bitmask(mask, union fl2000_rst_cntrl_reg, sw_reset);
	regmap_write_bits(regmap, FL2000_RST_CTRL_REG, mask, rst_cntrl_reg.val);

	msleep(FL2000_HW_RST_MDELAY);

	return 0;
}

int fl2000_afe_magic(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_usb_lpm_reg usb_lpm_reg = { .val = 0 };
	u32 mask = 0;

	usb_lpm_reg.magic = true;
	fl2000_add_bitmask(mask, union fl2000_usb_lpm_reg, magic);
	regmap_write_bits(regmap, FL2000_USB_LPM_REG, mask, usb_lpm_reg.val);

	return 0;
}

int fl2000_usb_magic(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_i2c_sc_reg vga_i2c_sc_reg = { .val = 0 };
	union fl2000_vga_ctrl_reg_3 vga_ctrl_reg_3 = { .val = 0 };
	union fl2000_usb_lpm_reg usb_lpm_reg = { .val = 0 };
	union fl2000_usb_ctrl_reg usb_ctrl_reg = { .val = 0 };
	u32 mask;

	mask = 0;
	vga_i2c_sc_reg.monitor_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, monitor_detect);
	vga_i2c_sc_reg.edid_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, edid_detect);
	regmap_write_bits(regmap, FL2000_VGA_I2C_SC_REG, mask, vga_i2c_sc_reg.val);

	mask = 0;
	vga_ctrl_reg_3.wakeup_clr_en = false;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_3, wakeup_clr_en);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_3, mask, vga_ctrl_reg_3.val);

	mask = 0;
	usb_lpm_reg.u1_reject = true;
	fl2000_add_bitmask(mask, union fl2000_usb_lpm_reg, u1_reject);
	usb_lpm_reg.u2_reject = true;
	fl2000_add_bitmask(mask, union fl2000_usb_lpm_reg, u2_reject);
	regmap_write_bits(regmap, FL2000_USB_LPM_REG, mask, usb_lpm_reg.val);

	mask = 0;
	usb_ctrl_reg.wake_nrdy = false;
	fl2000_add_bitmask(mask, union fl2000_usb_ctrl_reg, wake_nrdy);
	regmap_write_bits(regmap, FL2000_USB_CTRL_REG, mask, usb_ctrl_reg.val);

	return 0;
}

/* Reference-order detect enable (FrescoLogic fl2000_dongle_i2c_detection_enable): the EDID/I2C
 * detection circuit is switched on by itself and given 750 ms to settle before external monitor
 * detect joins in usb_magic(). Our usual init writes both bits in one shot with no settle time
 */
int fl2000_ddc_detect_enable_staged(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_i2c_sc_reg reg = { .val = 0 };
	u32 mask = 0;
	int ret;

	reg.edid_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, edid_detect);
	ret = regmap_write_bits(regmap, FL2000_VGA_I2C_SC_REG, mask, reg.val);
	if (ret)
		return ret;

	/* DELAY_FOR_I2C_CONNECTION_ENABLE in the reference driver */
	msleep(750);

	return 0;
}

/* Read and decode the chip's own idea of what is connected. Field names follow the reference
 * driver's struct vga_status (fl2000_interrupt.h); bit 0 is the overall connect status that
 * gates the reference driver's EDID read
 */
void fl2000_log_vga_status(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_status_reg status;
	int ret;

	ret = regmap_read(regmap, FL2000_VGA_STATUS_REG, &status.val);
	if (ret) {
		dev_err(&usb_dev->dev, "VGA status read failed (%d)", ret);
		return;
	}

	dev_info(&usb_dev->dev,
		 "VGA status 0x%08x: connected %u edid_connected %u ext_mon_connected %u pll_up %u dac_up %u",
		 status.val, status.vga_status, status.edid_status, status.monitor_status,
		 status.pll_status, status.dac_status);
}

/* Raw dump of the VGA register block plus the two USB-block registers the drivers touch. Uses
 * the usb_control_msg backend directly, NOT regmap_read: most of these registers are cached by
 * regmap, and a cached read would show our writes rather than the hardware's live state. Meant
 * for diffing chip state against a physical change (cable attached vs detached) to locate live
 * detect/status bits without trusting either driver's struct layouts
 */
void fl2000_dump_regs(struct usb_device *usb_dev)
{
	static const u16 usb_regs[] = { 0x0070, 0x0078 };
	unsigned int val;
	u16 reg;

	for (int i = 0; i < ARRAY_SIZE(usb_regs); i++) {
		if (fl2000_reg_read(usb_dev, usb_regs[i], &val))
			continue;
		dev_info(&usb_dev->dev, "regdump %04x: %08x", usb_regs[i], val);
	}

	for (reg = 0x8000; reg < 0x8100; reg += 16) {
		unsigned int v[4] = { 0 };

		for (int i = 0; i < 4; i++)
			if (fl2000_reg_read(usb_dev, reg + i * 4, &v[i]))
				v[i] = 0xdeaddead;

		dev_info(&usb_dev->dev, "regdump %04x: %08x %08x %08x %08x", reg, v[0], v[1],
			 v[2], v[3]);
	}
}

/* Live connector presence sense. Register 0x805C is modelled by neither vendor driver but was
 * found by raw-diffing chip state against physical cable changes (NOTES.md "Round 4"): it reads
 * zero with the VGA cable detached and a nonzero analog-sense pattern (e.g. 0x00ffecbb) with a
 * monitor attached, tracking the cable exactly and staying stable within a state. Read through
 * the raw backend on purpose - the register is not in the regmap volatile list, so a regmap_read
 * would return a stale cached value.
 *
 * Returns 1 if a sink is sensed, 0 if not, negative errno on transfer failure
 */
int fl2000_connector_sense(struct usb_device *usb_dev)
{
	unsigned int val = 0;
	int ret;

	ret = fl2000_reg_read(usb_dev, 0x805C, &val);
	if (ret)
		return ret;

	return val != 0;
}

/* Re-arm the EDID/monitor detect block (incident #3). The chip's detect circuit can stop
 * reporting the attached monitor mid-session; the same two enable bits that fl2000_usb_magic()
 * sets are rewritten here to try to revive it before the connector falls back to the sticky
 * latch. Bounded, no reset and no timing changes, so it is safe to call from the output poll.
 * Returns 0 on success (the bits were written), negative errno on transfer failure.
 */
int fl2000_detect_rearm(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_i2c_sc_reg reg = { .val = 0 };
	u32 mask = 0;

	reg.monitor_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, monitor_detect);
	reg.edid_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, edid_detect);

	return regmap_write_bits(regmap, FL2000_VGA_I2C_SC_REG, mask, reg.val);
}

/* Round 5 experiments: values below are VERBATIM from the Windows USBPcap capture
 * (~/src/fl2000/win-capture.pcapng, NOTES.md "Round 5") of this exact dongle+monitor working.
 * Not decoded through our field unions on purpose - the point is replaying known-good state
 */

/* ddc_step=3: the 0x803C sequence Windows performs around its modeset (e4 -> c4 -> d5).
 * Bit 26 is "force PLL always on"; if a clock domain feeding the DDC/detect block hangs off
 * these bits, this alone may unlock DDC without full video timings
 */
int fl2000_ddc_step_aclk(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	int ret;

	ret = regmap_write(regmap, 0x803C, 0xe401084d);
	if (ret)
		return ret;
	ret = regmap_write(regmap, 0x803C, 0xc401084d);
	if (ret)
		return ret;
	return regmap_write(regmap, 0x803C, 0xd501084d);
}

/* ddc_step=4: full replay of the captured Windows 1024x768@60 modeset, same order. Brings the
 * output stage up for real: PLL locked, DAC on, sync toward the monitor. Tests the leading
 * theory that DDC (monitor-side or dongle-side) only powers up once video is active. No pixel
 * data is streamed - the timing generator runs autonomously, the screen shows black/no-image
 */
int fl2000_ddc_step_modeset(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	static const struct {
		u16 reg;
		u32 val;
	} seq[] = {
		{ 0x803C, 0xe401084d }, /* +force PLL always on */
		{ 0x802C, 0x000d2102 }, /* PLL word for 65 MHz pixel clock */
		{ 0x8048, 0x00008004 }, /* app reset */
		{ 0x803C, 0xc401084d },
		{ 0x803C, 0xd501084d },
		{ 0x8004, 0x0010031c }, /* DAC off, colour modes cleared */
		{ 0x8004, 0x0010039d }, /* DAC on + clear watermark */
		{ 0x8008, 0x04000540 }, /* hactive 1024 | htotal 1344 */
		{ 0x800C, 0x00880129 }, /* hsync width/start */
		{ 0x8010, 0x03000326 }, /* vactive 768 | vtotal 806 */
		{ 0x8014, 0x02460024 }, /* vsync width/start */
		{ 0x801C, 0x00000000 }, /* ISO bits cleared */
	};
	int ret;

	for (int i = 0; i < ARRAY_SIZE(seq); i++) {
		ret = regmap_write(regmap, seq[i].reg, seq[i].val);
		if (ret) {
			dev_err(&usb_dev->dev, "modeset replay: write %04x failed (%d)",
				seq[i].reg, ret);
			return ret;
		}
		if (seq[i].reg == 0x8048)
			msleep(FL2000_HW_RST_MDELAY);
	}

	dev_info(&usb_dev->dev, "modeset replay done: 1024x768@60 sync should be live");
	return 0;
}

int fl2000_enable_interrupts(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_ctrl_reg_aclk aclk = { .val = 0 };
	union fl2000_vga_ctrl2_reg_axclk axclk = { .val = 0 };
	u32 mask;

	mask = 0;
	aclk.vga_err_int_en = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, vga_err_int_en);
	aclk.lbuf_err_int_en = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, lbuf_err_int_en);
	aclk.edid_mon_int_en = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, edid_mon_int_en);
	aclk.edid_mon_int_en = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, edid_mon_int_en);
	aclk.feedback_int_en = false;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl_reg_aclk, feedback_int_en);
	regmap_write_bits(regmap, FL2000_VGA_CTRL_REG_ACLK, mask, aclk.val);

	mask = 0;
	axclk.hdmi_int_en = true;
	fl2000_add_bitmask(mask, union fl2000_vga_ctrl2_reg_axclk, hdmi_int_en);
	regmap_write_bits(regmap, FL2000_VGA_CTRL2_REG_ACLK, mask, axclk.val);

	return 0;
}

int fl2000_check_interrupt(struct usb_device *usb_dev)
{
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);
	union fl2000_vga_status_reg status;
	int ret;
	int sink_event = 0;
	u32 mask = 0;

	/* Process interrupt */
	ret = regmap_read(regmap, FL2000_VGA_STATUS_REG, &status.val);
	if (ret)
		return 0; /* XXX: Cannot report error here */

	if (status.hdmi_event || status.monitor_event || status.edid_event)
		sink_event = 1;

	/* LBUF issues are recoverable */
	if (status.lbuf_overflow)
		fl2000_add_bitmask(mask, union fl2000_vga_status_reg, lbuf_overflow);
	if (status.lbuf_underflow)
		fl2000_add_bitmask(mask, union fl2000_vga_status_reg, lbuf_underflow);
	regmap_write_bits(regmap, FL2000_VGA_STATUS_REG, mask, status.val);

	/* TODO: Reset LBUF using regmap_field if (status.lbuf_halt) */

	/* TODO: Don't know how to recover if (status.vga_error) */

	return sink_event;
}

int fl2000_i2c_dword(struct usb_device *usb_dev, bool read, u16 addr, u8 offset, u32 *data)
{
	int ret;
	union fl2000_vga_i2c_sc_reg reg = { .val = 0 };
	u32 mask = 0;
	struct regmap *regmap = dev_get_regmap(&usb_dev->dev, NULL);

	if (!read) {
		ret = regmap_write(regmap, FL2000_VGA_I2C_WR_REG, *data);
		if (ret)
			return -EIO;
	}

	/* This bit always reads back as 0, so we need to restore it back. Though not quite
	 * sure if we need to enable monitor detection circuit for HDMI use case
	 */
	reg.monitor_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, monitor_detect);
	reg.edid_detect = true;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, edid_detect);

	/* Force an EEPROM (I2C) operation. These bits are not self-clearing and were previously
	 * left untouched by the write mask, so a stale SPI selection would silently divert every
	 * transfer away from the DDC bus. The reference driver clears both on every transfer
	 */
	reg.i2c_spi_op = false;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_spi_op);
	reg.i2c_spi_erase = false;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_spi_erase);

	reg.i2c_status = 0;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_status);
	reg.i2c_addr = addr;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_addr);
	reg.i2c_cmd = read;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_cmd);
	reg.i2c_offset = offset;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_offset);
	reg.i2c_done = false;
	fl2000_add_bitmask(mask, union fl2000_vga_i2c_sc_reg, i2c_done);

	{
		u32 before = 0;

		regmap_read(regmap, FL2000_VGA_I2C_SC_REG, &before);
		dev_dbg(&usb_dev->dev,
			"i2c %s addr 0x%02x off 0x%02x: pre 0x%08x mask 0x%08x val 0x%08x",
			read ? "rd" : "wr", addr, offset, before, mask, reg.val);
	}

	ret = regmap_write_bits(regmap, FL2000_VGA_I2C_SC_REG, mask, reg.val);
	if (ret)
		return -EIO;

	ret = regmap_read_poll_timeout(regmap, FL2000_VGA_I2C_SC_REG, reg.val, reg.i2c_done,
				       I2C_RDWR_INTERVAL, I2C_RDWR_TIMEOUT);
	/* This shouldn't normally happen: there's internal 256ms HW timeout on I2C operations and
	 * USB must be always available so no I/O errors. But if it happens we are probably in
	 * irreversible HW issue
	 */
	if (ret) {
		dev_err(&usb_dev->dev,
			"i2c %s addr 0x%02x off 0x%02x timed out: post 0x%08x (done %u status 0x%x ready %u mon_det %u edid_det %u)",
			read ? "rd" : "wr", addr, offset, reg.val, reg.i2c_done, reg.i2c_status,
			reg.i2c_ready, reg.monitor_detect, reg.edid_detect);
		return -EIO;
	}

	/* The full-bus scan in NOTES.md ("Round 2") proved i2c_status is a reliable per-byte
	 * ACK/NAK indicator on this hardware: clean 0x0 on the one device that answers, 0xF on
	 * the 111 that do not. So treat NAK as the failure it is - detect() depends on exactly
	 * this signal, and letting NAKed reads through only returns bus garbage (0xf1). The
	 * reference driver ignores the nibble and waits only on done; it is the sloppy one here
	 */
	if (reg.i2c_status) {
		dev_dbg(&usb_dev->dev, "i2c %s addr 0x%02x off 0x%02x: NAK (status 0x%x)",
			read ? "rd" : "wr", addr, offset, reg.i2c_status);
		return -EIO;
	}

	dev_dbg(&usb_dev->dev, "i2c %s addr 0x%02x off 0x%02x ok: post 0x%08x", read ? "rd" : "wr",
		addr, offset, reg.val);

	if (read) {
		ret = regmap_read(regmap, FL2000_VGA_I2C_RD_REG, data);
		if (ret)
			return -EIO;
	}

	return 0;
}

/* Not devm_ managed for the same reason as the i2c adapter: devres on &usb_dev->dev outlives this
 * module, so the regmap (and its ops, which live in module text) would survive rmmod. Freed
 * explicitly via fl2000_regmap_cleanup() from the disconnect path
 */
struct regmap *fl2000_regmap_init(struct usb_device *usb_dev)
{
	struct regmap *regmap;

	regmap = regmap_init(&usb_dev->dev, NULL, usb_dev, &fl2000_regmap_config);
	if (IS_ERR(regmap))
		dev_err(&usb_dev->dev, "Registers map failed (%ld)", PTR_ERR(regmap));

	return regmap;
}

void fl2000_regmap_cleanup(struct regmap *regmap)
{
	if (!IS_ERR_OR_NULL(regmap))
		regmap_exit(regmap);
}
