// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dell privacy notification driver
 *
 * Copyright (C) 2021 Dell Inc. All Rights Reserved.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/wmi.h>

#include "dell-privacy-wmi.h"

#define PRIVACY_PLATFORM_NAME	"dell-privacy-acpi"
#define DELL_PRIVACY_GUID	"6932965F-1671-4CEB-B988-D3AB0A901919"

struct privacy_acpi_priv {
	struct device *dev;
	struct platform_device *platform_device;
	struct led_classdev cdev;
};
static struct privacy_acpi_priv *privacy_acpi;

static int dell_privacy_micmute_led_set(struct led_classdev *led_cdev,
		enum led_brightness brightness)
{
	struct privacy_acpi_priv *priv = privacy_acpi;
	acpi_status status;
	acpi_handle handle;
	static char *acpi_method = (char *)"ECAK";

	handle = ec_get_handle();
	if (!handle)
		return -EIO;
	if (!acpi_has_method(handle, acpi_method))
		return -EIO;
	status = acpi_evaluate_object(handle, acpi_method, NULL, NULL);
	if (ACPI_FAILURE(status)) {
		dev_err(priv->dev, "Error setting privacy EC ack value: %s\n",
				acpi_format_exception(status));
		return -EIO;
	}
	pr_debug("set dell privacy micmute ec ack event done\n");
	return 0;
}

static int dell_privacy_acpi_remove(struct platform_device *pdev)
{
	struct privacy_acpi_priv *priv = dev_get_drvdata(privacy_acpi->dev);

	led_classdev_unregister(&priv->cdev);

	return 0;
}
/*
 * Pressing the mute key activates a time delayed circuit to physically cut
 * off the mute. The LED is in the same circuit, so it reflects the true
 * state of the HW mute.  The reason for the EC "ack" is so that software
 * can first invoke a SW mute before the HW circuit is cut off.  Without SW
 * cutting this off first does not affect the time delayed muting or status
 * of the LED but there is a possibility of a "popping" noise.
 *
 * If the EC receives the SW ack, the circuit will be activated before the
 * delay completed.
 *
 * Exposing as an LED device allows the codec drivers notification path to
 * EC ACK to work
 */
static int dell_privacy_leds_setup(struct device *dev)
{
	struct privacy_acpi_priv *priv = dev_get_drvdata(dev);
	int ret = 0;

	priv->cdev.name = "dell-privacy::micmute";
	priv->cdev.max_brightness = 1;
	priv->cdev.brightness_set_blocking = dell_privacy_micmute_led_set;
	priv->cdev.default_trigger = "audio-micmute";
	priv->cdev.brightness = ledtrig_audio_get(LED_AUDIO_MICMUTE);
	ret = devm_led_classdev_register(dev, &priv->cdev);
	if (ret)
		return ret;
	return 0;
}

static int dell_privacy_acpi_probe(struct platform_device *pdev)
{
	int ret;

	platform_set_drvdata(pdev, privacy_acpi);
	privacy_acpi->dev = &pdev->dev;
	privacy_acpi->platform_device = pdev;

	ret = dell_privacy_leds_setup(&pdev->dev);
	if (ret)
		return -EIO;

	return 0;
}

static struct platform_driver dell_privacy_platform_drv = {
	.driver = {
		.name = PRIVACY_PLATFORM_NAME,
	},
	.probe = dell_privacy_acpi_probe,
	.remove = dell_privacy_acpi_remove,
};

int __init dell_privacy_acpi_init(void)
{
	int err;
	struct platform_device *pdev;

	if (!wmi_has_guid(DELL_PRIVACY_GUID))
		return -ENODEV;

	privacy_acpi = kzalloc(sizeof(*privacy_acpi), GFP_KERNEL);
	if (!privacy_acpi)
		return -ENOMEM;

	err = platform_driver_register(&dell_privacy_platform_drv);
	if (err)
		goto pdrv_err;

	pdev = platform_device_register_simple(
			PRIVACY_PLATFORM_NAME, PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(pdev)) {
		err = PTR_ERR(pdev);
		goto pdev_err;
	}

	return 0;

pdev_err:
	platform_device_unregister(pdev);
pdrv_err:
	kfree(privacy_acpi);
	return err;
}

void __exit dell_privacy_acpi_exit(void)
{
	struct platform_device *pdev = to_platform_device(privacy_acpi->dev);

	platform_device_unregister(pdev);
	platform_driver_unregister(&dell_privacy_platform_drv);
	kfree(privacy_acpi);
}

MODULE_AUTHOR("Perry Yuan <perry_yuan@dell.com>");
MODULE_DESCRIPTION("DELL Privacy ACPI Driver");
MODULE_LICENSE("GPL");
