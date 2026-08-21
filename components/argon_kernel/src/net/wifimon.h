/*
 * ArgonOS - Wi-Fi monitor/injection, kernel side.
 *
 * The port (argon/port/wifimon.h) captures raw 802.11 frames on the radio's own
 * task and injects raw ones.  This owns the small ring the captured frames land
 * in - only a prefix of each, because the whole air will not fit in this chip's
 * memory and a monitor does not need it to - and the type counters the callback
 * keeps, and it wraps injection so a command never touches the port directly.
 *
 * What a frame *means* is not here either: the shell parses the prefixes it
 * drains and builds whatever frames it injects.  This is the buffer and the
 * lock between the radio and the shell, nothing more.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_KERNEL_NET_WIFIMON_H
#define ARGON_KERNEL_NET_WIFIMON_H

#include <argon/port/wifimon.h>

#if AG_PORT_HAS_WIFIMON

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

/*
 * How much of each frame is kept.  A beacon's name lives in the first seventy
 * bytes or so (24 header + 12 fixed + the SSID tag), and the addresses that
 * identify any frame are in the first twenty-four - so a prefix this long is
 * everything the shell parses, at a fraction of the memory a full capture is.
 */
#define AG_WIFIMON_SNAP 128u

/* Index into the counters array from ag_wifimon_counters(). */
enum {
    AG_WIFIMON_C_TOTAL = 0,
    AG_WIFIMON_C_MGMT,
    AG_WIFIMON_C_CTRL,
    AG_WIFIMON_C_DATA,
    AG_WIFIMON_C_MISC,
    AG_WIFIMON_C_N
};

ag_err_t ag_wifimon_start(void);
void     ag_wifimon_stop(void);
bool     ag_wifimon_running(void);

ag_err_t ag_wifimon_channel(uint8_t primary);
uint8_t  ag_wifimon_channel_get(void);
ag_err_t ag_wifimon_filter(uint32_t mask);

/* The running frame counts, by type - AG_WIFIMON_C_* index the array. */
void     ag_wifimon_counters(uint32_t out[AG_WIFIMON_C_N]);
uint32_t ag_wifimon_dropped(void);

/*
 * Pop the oldest captured frame prefix.  `full` gets the frame's real length
 * (which may be larger than what fit in buf), `copied` how many bytes are in
 * buf.  false when nothing is waiting.
 */
bool ag_wifimon_drain(int8_t *rssi, uint8_t *channel, uint32_t *full,
                      uint8_t *buf, uint32_t bufcap, uint32_t *copied);

/* Put one raw frame on the current channel. */
ag_err_t ag_wifimon_tx(const void *frame, uint32_t len);

#endif /* AG_PORT_HAS_WIFIMON */

#endif /* ARGON_KERNEL_NET_WIFIMON_H */
