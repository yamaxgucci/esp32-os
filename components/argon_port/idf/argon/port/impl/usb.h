/*
 * ArgonOS port: ESP-IDF - is there a USB host in this build.
 *
 * The Kconfig option depends on SOC_USB_OTG_SUPPORTED, so it cannot be set on a
 * chip without the peripheral; a single symbol here is therefore enough, and it
 * agrees with what usb_hw.c compiles.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_USB_H
#define ARGON_PORT_IMPL_USB_H

#include "sdkconfig.h"

#if defined(CONFIG_ARGON_USB_HOST) && CONFIG_ARGON_USB_HOST
#define AG_PORT_HAS_USB_HID 1
#else
#define AG_PORT_HAS_USB_HID 0
#endif

#endif /* ARGON_PORT_IMPL_USB_H */
