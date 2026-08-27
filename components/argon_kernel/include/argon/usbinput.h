/*
 * ArgonOS - HID reports from a USB device, as console events.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_USBINPUT_H
#define ARGON_USBINPUT_H

#include <argon/abi.h>
#include <argon/port/usb.h>

#ifdef __cplusplus
extern "C" {
#endif

#if AG_PORT_HAS_USB_HID
/* Registers the report handler.  The host is started separately: a board can
 * have USB host up and nothing plugged into it. */
ag_err_t ag_usbinput_init(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ARGON_USBINPUT_H */
