/*
 * ArgonOS - /dev/usbhid0: the keyboard and mouse this board pretends to be.
 *
 * The counterpart of usbinput.h.  There, HID reports arrive from a keyboard
 * plugged into this board and become console events; here they leave, down a
 * cable into somebody else's computer, and become what that computer believes a
 * person typed.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_USBHID_H
#define ARGON_USBHID_H

#include <argon/abi.h>
#include <argon/port/usbdev.h>

#ifdef __cplusplus
extern "C" {
#endif

#if AG_PORT_HAS_USB_DEVICE
/*
 * Registers /dev/usbhid0.  The gadget itself is started separately, exactly as
 * the host is: a board can carry the device stack and not be plugged into
 * anything, and a board can be plugged in with nothing driving it.
 */
ag_err_t ag_usbhid_init(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ARGON_USBHID_H */
