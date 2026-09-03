// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

#define USB_DRIVER_NAME "fl2000_usb"

#define USB_CLASS_AV		0x10
#define USB_SUBCLASS_AV_CONTROL 0x01
#define USB_SUBCLASS_AV_VIDEO	0x02
#define USB_SUBCLASS_AV_AUDIO	0x03

#define USB_VENDOR_FRESCO_LOGIC 0x1D5C
#define USB_PRODUCT_FL2000	0x2000

#define FL2000_ALL_IFS \
	(BIT(FL2000_USBIF_AVCONTROL) | BIT(FL2000_USBIF_STREAMING) | BIT(FL2000_USBIF_INTERRUPT))

bool fl2000_vga_mode;
module_param_named(vga_mode, fl2000_vga_mode, bool, 0444);
MODULE_PARM_DESC(vga_mode,
		 "Drive the FL2000 integrated VGA DAC directly (default 0). VGA dongles have no "
		 "IT66121 bridge chip, so the component match never completes and the display "
		 "pipeline never comes up; set this to bring up a VGA connector instead");

/* DDC bring-up experiments for the probe_only loop, see NOTES.md "Round 3"/"Round 5":
 * 0 = baseline init; 1 = reference-order detect enable (EDID/I2C detect bit alone, 750 ms
 * settle); 2 = step 1 plus usb_set_interface(0, 1) first (both eliminated in Round 3);
 * 3 = Windows-captured 0x803C sequence (force-PLL-on etc.); 4 = full replay of the captured
 * Windows 1024x768@60 modeset, bringing sync/DAC live to test sync-dependent DDC
 */
static int fl2000_ddc_step;
module_param_named(ddc_step, fl2000_ddc_step, int, 0444);
MODULE_PARM_DESC(ddc_step, "DDC bring-up experiment 0-4 (default 0), see NOTES.md");

/* Raw-dump the register space at the end of probe (probe_only aid, see NOTES.md) */
static bool fl2000_dump;
module_param_named(dump_regs, fl2000_dump, bool, 0444);
MODULE_PARM_DESC(dump_regs, "Dump raw register space after probe init (default 0)");

bool fl2000_test_fill;
module_param_named(test_fill, fl2000_test_fill, bool, 0644);
MODULE_PARM_DESC(test_fill,
		 "Stream solid white instead of framebuffer content (default 0). Diagnostic for "
		 "black-screen-with-locked-sync: white on screen proves the chip/DAC data path "
		 "and isolates the fault to the framebuffer copy; still-black means the chip "
		 "discards pixel data. Writable at runtime via /sys/module/fl2000/parameters");

bool fl2000_probe_only;
module_param_named(probe_only, fl2000_probe_only, bool, 0444);
MODULE_PARM_DESC(probe_only,
		 "Bring up USB, regmap and the I2C adapter but register no DRM device (default 0). "
		 "Without a card there is nothing for a compositor to open, so the module refcount "
		 "stays at zero and rmmod/insmod can be iterated freely. Intended for poking the "
		 "chip's I2C master with i2c-tools while debugging DDC");

int fl2000_force_bpp;
module_param_named(force_bpp, fl2000_force_bpp, int, 0644);
MODULE_PARM_DESC(force_bpp,
		 "Force bytes per pixel for every mode: 0 = auto (default), 2 = RGB565, 3 = RGB888. "
		 "Writable at runtime via /sys/module/fl2000/parameters; takes effect on the next "
		 "modeset. Auto uses RGB565 when the pixel clock would drain more USB bandwidth than "
		 "the bus sustains (e.g. 1920x1080@60 on USB 3.0)");

static struct usb_device_id fl2000_id_table[] = {
	{ USB_DEVICE_INTERFACE_CLASS(USB_VENDOR_FRESCO_LOGIC, USB_PRODUCT_FL2000, USB_CLASS_AV) },
	{},
};
MODULE_DEVICE_TABLE(usb, fl2000_id_table);

/* Devices that are independent of interfaces, created for the lifetime of USB device instance */
struct fl2000_devs {
	struct regmap *regmap;
	struct i2c_adapter *adapter;
	struct component_match *match;
	int active_if;
	bool master_added;
	bool drm_bound;

	/* Device the VGA-mode DRM device is bound on: the AVCONTROL usb_interface. NOT the i2c
	 * adapter: a devm DRM device parented on the adapter deadlocks disconnect, because
	 * i2c_del_adapter() wait_for_completion()s on the adapter's final release while an open
	 * compositor fd (plus the devres link itself) keeps the DRM device - and therefore the
	 * adapter - referenced. Observed as a permanently hung usb_hub_wq on hot-unplug.
	 * usb_interface device_del() has no such wait, so the standard drm_dev_unplug()
	 * lingering-fd pattern works as intended
	 */
	struct device *drm_master;
};

static struct component_master_ops fl2000_master_ops = {
	.bind = fl2000_drm_bind,
	.unbind = fl2000_drm_unbind,
};

static int fl2000_compare(struct device *dev, void *data)
{
	struct i2c_client *client = i2c_verify_client(dev);
	static const char *const fl2000_supported_bridges[] = {
		"it66121", /* IT66121 driver name*/
	};

	UNUSED(data);

	if (!client)
		return 0;

	/* Check this is a supported DRM bridge */
	for (int i = 0; i < ARRAY_SIZE(fl2000_supported_bridges); i++)
		if (!strncmp(fl2000_supported_bridges[i], client->name, sizeof(client->name)))
			return 1; /* Must be not 0 for success */

	return 0;
}

static struct fl2000_devs *fl2000_get_devices(struct usb_device *usb_dev)
{
	struct fl2000_devs *devs;

	devs = kzalloc(sizeof(*devs), GFP_KERNEL);
	if (!devs)
		return (ERR_PTR(-ENOMEM));

	devs->regmap = fl2000_regmap_init(usb_dev);
	if (IS_ERR(devs->regmap)) {
		struct regmap *regmap = devs->regmap;

		kfree(devs);
		return ERR_CAST(regmap);
	}

	devs->adapter = fl2000_i2c_init(usb_dev);
	if (IS_ERR(devs->adapter)) {
		struct i2c_adapter *adapter = devs->adapter;

		fl2000_regmap_cleanup(devs->regmap);
		kfree(devs);
		return ERR_CAST(adapter);
	}

	component_match_add(&devs->adapter->dev, &devs->match, fl2000_compare, NULL);

	dev_set_drvdata(&usb_dev->dev, devs);

	return devs;
}

/* Tears down everything fl2000_get_devices() created. None of it is devres managed on
 * &usb_dev->dev, because that is released only when the usb_device disappears - which outlives
 * this module and left freed module text reachable through a registered i2c_adapter after rmmod
 */
static void fl2000_put_devices(struct usb_device *usb_dev, struct fl2000_devs *devs)
{
	/* Stop anyone finding a stale devs on the next probe */
	dev_set_drvdata(&usb_dev->dev, NULL);

	/* Drops the component match devres along with the adapter device */
	fl2000_i2c_cleanup(devs->adapter);
	fl2000_regmap_cleanup(devs->regmap);

	kfree(devs);
}

/* TODO: Halt driver on initialization failure */
static int fl2000_probe(struct usb_interface *interface, const struct usb_device_id *usb_dev_id)
{
	int ret = 0;
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_devs *devs = dev_get_drvdata(&usb_dev->dev);

	UNUSED(usb_dev_id);

	if (usb_dev->speed < USB_SPEED_HIGH) {
		dev_err(&usb_dev->dev, "USB 1.1 is not supported!");
		return -ENODEV;
	}

	if (!devs) {
		devs = fl2000_get_devices(usb_dev);
		if (IS_ERR(devs)) {
			dev_err(&usb_dev->dev, "Cannot initialize I2C and regmap!");
			return -ENODEV;
		}
	}

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
		devs->drm_master = &interface->dev;
		fallthrough;
	case FL2000_USBIF_STREAMING:
	case FL2000_USBIF_INTERRUPT:
		devs->active_if |= BIT(iface_num);
		break;

	default: /* Device does not have any other interfaces */
		dev_warn(&interface->dev, "What interface %d?", iface_num);
		ret = -ENODEV;
		break;
	}

	/* When all interfaces are up - proceed with registration */
	if (devs->active_if == FL2000_ALL_IFS) {
		/* Bring the chip out of its power-on state before anything tries to use it. The
		 * I2C master and the monitor detect circuit are enabled by fl2000_usb_magic(), so
		 * until this runs no EDID can be read over DDC
		 */
		if (fl2000_ddc_step == 2) {
			ret = usb_set_interface(usb_dev, 0, 1);
			dev_info(&usb_dev->dev, "ddc_step2: usb_set_interface(0, 1) = %d", ret);
		}

		fl2000_reset(usb_dev);

		if (fl2000_ddc_step == 1 || fl2000_ddc_step == 2) {
			ret = fl2000_ddc_detect_enable_staged(usb_dev);
			dev_info(&usb_dev->dev, "ddc_step1: staged detect enable + 750ms = %d",
				 ret);
		}

		fl2000_usb_magic(usb_dev);

		/* Round 5 experiments (alternatives, not cumulative with steps 1/2) */
		if (fl2000_ddc_step == 3) {
			ret = fl2000_ddc_step_aclk(usb_dev);
			dev_info(&usb_dev->dev, "ddc_step3: 0x803C sequence = %d", ret);
		} else if (fl2000_ddc_step == 4) {
			ret = fl2000_ddc_step_modeset(usb_dev);
			dev_info(&usb_dev->dev, "ddc_step4: modeset replay = %d", ret);
		}

		/* Stop short of DRM. The i2c adapter is live at this point, which is all that is
		 * needed to exercise the chip's I2C master from userspace
		 */
		if (fl2000_probe_only) {
			/* What does the chip's own monitor-detect circuit see? The reference
			 * driver gates its EDID read on bit 0 of this register
			 */
			fl2000_log_vga_status(usb_dev);

			if (fl2000_dump)
				fl2000_dump_regs(usb_dev);

			dev_info(&usb_dev->dev,
				 "probe_only: I2C bus %d is live, no DRM device registered",
				 i2c_adapter_id(devs->adapter));
			return 0;
		}

		if (fl2000_vga_mode) {
			/* Nothing to wait for: a VGA dongle has no bridge chip, so bind the DRM
			 * device straight away rather than registering a component master that
			 * would never be satisfied
			 */
			ret = fl2000_drm_bind(devs->drm_master);
			if (ret) {
				dev_err(&usb_dev->dev, "Cannot bind DRM device (%d)", ret);
				return ret;
			}
			devs->drm_bound = true;
		} else {
			ret = component_master_add_with_match(&devs->adapter->dev,
							      &fl2000_master_ops, devs->match);
			if (ret) {
				dev_err(&usb_dev->dev, "Cannot register component master (%d)",
					ret);
				return ret;
			}
			devs->master_added = true;
		}
	}

	return ret;
}

static void fl2000_disconnect(struct usb_interface *interface)
{
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_devs *devs = dev_get_drvdata(&usb_dev->dev);

	if (!devs)
		return;

	if (devs->master_added) {
		component_master_del(&devs->adapter->dev, &fl2000_master_ops);
		devs->master_added = false;
	}

	if (devs->drm_bound) {
		fl2000_drm_unbind(devs->drm_master);
		devs->drm_bound = false;
	}

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
	case FL2000_USBIF_STREAMING:
	case FL2000_USBIF_INTERRUPT:
		devs->active_if &= ~BIT(iface_num);
		break;

	default: /* Device does not have any other interfaces */
		dev_warn(&interface->dev, "What interface %d?", iface_num);
		break;
	}

	/* Once the last interface is gone nothing can reach the chip any more, so drop the shared
	 * resources. Doing this here rather than leaving it to devres is what makes rmmod safe
	 */
	if (!devs->active_if)
		fl2000_put_devices(usb_dev, devs);
}

static int fl2000_suspend(struct usb_interface *interface, pm_message_t message)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	UNUSED(message);

	dev_dbg(&usb_dev->dev, "resume");

	/* TODO: suspend
	 * drm_mode_config_helper_suspend()
	 */

	return 0;
}

static int fl2000_resume(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);

	dev_dbg(&usb_dev->dev, "suspend");

	/* TODO: resume
	 * drm_mode_config_helper_resume()
	 */

	return 0;
}

static struct usb_driver fl2000_driver = {
	.name = USB_DRIVER_NAME,
	.probe = fl2000_probe,
	.disconnect = fl2000_disconnect,
	.suspend = fl2000_suspend,
	.resume = fl2000_resume,
	.id_table = fl2000_id_table,
	.supports_autosuspend = false,
	.disable_hub_initiated_lpm = true,
};

module_usb_driver(fl2000_driver);

MODULE_AUTHOR("Artem Mygaiev");
MODULE_DESCRIPTION("FL2000 USB display driver");
MODULE_LICENSE("GPL v2");
