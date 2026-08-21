/*
 * ArgonOS port: ESP-IDF - is there a monitor/injection radio in this build.
 *
 * The deepest sub-option of the radio, and the one turned off by default.  The
 * station and the access point are a machine using a network; this is a machine
 * listening to the air that carries every network and putting frames of its own
 * on it, which is a different thing to be and not one a board should be able to
 * do because it happens to have a radio.  A build asks for it on purpose.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_WIFIMON_H
#define ARGON_PORT_IMPL_WIFIMON_H

#include "sdkconfig.h"

#include <argon/port/impl/wifi.h> /* AG_PORT_HAS_WIFI */

#if AG_PORT_HAS_WIFI && defined(CONFIG_ARGON_NET_WIFI_MON) && \
    CONFIG_ARGON_NET_WIFI_MON
#define AG_PORT_HAS_WIFIMON 1
#else
#define AG_PORT_HAS_WIFIMON 0
#endif

#endif /* ARGON_PORT_IMPL_WIFIMON_H */
