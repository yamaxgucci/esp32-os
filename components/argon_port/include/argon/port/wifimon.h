/*
 * ArgonOS port contract - monitor mode and raw injection.
 *
 * Everything else the radio does is about a network: joining one (wifi.h),
 * offering one (the access point), or reaching one particular board without a
 * network (espnow.h).  This is about none of them.  In monitor mode the radio
 * is not on any network at all - it is a receiver turned to a channel, handing
 * up every frame that channel carries, addressed to this board or not; and
 * tx_raw puts a frame of the caller's own making into the air, with no check
 * that it is one this board had any business sending.
 *
 * That is the whole of it, on purpose.  This layer captures and it injects.  It
 * does not know what a beacon is, or a deauthentication, or a probe request -
 * building those out of bytes and deciding to send them is the kernel's
 * business, above here, because the port has no way to tell a frame sent to
 * test a driver from one sent to knock a stranger off their network, and the
 * place that can tell is the place a person typed the command.
 *
 * What a port must supply, when AG_PORT_HAS_WIFIMON is 1:
 *
 *   ag_err_t ag_port_wifi_mon_start(void)
 *   ag_err_t ag_port_wifi_mon_stop(void)
 *   ag_err_t ag_port_wifi_mon_channel(uint8_t primary)
 *   uint8_t  ag_port_wifi_mon_get_channel(void)
 *   ag_err_t ag_port_wifi_mon_filter(uint32_t mask)
 *   void     ag_port_wifi_mon_on_frame(ag_port_wifi_mon_fn fn)
 *   ag_err_t ag_port_wifi_tx_raw(const void *frame, uint32_t len)
 *
 * Contract, not advice:
 *
 * - start() needs the radio started (ag_port_wifi_start).  It puts the radio in
 *   promiscuous mode, which is not a mode a joined station is in: a board that
 *   was on a network leaves it here, and getting back on is a fresh connect().
 *   -AG_ENODEV when the radio is off.
 * - Frames arrive on the radio's own task, one call per frame, and the callback
 *   must be short - the air on a busy channel is thousands of frames a second,
 *   and anything slow here loses the ones behind it.  The frame pointer is the
 *   whole 802.11 frame including its header; it is valid only for the length of
 *   the call, so a callback that wants to keep bytes copies them.
 * - channel() is 1..14; the radio hears one channel at a time, so sweeping the
 *   band is the caller setting each in turn.  A board joined to a network cannot
 *   change channel, but a monitor is not joined to one.
 * - filter() is a mask of AG_WIFIMON_* bits: which frame types are handed up at
 *   all, decided in the driver so the ones filtered out never reach the task.
 *   AG_WIFIMON_ALL is every type.  The default before filter() is called is
 *   management and data.
 * - tx_raw() puts one frame on the current channel.  It returns when the frame
 *   has been handed to the radio, not when anything received it - there is no
 *   acknowledgement for a frame with no connection behind it.  The bytes are a
 *   complete 802.11 frame without the trailing FCS, which the radio appends.
 *   The driver rejects a frame that is malformed at the length or duration it
 *   checks; it does not, and cannot, reject one that is merely hostile.
 * - stop() leaves promiscuous mode and gives the radio back to being a station.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_WIFIMON_H
#define ARGON_PORT_WIFIMON_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

#include <argon/port/impl/wifimon.h>

/* Which kinds of frame the driver hands up.  A mask; they combine. */
#define AG_WIFIMON_MGMT 0x1u /* beacons, probes, auth, deauth, assoc     */
#define AG_WIFIMON_CTRL 0x2u /* RTS/CTS/ACK and the rest of the fabric   */
#define AG_WIFIMON_DATA 0x4u /* the frames that actually carry something */
#define AG_WIFIMON_MISC 0x8u /* everything the radio could not classify  */
#define AG_WIFIMON_ALL  0xfu

#define AG_WIFIMON_TX_MAX 1500u /* a raw frame this layer will inject       */

/*
 * One captured frame: the whole 802.11 frame as it was on the air, plus the two
 * things the radio knows that the frame itself does not - how strong it was and
 * which channel it came in on.
 */
typedef void (*ag_port_wifi_mon_fn)(const uint8_t *frame, uint32_t len,
                                    int8_t rssi, uint8_t channel);

#if AG_PORT_HAS_WIFIMON

ag_err_t ag_port_wifi_mon_start(void);
ag_err_t ag_port_wifi_mon_stop(void);
ag_err_t ag_port_wifi_mon_channel(uint8_t primary);
uint8_t  ag_port_wifi_mon_get_channel(void);
ag_err_t ag_port_wifi_mon_filter(uint32_t mask);
void     ag_port_wifi_mon_on_frame(ag_port_wifi_mon_fn fn);
ag_err_t ag_port_wifi_tx_raw(const void *frame, uint32_t len);

#endif /* AG_PORT_HAS_WIFIMON */

#endif /* ARGON_PORT_WIFIMON_H */
