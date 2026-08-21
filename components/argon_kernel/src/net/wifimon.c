/*
 * ArgonOS - Wi-Fi monitor/injection, kernel side.  See net/wifimon.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "net/wifimon.h"

#if AG_PORT_HAS_WIFIMON

#include <string.h>

#include <argon/port/mem.h>
#include <argon/port/sync.h>
#include <argon/port/task.h> /* AG_PORT_FOREVER */

/*
 * Sixteen frame prefixes waiting.  The air moves far faster than a shell can
 * print, so this is a sample, not a capture: what does not fit is dropped and
 * counted.  Sixteen times 128 bytes is about two kilobytes, taken while the
 * monitor is on and given back when it stops.
 */
#define AG_WIFIMON_RING 16u

typedef struct {
    int8_t   rssi;
    uint8_t  channel;
    uint32_t full;                 /* the frame's real length on the air   */
    uint32_t copied;               /* how much of it is in data[]          */
    uint8_t  data[AG_WIFIMON_SNAP];
} slot_t;

static ag_port_mutex_t   s_lock;
static slot_t           *s_ring;
static volatile uint32_t s_head;
static volatile uint32_t s_tail;
static volatile uint32_t s_dropped;
static volatile uint32_t s_count[AG_WIFIMON_C_N];
static volatile bool     s_running;

/*
 * On the radio's task.  Everything slow is forbidden here, so this only reads
 * one byte to bin the frame by type, then copies a prefix into the ring under
 * the lock.  The lock is taken because stop() frees the ring from another task.
 */
static void on_frame(const uint8_t *frame, uint32_t len, int8_t rssi,
                     uint8_t channel)
{
    s_count[AG_WIFIMON_C_TOTAL]++;
    if (len >= 1u) {
        /* 802.11 frame control: the type is bits 2..3 of the first byte. */
        switch ((frame[0] >> 2) & 0x3u) {
        case 0: s_count[AG_WIFIMON_C_MGMT]++; break;
        case 1: s_count[AG_WIFIMON_C_CTRL]++; break;
        case 2: s_count[AG_WIFIMON_C_DATA]++; break;
        default: s_count[AG_WIFIMON_C_MISC]++; break;
        }
    }

    ag_port_mutex_take(s_lock, AG_PORT_FOREVER);
    if (s_ring != NULL) {
        const uint32_t next = (s_head + 1u) % AG_WIFIMON_RING;
        if (next == s_tail) {
            s_dropped++;
        } else {
            slot_t  *sl = &s_ring[s_head];
            uint32_t n = (len < AG_WIFIMON_SNAP) ? len : AG_WIFIMON_SNAP;
            sl->rssi = rssi;
            sl->channel = channel;
            sl->full = len;
            sl->copied = n;
            if (n > 0u) {
                memcpy(sl->data, frame, n);
            }
            s_head = next;
        }
    }
    ag_port_mutex_give(s_lock);
}

ag_err_t ag_wifimon_start(void)
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
        s_ring = ag_port_alloc(sizeof(slot_t) * AG_WIFIMON_RING,
                               AG_MEM_FAST | AG_MEM_BYTE);
        if (s_ring == NULL) {
            return -AG_ENOMEM;
        }
    }
    s_head = 0;
    s_tail = 0;
    s_dropped = 0;
    memset((void *)s_count, 0, sizeof(s_count));

    ag_port_wifi_mon_on_frame(on_frame);
    const ag_err_t err = ag_port_wifi_mon_start();
    if (err != AG_OK) {
        ag_port_wifi_mon_on_frame(NULL);
        ag_port_free(s_ring);
        s_ring = NULL;
        return err;
    }
    s_running = true;
    return AG_OK;
}

void ag_wifimon_stop(void)
{
    if (!s_running) {
        return;
    }
    (void)ag_port_wifi_mon_stop();
    ag_port_wifi_mon_on_frame(NULL);
    s_running = false;

    ag_port_mutex_take(s_lock, AG_PORT_FOREVER);
    if (s_ring != NULL) {
        ag_port_free(s_ring);
        s_ring = NULL;
    }
    ag_port_mutex_give(s_lock);
}

bool ag_wifimon_running(void) { return s_running; }

ag_err_t ag_wifimon_channel(uint8_t primary)
{
    return ag_port_wifi_mon_channel(primary);
}

uint8_t ag_wifimon_channel_get(void) { return ag_port_wifi_mon_get_channel(); }

ag_err_t ag_wifimon_filter(uint32_t mask)
{
    return ag_port_wifi_mon_filter(mask);
}

void ag_wifimon_counters(uint32_t out[AG_WIFIMON_C_N])
{
    for (int i = 0; i < AG_WIFIMON_C_N; i++) {
        out[i] = s_count[i];
    }
}

uint32_t ag_wifimon_dropped(void) { return s_dropped; }

bool ag_wifimon_drain(int8_t *rssi, uint8_t *channel, uint32_t *full,
                      uint8_t *buf, uint32_t bufcap, uint32_t *copied)
{
    bool got = false;
    if (s_lock == NULL) {
        return false;
    }
    ag_port_mutex_take(s_lock, AG_PORT_FOREVER);
    if (s_ring != NULL && s_tail != s_head) {
        const slot_t *sl = &s_ring[s_tail];
        uint32_t      n = (sl->copied < bufcap) ? sl->copied : bufcap;
        *rssi = sl->rssi;
        *channel = sl->channel;
        *full = sl->full;
        if (n > 0u) {
            memcpy(buf, sl->data, n);
        }
        *copied = n;
        s_tail = (s_tail + 1u) % AG_WIFIMON_RING;
        got = true;
    }
    ag_port_mutex_give(s_lock);
    return got;
}

ag_err_t ag_wifimon_tx(const void *frame, uint32_t len)
{
    return ag_port_wifi_tx_raw(frame, len);
}

#endif /* AG_PORT_HAS_WIFIMON */
