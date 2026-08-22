/*
 * ArgonOS port: ESP-IDF - is there ESP-NOW in this build.
 *
 * A sub-option of the radio, like the access point: ESP-NOW is the same
 * transceiver used a different way - short datagrams straight to another
 * board's hardware address, no access point and no association in between - so
 * it needs the Wi-Fi stack linked but adds only a little of its own.  Off turns
 * that little back into free memory on a board that has no sibling to talk to.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_ESPNOW_H
#define ARGON_PORT_IMPL_ESPNOW_H

#include "sdkconfig.h"

#include <argon/port/impl/wifi.h> /* AG_PORT_HAS_WIFI */

#if AG_PORT_HAS_WIFI && defined(CONFIG_ARGON_NET_ESPNOW) && \
    CONFIG_ARGON_NET_ESPNOW
#define AG_PORT_HAS_ESPNOW 1
#else
#define AG_PORT_HAS_ESPNOW 0
#endif

#endif /* ARGON_PORT_IMPL_ESPNOW_H */
