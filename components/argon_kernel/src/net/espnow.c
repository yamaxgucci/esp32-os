/*
 * ArgonOS - ESP-NOW, kernel side.  See net/espnow.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "net/espnow.h"

#if AG_PORT_HAS_ESPNOW

#include <string.h>

#include <argon/port/mem.h>
#include <argon/port/sync.h>
#include <argon/port/task.h> /* AG_PORT_FOREVER */

/*
 * Eight datagrams waiting.  Each is 250 bytes plus its address, so the ring is
 * about two kilobytes - taken from the heap the first time ESP-NOW is turned on
 * and given straight back when it is turned off, because a board that never
 * uses it should not carry the buffer.  Eight is a depth, not a queue anyone
 * relies on: a datagram is allowed to be lost, and dropped ones are counted
 * rather than pretended away.
 */
#define AG_ESPNOW_RING 8u

typedef struct {
    uint8_t  mac[6];
    uint32_t len;
    uint8_t  data[AG_ESPNOW_MAX];
} slot_t;

static ag_port_mutex_t   s_lock;
static slot_t           *s_ring;
static volatile uint32_t s_head;    /* next slot the radio will fill    */
static volatile uint32_t s_tail;    /* next slot the shell will read    */
static volatile uint32_t s_dropped;
static volatile bool     s_running;

/*
 * On the radio's task.  The lock is taken here as well as by the reader because
 * this is the producer and the shell is the consumer, on two different tasks,
 * and the port's stop() frees the ring - so the null check has to be inside the
 * lock, or a frame in flight during `espnow off` writes into freed memory.
 */
static void on_recv(const uint8_t mac[6], const uint8_t *data, uint32_t len)
{
    if (len > AG_ESPNOW_MAX) {
        len = AG_ESPNOW_MAX;
    }
    ag_port_mutex_take(s_lock, AG_PORT_FOREVER);
    if (s_ring != NULL) {
        const uint32_t next = (s_head + 1u) % AG_ESPNOW_RING;
        if (next == s_tail) {
            s_dropped++; /* full - the incoming one is lost, and counted */
        } else {
            memcpy(s_ring[s_head].mac, mac, 6);
            s_ring[s_head].len = len;
            if (len > 0u) {
                memcpy(s_ring[s_head].data, data, len);
            }
            s_head = next;
        }
    }
    ag_port_mutex_give(s_lock);
}

ag_err_t ag_espnow_start(void)
{
    if (s_running) {
        return AG_OK;
    }
    if (s_lock == NULL) {
        s_lock = ag_port_mutex_new();
        if (s_lock == NULL) {
            return -AG_ENOMEM;
        }
    }
    if (s_ring == NULL) {
        s_ring = ag_port_alloc(sizeof(slot_t) * AG_ESPNOW_RING,
                               AG_MEM_FAST | AG_MEM_BYTE);
        if (s_ring == NULL) {
            return -AG_ENOMEM;
        }
    }
    s_head = 0;
    s_tail = 0;
    s_dropped = 0;

    ag_port_espnow_on_recv(on_recv);
    const ag_err_t err = ag_port_espnow_start();
    if (err != AG_OK) {
        ag_port_espnow_on_recv(NULL);
        ag_port_free(s_ring);
        s_ring = NULL;
        return err;
    }
    s_running = true;
    return AG_OK;
}

void ag_espnow_stop(void)
{
    if (!s_running) {
        return;
    }
    /* Order matters: stop the source first, so no callback can be on its way in
     * while the ring is being freed, then take the lock to close the window on
     * one that already started. */
    (void)ag_port_espnow_stop();
    ag_port_espnow_on_recv(NULL);
    s_running = false;

    ag_port_mutex_take(s_lock, AG_PORT_FOREVER);
    if (s_ring != NULL) {
        ag_port_free(s_ring);
        s_ring = NULL;
    }
    ag_port_mutex_give(s_lock);
}

bool ag_espnow_running(void) { return s_running; }

ag_err_t ag_espnow_self(uint8_t out[6]) { return ag_port_espnow_self(out); }

ag_err_t ag_espnow_peer_add(const uint8_t mac[6], uint8_t channel,
                            const uint8_t *key)
{
    return ag_port_espnow_peer_add(mac, channel, key);
}

ag_err_t ag_espnow_peer_del(const uint8_t mac[6])
{
    return ag_port_espnow_peer_del(mac);
}

ag_err_t ag_espnow_send(const uint8_t mac[6], const void *data, uint32_t len)
{
    return ag_port_espnow_send(mac, data, len);
}

bool ag_espnow_recv(uint8_t mac[6], uint8_t *buf, uint32_t bufcap,
                    uint32_t *len)
{
    bool got = false;
    if (s_lock == NULL) {
        return false;
    }
    ag_port_mutex_take(s_lock, AG_PORT_FOREVER);
    if (s_ring != NULL && s_tail != s_head) {
        const slot_t  *s = &s_ring[s_tail];
        uint32_t       n = s->len;
        if (n > bufcap) {
            n = bufcap;
        }
        memcpy(mac, s->mac, 6);
        if (n > 0u) {
            memcpy(buf, s->data, n);
        }
        *len = n;
        s_tail = (s_tail + 1u) % AG_ESPNOW_RING;
        got = true;
    }
    ag_port_mutex_give(s_lock);
    return got;
}

uint32_t ag_espnow_dropped(void) { return s_dropped; }

#endif /* AG_PORT_HAS_ESPNOW */
