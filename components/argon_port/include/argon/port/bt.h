/*
 * ArgonOS port contract - Bluetooth, for the things a person types on.
 *
 * Deliberately not "Bluetooth support".  What this layer is for is one thing:
 * a chip with no USB - which the original ESP32 is - still has to be able to
 * have a keyboard.  So the contract is about finding devices, pairing with
 * one, and turning what it sends into the events the console already has.
 * Audio, serial ports over RFCOMM and being a peripheral are all absent
 * because nothing above here asks for them.
 *
 * What a port must supply, when AG_PORT_HAS_BT is 1:
 *
 *   ag_err_t ag_port_bt_start(void)
 *   ag_err_t ag_port_bt_scan(ag_port_bt_dev_t *out, uint32_t max,
 *                            uint32_t *found, uint32_t seconds)
 *   ag_err_t ag_port_bt_open(const uint8_t addr[6], int addr_type)
 *   ag_err_t ag_port_bt_close(void)
 *   ag_err_t ag_port_bt_status(ag_port_bt_status_t *out)
 *   void     ag_port_bt_on_report(ag_port_bt_report_fn fn)
 *
 * Contract, not advice:
 *
 * - scan() blocks for `seconds` because a BLE scan is a listening window and
 *   there is nothing to return until it closes.  Devices that advertise more
 *   than once appear once.
 * - open() returns when the connection has been asked for, not when the
 *   keyboard is usable: pairing may want a passkey and service discovery takes
 *   a moment.  status() says where it got to.
 * - Reports arrive on the port's own task, one call per report, and the
 *   callback must be short: what the kernel does with it is put events in a
 *   queue.  A report is the raw HID payload, because deciding that byte 2 of
 *   an eight byte report is a keycode is the business of whoever knows the
 *   report descriptor, and that is the port.
 * - A keyboard that goes out of range disconnects and the port reconnects when
 *   it comes back, without being asked.  Nothing above here keeps a state
 *   machine for a radio link.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_BT_H
#define ARGON_PORT_BT_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

#include <argon/port/impl/bt.h>

#define AG_BT_NAME_MAX 31

/* What the scan saw.  `hid` is a guess from the advertisement, and a good one:
 * a keyboard says so in its appearance and its service list. */
typedef struct {
    char    name[AG_BT_NAME_MAX + 1];
    uint8_t addr[6];
    int     addr_type; /* the port's own numbering; pass back to open()     */
    int8_t  rssi;
    bool    hid;
    bool    bonded; /* paired before, so open() will not ask again          */
} ag_port_bt_dev_t;

typedef enum {
    AG_BT_OFF = 0,
    AG_BT_IDLE,
    AG_BT_SCANNING,
    AG_BT_OPENING, /* connecting, pairing or discovering services           */
    AG_BT_OPEN,    /* reports are arriving                                  */
} ag_bt_state_t;

typedef struct {
    ag_bt_state_t state;
    char          name[AG_BT_NAME_MAX + 1];
    uint8_t       addr[6];
    int8_t        rssi;
    uint32_t      reports; /* how many have arrived since it opened          */
} ag_port_bt_status_t;

/*
 * One HID input report.  `usage` is the port's classification of the device
 * (keyboard, mouse, other) so the kernel can route without parsing.
 */
typedef enum {
    AG_BT_USAGE_OTHER = 0,
    AG_BT_USAGE_KEYBOARD,
    AG_BT_USAGE_MOUSE,
} ag_bt_usage_t;

typedef void (*ag_port_bt_report_fn)(ag_bt_usage_t usage, uint8_t report_id,
                                     const uint8_t *data, uint32_t len);

#if AG_PORT_HAS_BT

ag_err_t ag_port_bt_start(void);
ag_err_t ag_port_bt_stop(void);
ag_err_t ag_port_bt_scan(ag_port_bt_dev_t *out, uint32_t max, uint32_t *found,
                         uint32_t seconds);
ag_err_t ag_port_bt_open(const uint8_t addr[6], int addr_type);
ag_err_t ag_port_bt_close(void);
ag_err_t ag_port_bt_status(ag_port_bt_status_t *out);
void     ag_port_bt_on_report(ag_port_bt_report_fn fn);

#endif /* AG_PORT_HAS_BT */

#endif /* ARGON_PORT_BT_H */
