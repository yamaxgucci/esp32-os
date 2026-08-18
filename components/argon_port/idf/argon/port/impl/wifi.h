/*
 * ArgonOS port: ESP-IDF - is there a radio in this build.
 *
 * A build option and not a chip fact: every ESP32 has the radio, and linking
 * it costs about fifty kilobytes of RAM that a board using Ethernet, or QEMU,
 * has no reason to spend.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_WIFI_H
#define ARGON_PORT_IMPL_WIFI_H

#include "sdkconfig.h"

#if defined(CONFIG_ARGON_NET_WIFI) && CONFIG_ARGON_NET_WIFI
#define AG_PORT_HAS_WIFI 1
#else
#define AG_PORT_HAS_WIFI 0
#endif

#endif /* ARGON_PORT_IMPL_WIFI_H */
