// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dell privacy notification driver
 *
 * Copyright (C) 2021 Dell Inc. All Rights Reserved.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/wmi.h>
#include "dell-privacy-wmi.h"

#define DELL_PRIVACY_GUID "6932965F-1671-4CEB-B988-D3AB0A901919"
#define MICROPHONE_STATUS		    BIT(0)
#define CAMERA_STATUS		        BIT(1)
#define PRIVACY_SCREEN_STATUS		BIT(2)

static int privacy_valid = -EPROBE_DEFER;
static LIST_HEAD(wmi_list);
static DEFINE_MUTEX(list_mutex);

struct privacy_wmi_data {
	struct input_dev *input_dev;
	struct wmi_device *wdev;
	struct list_head list;
	u32 features_present;
	u32 last_status;
};

/*
 * Keymap for WMI privacy events of type 0x0012
 */
static const struct key_entry dell_wmi_keymap_type_0012[] = {
	/* privacy mic mute */
	{ KE_KEY, 0x0001, { KEY_MICMUTE } },
	/* privacy camera mute */
	{ KE_SW,  0x0002, { SW_CAMERA_LENS_COVER } },
	{ KE_END, 0},
};

int dell_privacy_valid(void)
{
	int ret;

	ret = wmi_has_guid(DELL_PRIVACY_GUID);
	if (!ret)
		return -ENODEV;
	ret = privacy_valid;
	return ret;
}
EXPORT_SYMBOL_GPL(dell_privacy_valid);

void dell_privacy_process_event(int type, int code, int status)
{
	struct privacy_wmi_data *priv;
	const struct key_entry *key;

	mutex_lock(&list_mutex);
	priv = list_first_entry_or_null(&wmi_list,
			struct privacy_wmi_data,
			list);
	if (!priv) {
		pr_err("dell privacy priv is NULL\n");
		goto error;
	}
	key = sparse_keymap_entry_from_scancode(priv->input_dev, (type << 16)|code);
	if (!key) {
		dev_dbg(&priv->wdev->dev, "Unknown key with type 0x%04x and code 0x%04x pressed\n",
				type, code);
		goto error;
	}
	switch (code) {
	case DELL_PRIVACY_TYPE_AUDIO: /* Mic mute */
		priv->last_status = status;
		sparse_keymap_report_entry(priv->input_dev, key, 1, true);
		break;
	case DELL_PRIVACY_TYPE_CAMERA: /* Camera mute */
		priv->last_status = status;
		sparse_keymap_report_entry(priv->input_dev, key, 1, true);
		break;
	default:
			dev_dbg(&priv->wdev->dev, "unknown event type 0x%04x 0x%04x",
					type, code);
	}
error:
	mutex_unlock(&list_mutex);
}
EXPORT_SYMBOL_GPL(dell_privacy_process_event);

static ssize_t devices_supported_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct privacy_wmi_data *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%x\n", priv->features_present);
}

static ssize_t current_state_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct privacy_wmi_data *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%x\n", priv->last_status);
}

static DEVICE_ATTR_RO(devices_supported);
static DEVICE_ATTR_RO(current_state);

static struct attribute *privacy_attributes[] = {
	&dev_attr_devices_supported.attr,
	&dev_attr_current_state.attr,
	NULL,
};

static const struct attribute_group privacy_attribute_group = {
	.attrs = privacy_attributes
};

/*
 * Describes the Device State class exposed by BIOS which can be consumed by
 * various applications interested in knowing the Privacy feature capabilities.
 * class DeviceState
 * {
 *  [key, read] string InstanceName;
 *  [read] boolean ReadOnly;
 *  [WmiDataId(1), read] uint32 DevicesSupported;
 *   0 – None, 0x1 – Microphone, 0x2 – Camera, 0x4 -ePrivacy  Screen
 *  [WmiDataId(2), read] uint32 CurrentState;
 *   0:Off; 1:On. Bit0 – Microphone, Bit1 – Camera, Bit2 - ePrivacyScreen
 * };
 */

static int get_current_status(struct wmi_device *wdev)
{
	struct privacy_wmi_data *priv = dev_get_drvdata(&wdev->dev);
	union acpi_object *obj_present;
	u32 *buffer;
	int ret = 0;

	if (!priv) {
		pr_err("dell privacy priv is NULL\n");
		return -EINVAL;
	}
	/* check privacy support features and device states */
	obj_present = wmidev_block_query(wdev, 0);
	if (!obj_present) {
		dev_err(&wdev->dev, "failed to read Binary MOF\n");
		ret = -EIO;
		privacy_valid = ret;
		return ret;
	}

	if (obj_present->type != ACPI_TYPE_BUFFER) {
		dev_err(&wdev->dev, "Binary MOF is not a buffer!\n");
		ret = -EIO;
		privacy_valid = ret;
		goto obj_free;
	}
	/*  Although it's not technically a failure, this would lead to
	 *  unexpected behavior
	 */
	if (obj_present->buffer.length != 8) {
		dev_err(&wdev->dev, "Dell privacy buffer has unexpected length (%d)!\n",
				obj_present->buffer.length);
		ret = -EINVAL;
		privacy_valid = ret;
		goto obj_free;
	}
	buffer = (u32 *)obj_present->buffer.pointer;
	priv->features_present = buffer[0];
	priv->last_status = buffer[1];
	privacy_valid = 0;

obj_free:
	kfree(obj_present);
	return ret;
}

static int dell_privacy_wmi_probe(struct wmi_device *wdev, const void *context)
{
	struct privacy_wmi_data *priv;
	struct key_entry *keymap;
	int ret, i, pos = 0;

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dev_set_drvdata(&wdev->dev, priv);
	priv->wdev = wdev;
	/* create evdev passing interface */
	priv->input_dev = devm_input_allocate_device(&wdev->dev);
	if (!priv->input_dev)
		return -ENOMEM;
	/* remap the wmi keymap event to new keymap */
	keymap = kcalloc(ARRAY_SIZE(dell_wmi_keymap_type_0012) +
			1,
			sizeof(struct key_entry), GFP_KERNEL);
	if (!keymap) {
		ret = -ENOMEM;
		goto err_free_dev;
	}
	/* remap the keymap code with Dell privacy key type 0x12 as prefix
	 * KEY_MICMUTE scancode will be reported as 0x120001
	 */
	for (i = 0; i < ARRAY_SIZE(dell_wmi_keymap_type_0012); i++) {
		keymap[pos] = dell_wmi_keymap_type_0012[i];
		keymap[pos].code |= (0x0012 << 16);
		pos++;
	}
	ret = sparse_keymap_setup(priv->input_dev, keymap, NULL);
	if (ret)
		return ret;
	priv->input_dev->dev.parent = &wdev->dev;
	priv->input_dev->name = "Dell Privacy Driver";
	priv->input_dev->id.bustype = BUS_HOST;
	if (input_register_device(priv->input_dev)) {
		pr_debug("input_register_device failed to register!\n");
		goto err_free_keymap;
	}
	mutex_lock(&list_mutex);
	list_add_tail(&priv->list, &wmi_list);
	mutex_unlock(&list_mutex);
	if (get_current_status(priv->wdev))
		goto err_free_input;
	ret = devm_device_add_group(&wdev->dev, &privacy_attribute_group);
	if (ret)
		goto err_free_input;
	kfree(keymap);
	return 0;

err_free_input:
	input_unregister_device(priv->input_dev);
err_free_keymap:
	privacy_valid = -ENODEV;
	kfree(keymap);
err_free_dev:
	input_free_device(priv->input_dev);
	return ret;
}

static int dell_privacy_wmi_remove(struct wmi_device *wdev)
{
	struct privacy_wmi_data *priv = dev_get_drvdata(&wdev->dev);

	mutex_lock(&list_mutex);
	list_del(&priv->list);
	mutex_unlock(&list_mutex);
	privacy_valid = -ENODEV;
	input_unregister_device(priv->input_dev);

	return 0;
}

static const struct wmi_device_id dell_wmi_privacy_wmi_id_table[] = {
	{ .guid_string = DELL_PRIVACY_GUID },
	{ },
};

static struct wmi_driver dell_privacy_wmi_driver = {
	.driver = {
		.name = "dell-privacy",
	},
	.probe = dell_privacy_wmi_probe,
	.remove = dell_privacy_wmi_remove,
	.id_table = dell_wmi_privacy_wmi_id_table,
};

static int __init init_dell_privacy(void)
{
	int ret;

	ret = wmi_has_guid(DELL_PRIVACY_GUID);
	if (!ret)
		return -ENODEV;

	ret = wmi_driver_register(&dell_privacy_wmi_driver);
	if (ret) {
		pr_err("failed to initialize privacy wmi driver: %d\n", ret);
		return ret;
	}

	ret = dell_privacy_acpi_init();
	if (ret) {
		pr_err("failed to initialize privacy acpi driver: %d\n", ret);
		goto err_init;
	}

	return 0;

err_init:
	wmi_driver_unregister(&dell_privacy_wmi_driver);
	return ret;
}

static void dell_privacy_wmi_exit(void)
{
	wmi_driver_unregister(&dell_privacy_wmi_driver);
}

static void __exit exit_dell_privacy(void)
{
	dell_privacy_wmi_exit();
	dell_privacy_acpi_exit();
}

module_init(init_dell_privacy);
module_exit(exit_dell_privacy);

MODULE_DEVICE_TABLE(wmi, dell_wmi_privacy_wmi_id_table);
MODULE_AUTHOR("Perry Yuan <perry_yuan@dell.com>");
MODULE_DESCRIPTION("Dell Privacy WMI Driver");
MODULE_LICENSE("GPL");
