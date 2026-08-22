/*
 * ArgonOS port contract - general BLE, the central role.
 *
 * bt.h is the keyboard: find one, pair, turn its reports into keystrokes, and
 * nothing else, on purpose.  This is everything else a BLE central does, and
 * the reason it is a separate header is the same reason espnow.h is separate
 * from wifi.h - it is a different thing to be.  A central here does two things
 * a keyboard host never had to:
 *
 *   - observe: listen to what every device in range is advertising and say what
 *     it is - name, address, how strong, whether you could connect, which
 *     services it offers, who made it;
 *   - (coming next) connect to an arbitrary device and read or write an
 *     arbitrary characteristic, not only a HID report.
 *
 * It rides the NimBLE host bt_hw.c starts (ag_port_bt_start): there is one
 * controller and one host on this chip, and a keyboard and a scanner are two
 * uses of it, not two of it.  So the radio must be started before any of this
 * works, and only one scan - HID or general - runs at a time.
 *
 * What a port must supply, when AG_PORT_HAS_BLE_CENTRAL is 1:
 *
 *   ag_err_t ag_port_ble_scan(ag_port_ble_dev_t *out, uint32_t max,
 *                             uint32_t *found, uint32_t seconds)
 *
 * Contract, not advice:
 *
 * - scan() needs the radio started (ag_port_bt_start); -AG_ENODEV otherwise,
 *   and -AG_EBUSY while a HID scan or a connection attempt is in flight, because
 *   there is only one radio to point.
 * - It blocks for `seconds` (0 means a sensible default): a scan is a listening
 *   window and there is nothing to return until it closes.  A device that
 *   advertises many times a second appears once, with the most informative of
 *   what its several packets carried - the name usually arrives in a second
 *   packet the first did not have.
 * - It is an active scan: it asks each device for its scan response, because
 *   that is where the name usually is.  A passive observer would see addresses
 *   and little else.
 *
 * The GATT client, when AG_PORT_HAS_BLE_CENTRAL is 1:
 *
 *   ag_err_t ag_port_ble_connect(const uint8_t addr[6], int addr_type,
 *                                uint32_t timeout_ms)
 *   ag_err_t ag_port_ble_disconnect(void)
 *   bool     ag_port_ble_connected(void)
 *   ag_err_t ag_port_ble_discover(uint32_t timeout_ms)
 *   uint32_t ag_port_ble_services(ag_ble_svc_t *out, uint32_t max)
 *   uint32_t ag_port_ble_chars(ag_ble_chr_t *out, uint32_t max)
 *   int32_t  ag_port_ble_read(uint16_t handle, uint8_t *out, uint32_t max,
 *                             uint32_t timeout_ms)
 *   ag_err_t ag_port_ble_write(uint16_t handle, const void *data, uint32_t len,
 *                              bool with_response, uint32_t timeout_ms)
 *
 * Contract, not advice:
 *
 * - One connection at a time, and not while scanning: -AG_EBUSY.  The radio is
 *   one thing; observing and being connected are two uses of it that this layer
 *   does not overlap.  -AG_ENODEV when the radio is off.
 * - connect() blocks until the link is up or the attempt failed - unlike the
 *   HID open() in bt.h, because a GATT client has no state machine above it to
 *   wait on and the shell command that calls it wants an answer.
 * - discover() walks every service and every characteristic and fills the two
 *   tables read by services()/chars().  It blocks; on a device with many
 *   attributes that is a second or two.  A characteristic's `handle` is what
 *   read() and write() take - the value handle, not the declaration.
 * - read() returns the number of bytes placed in `out` (up to `max`, longer
 *   values are truncated at the ATT MTU), or a negative ag_err_t.  write() with
 *   with_response waits for the peer's acknowledgement; without, it returns as
 *   soon as the frame is queued and a failure is silent - which is what "without
 *   a response" means and the caller asked for.
 * - A peer that drops the link is not an error the port hides: connected() goes
 *   false, and the next read/write is -AG_ENODEV (there is no connection to use).
 *   Nothing here reconnects; a GATT client's session is the caller's to own,
 *   unlike a keyboard's.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_BLE_H
#define ARGON_PORT_BLE_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

#include <argon/port/impl/ble.h>

/*
 * The observed-device, service and characteristic shapes, and the AG_BLE_*
 * constants, are the public ABI's (argon/abi.h, included above) since 0.38 -
 * an application talks GATT through the same structs the port fills.  The port
 * keeps its historical name for the device record as an alias, so bt_hw.c and
 * the shell need no change.
 */
typedef ag_ble_dev_t ag_port_ble_dev_t;

#if AG_PORT_HAS_BLE_CENTRAL

ag_err_t ag_port_ble_scan(ag_port_ble_dev_t *out, uint32_t max, uint32_t *found,
                          uint32_t seconds);

ag_err_t ag_port_ble_connect(const uint8_t addr[6], int addr_type,
                             uint32_t timeout_ms);
ag_err_t ag_port_ble_disconnect(void);
bool     ag_port_ble_connected(void);
ag_err_t ag_port_ble_discover(uint32_t timeout_ms);
uint32_t ag_port_ble_services(ag_ble_svc_t *out, uint32_t max);
uint32_t ag_port_ble_chars(ag_ble_chr_t *out, uint32_t max);
int32_t  ag_port_ble_read(uint16_t handle, uint8_t *out, uint32_t max,
                          uint32_t timeout_ms);
ag_err_t ag_port_ble_write(uint16_t handle, const void *data, uint32_t len,
                           bool with_response, uint32_t timeout_ms);

#endif /* AG_PORT_HAS_BLE_CENTRAL */

/*
 * The peripheral, when AG_PORT_HAS_BLE_PERIPH is 1: the board as the device
 * others connect to.  It advertises a name and holds one custom service with a
 * characteristic a client can read and one it can write:
 *
 *   ag_err_t ag_port_ble_adv_start(const char *name)
 *   ag_err_t ag_port_ble_adv_stop(void)
 *   ag_err_t ag_port_ble_adv_status(ag_port_ble_adv_status_t *out)
 *   void     ag_port_ble_adv_set_read(const void *data, uint32_t len)
 *   int32_t  ag_port_ble_adv_last_write(uint8_t *out, uint32_t max)
 *
 * Contract, not advice:
 *
 * - adv_start() needs the radio started (ag_port_bt_start), like everything
 *   here.  It advertises connectably and forever until adv_stop(); a client
 *   that connects and leaves does not stop it - the board stays discoverable.
 * - The read characteristic returns whatever adv_set_read() last set (a short
 *   name by default).  The write characteristic accepts a client's bytes;
 *   adv_last_write() returns the most recent, and status() counts them.  What
 *   the board does with a write is the kernel's business, not the port's.
 * - One custom service, kept simple on purpose: this is "a phone can talk to
 *   the board", not a full profile.  A specific profile is a later, named job.
 *
 * The status shape is the public ABI's (ag_ble_adv_status_t, argon/abi.h) since
 * 0.38; the port keeps its historical name as an alias.
 */
typedef ag_ble_adv_status_t ag_port_ble_adv_status_t;

#if AG_PORT_HAS_BLE_PERIPH

ag_err_t ag_port_ble_adv_start(const char *name);
ag_err_t ag_port_ble_adv_stop(void);
ag_err_t ag_port_ble_adv_status(ag_port_ble_adv_status_t *out);
void     ag_port_ble_adv_set_read(const void *data, uint32_t len);
int32_t  ag_port_ble_adv_last_write(uint8_t *out, uint32_t max);

/*
 * BLE-MIDI: the board as a MIDI controller a phone or PC plays.  Same GATT
 * server, a different service - the standard Apple BLE-MIDI one - so a music
 * app (GarageBand and the like) recognises the board as a MIDI device.
 *
 * - midi_advertise() starts advertising *as a MIDI device* (the MIDI service
 *   UUID in the advertisement, which is what a music app scans for).  Otherwise
 *   the same as adv_start: connectable, forever, re-advertises after a client
 *   leaves.
 * - midi_send() sends one MIDI message (status + up to two data bytes) as a
 *   notification, wrapped in the BLE-MIDI timestamp header.  A note-on is
 *   0x90|channel, note, velocity; note-off is 0x80|channel or velocity 0.
 *   Returns -AG_ENODEV when no one is connected and subscribed - a controller
 *   with no listener is not an error to hide, but there is nowhere to send.
 * - midi_ready() is true when a client has subscribed to notifications, i.e.
 *   send() will actually reach someone.
 */
ag_err_t ag_port_ble_midi_advertise(const char *name);
ag_err_t ag_port_ble_midi_send(uint8_t status, uint8_t data1, uint8_t data2);
bool     ag_port_ble_midi_ready(void);

#endif /* AG_PORT_HAS_BLE_PERIPH */

#endif /* ARGON_PORT_BLE_H */
