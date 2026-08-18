/*
 * ArgonOS port: ESP-IDF - what the link half tells the socket half.
 *
 * Private to the port.  Two files implement ag_port_net_*: net_hw.c owns the
 * address and the sockets, wifi_hw.c owns the radio, and these are the only
 * two things the radio has to say.  Nothing above the port sees this header.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_NET_HW_H
#define ARGON_PORT_NET_HW_H

#include "esp_netif.h"

/* Which interface's address counts as "the" address. */
void ag_port_net_set_netif(esp_netif_t *netif);

/* The link went away: there is no address any more, whatever DHCP said. */
void ag_port_net_link_down(void);

#endif /* ARGON_PORT_NET_HW_H */
