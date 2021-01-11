/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Dell privacy notification driver
 *
 * Copyright (C) 2021 Dell Inc. All Rights Reserved.
 */

#ifndef _DELL_PRIVACY_WMI_H_
#define _DELL_PRIVACY_WMI_H_

#if IS_ENABLED(CONFIG_DELL_PRIVACY)
extern int privacy_valid;
int  dell_privacy_state(void);
void dell_privacy_process_event(int type, int code, int status);
#else /* CONFIG_DELL_PRIVACY */
int privacy_valid = -ENODEV;
static inline int dell_privacy_state(void)
{
	return privacy_valid;
}

static inline void dell_privacy_process_event(int type, int code, int status)
{}
#endif /* CONFIG_DELL_PRIVACY */

int  dell_privacy_acpi_init(void);
void dell_privacy_acpi_exit(void);

/* DELL Privacy Type */
enum {
	DELL_PRIVACY_TYPE_UNKNOWN = 0x0,
	DELL_PRIVACY_TYPE_AUDIO,
	DELL_PRIVACY_TYPE_CAMERA,
};
#endif
