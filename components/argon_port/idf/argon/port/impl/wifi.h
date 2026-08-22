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

/*
 * The access point half, a sub-option of the radio and not a chip fact either.
 *
 * The station code is what makes a network exist for this board to use; the
 * access point is the board making a network for something else to use, and it
 * is separable because it is not free.  It is a second netif, a DHCP server and
 * their buffers - a few kilobytes taken for as long as the point is up - on a
 * chip that already warns when the margin to associate is thin.  A board that
 * only ever joins networks should not carry the code to run one, so it is a
 * build option under the radio rather than always present.
 */
#if AG_PORT_HAS_WIFI && defined(CONFIG_ARGON_NET_WIFI_AP) && \
    CONFIG_ARGON_NET_WIFI_AP
#define AG_PORT_WIFI_HAS_AP 1
#else
#define AG_PORT_WIFI_HAS_AP 0
#endif

#endif /* ARGON_PORT_IMPL_WIFI_H */
