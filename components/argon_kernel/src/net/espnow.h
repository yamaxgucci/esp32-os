/*
 * ArgonOS - ESP-NOW, kernel side: a queue between the radio and the shell.
 *
 * The port (argon/port/espnow.h) delivers a received datagram on the radio's
 * own task, once, and must not block there.  This owns the ring those frames
 * land in and the lock that lets the shell drain it from its own task, and
 * wraps the rest of the port so a command never touches the port directly.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_KERNEL_NET_ESPNOW_H
#define ARGON_KERNEL_NET_ESPNOW_H

#include <argon/port/espnow.h>

#if AG_PORT_HAS_ESPNOW

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

/* Start receiving and allocate the ring; idempotent.  The radio must already
 * be up (ag_net_init), exactly as ESP-NOW itself requires. */
ag_err_t ag_espnow_start(void);
void     ag_espnow_stop(void);
bool     ag_espnow_running(void);

ag_err_t ag_espnow_self(uint8_t out[6]);
ag_err_t ag_espnow_peer_add(const uint8_t mac[6], uint8_t channel,
                            const uint8_t *key);
ag_err_t ag_espnow_peer_del(const uint8_t mac[6]);
ag_err_t ag_espnow_send(const uint8_t mac[6], const void *data, uint32_t len);

/* Pop the oldest waiting datagram into buf (up to bufcap bytes); false when
 * none is waiting.  mac gets the sender's six bytes. */
bool ag_espnow_recv(uint8_t mac[6], uint8_t *buf, uint32_t bufcap,
                    uint32_t *len);

/* How many frames the ring had to drop because the shell was not draining it
 * fast enough - the one number that says "you are missing traffic". */
uint32_t ag_espnow_dropped(void);

#endif /* AG_PORT_HAS_ESPNOW */

#endif /* ARGON_KERNEL_NET_ESPNOW_H */
