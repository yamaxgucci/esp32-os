/*
 * ArgonOS port: bare - no USB device gadget.
 *
 * The host build has no OTG peripheral to be a keyboard with, so the kernel's
 * usbhid device compiles to nothing there.  The part worth testing without a
 * board is not this: it is the report builders and the phone's wire format,
 * which are plain C in apps/common/kvm and are compiled straight into
 * host-tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_USBDEV_H
#define ARGON_PORT_IMPL_USBDEV_H

#define AG_PORT_HAS_USB_DEVICE 0

#define AG_PORT_HAS_USB_CDC 0

#define AG_PORT_HAS_USB_MSC 0

#endif /* ARGON_PORT_IMPL_USBDEV_H */
