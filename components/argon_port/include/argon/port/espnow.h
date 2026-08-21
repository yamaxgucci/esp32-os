/*
 * ArgonOS port contract - ESP-NOW, board to board without a network.
 *
 * Separate from net.h and wifi.h on purpose, because it is neither.  net.h is
 * about having an address and sockets; wifi.h is about which network to join or
 * offer.  This is about neither having nor joining a network: two boards on the
 * same channel throw short frames straight at each other's hardware address,
 * with no access point, no association, no DHCP and no IP.  It is what a mesh of
 * sensors uses to talk when there is no router in the building, and it costs
 * almost nothing on top of a radio that is already linked.
 *
 * What a port must supply, when AG_PORT_HAS_ESPNOW is 1:
 *
 *   ag_err_t ag_port_espnow_start(void)
 *   ag_err_t ag_port_espnow_stop(void)
 *   ag_err_t ag_port_espnow_self(uint8_t out[6])
 *   ag_err_t ag_port_espnow_peer_add(const uint8_t mac[6], uint8_t channel,
 *                                    const uint8_t *key)
 *   ag_err_t ag_port_espnow_peer_del(const uint8_t mac[6])
 *   ag_err_t ag_port_espnow_send(const uint8_t mac[6], const void *data,
 *                                uint32_t len)
 *   void     ag_port_espnow_on_recv(ag_port_espnow_recv_fn fn)
 *
 * Contract, not advice:
 *
 * - start() needs the radio already started (ag_port_wifi_start): ESP-NOW rides
 *   the same transceiver, and there is nothing to send on until it is on.
 *   -AG_ENODEV otherwise.  start() also adds the broadcast address as a peer, so
 *   that a board can be heard before anyone knows its address.
 * - self() is the board's own hardware address, the thing the other end must
 *   pass to its own peer_add(): a datagram goes to an address, and the address
 *   is not discoverable from thin air.
 * - A frame carries at most AG_ESPNOW_MAX bytes.  Longer is -AG_EINVAL, not a
 *   silent truncation - a datagram protocol that keeps half a message is worse
 *   than one that refuses the whole.
 * - send() must have the destination as a peer first (peer_add, or the
 *   broadcast peer start() added).  It returns when the frame has been handed to
 *   the radio, not when it was acknowledged: on a shared channel a frame can be
 *   lost and nothing above here retries, because ESP-NOW is a datagram and a
 *   datagram that must arrive is a socket's job, not this one's.
 * - channel 0 in peer_add means "the channel the radio is on now".  Two boards
 *   with no access point between them must be told the same channel; a board
 *   joined to a network is on that network's channel and cannot leave it.
 * - key NULL is an open peer.  A key is exactly AG_ESPNOW_KEY bytes and turns on
 *   encryption for that peer; the same key must be set on both ends.
 * - Received frames arrive on the radio's own task, one call per frame, and the
 *   callback must be short - copy the bytes into a queue and return.  What the
 *   kernel does with them is its business; the port only says "these bytes came
 *   from that address".
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_ESPNOW_H
#define ARGON_PORT_ESPNOW_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

#include <argon/port/impl/espnow.h>

#define AG_ESPNOW_MAX 250 /* one datagram's payload, bytes            */
#define AG_ESPNOW_KEY 16  /* length of an encryption key, bytes       */

/*
 * One received datagram.  `mac` is the sender's hardware address, which is the
 * only thing that identifies it - there is no name and no address above the
 * link here.
 */
typedef void (*ag_port_espnow_recv_fn)(const uint8_t mac[6],
                                       const uint8_t *data, uint32_t len);

#if AG_PORT_HAS_ESPNOW

ag_err_t ag_port_espnow_start(void);
ag_err_t ag_port_espnow_stop(void);
ag_err_t ag_port_espnow_self(uint8_t out[6]);
ag_err_t ag_port_espnow_peer_add(const uint8_t mac[6], uint8_t channel,
                                 const uint8_t *key);
ag_err_t ag_port_espnow_peer_del(const uint8_t mac[6]);
ag_err_t ag_port_espnow_send(const uint8_t mac[6], const void *data,
                             uint32_t len);
void     ag_port_espnow_on_recv(ag_port_espnow_recv_fn fn);

#endif /* AG_PORT_HAS_ESPNOW */

#endif /* ARGON_PORT_ESPNOW_H */
