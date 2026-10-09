/*
 * ArgonOS port: ESP-IDF - is there a USB device in this build.
 *
 * A build option, not a chip fact: the OTG peripheral is a host on every part
 * that has one, and a device only when the operator asked for it, because as a
 * device it takes the one USB port away from the console (one internal PHY, two
 * owners).  Off by default, on only for the board that runs the card reader.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_USBD_H
#define ARGON_PORT_IMPL_USBD_H

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED && defined(CONFIG_ARGON_USB_MSC) && CONFIG_ARGON_USB_MSC
#define AG_PORT_HAS_USBD 1
#else
#define AG_PORT_HAS_USBD 0
#endif

#endif /* ARGON_PORT_IMPL_USBD_H */
