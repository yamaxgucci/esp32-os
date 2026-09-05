/*
 * ArgonOS port: ESP-IDF - is there a USB device gadget in this build.
 *
 * The counterpart of impl/usb.h, and mutually exclusive with it: the Kconfig
 * option depends on SOC_USB_OTG_SUPPORTED and on ARGON_USB_HOST being off, so a
 * single symbol here is enough and it agrees with what usbdev_hw.c compiles.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_USBDEV_H
#define ARGON_PORT_IMPL_USBDEV_H

#include "sdkconfig.h"

#if defined(CONFIG_ARGON_USB_DEVICE) && CONFIG_ARGON_USB_DEVICE
#define AG_PORT_HAS_USB_DEVICE 1
#else
#define AG_PORT_HAS_USB_DEVICE 0
#endif

#if defined(CONFIG_ARGON_USB_DEVICE_CDC) && CONFIG_ARGON_USB_DEVICE_CDC
#define AG_PORT_HAS_USB_CDC 1
#else
#define AG_PORT_HAS_USB_CDC 0
#endif

#endif /* ARGON_PORT_IMPL_USBDEV_H */
