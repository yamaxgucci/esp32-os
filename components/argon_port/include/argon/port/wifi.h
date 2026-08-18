/*
 * ArgonOS port contract - the radio, when the chip has one.
 *
 * Separate from net.h on purpose.  net.h is about having a network: an
 * interface that comes up, an address that arrives, and sockets.  This is
 * about the two things a radio adds to that and a cable does not - which
 * networks are within reach, and which one to join - and a board with an
 * Ethernet PHY implements the first header and not this one.
 *
 * What a port must supply, when AG_PORT_HAS_WIFI is 1:
 *
 *   ag_err_t ag_port_wifi_start(void)
 *   ag_err_t ag_port_wifi_scan(ag_port_wifi_ap_t *out, uint32_t max,
 *                              uint32_t *found)
 *   ag_err_t ag_port_wifi_connect(const char *ssid, const char *pass)
 *   ag_err_t ag_port_wifi_disconnect(void)
 *   ag_err_t ag_port_wifi_status(ag_port_wifi_status_t *out)
 *
 * Contract, not advice:
 *
 * - start() turns the radio on and does not join anything.  A board that comes
 *   up joining a network it was told about last week is a board that cannot be
 *   used to find out what went wrong.
 * - connect() returns as soon as the attempt has been made, not when it has
 *   succeeded: association takes seconds and DHCP takes longer.  ag_port_net_
 *   ready() is still the answer to "is there a network", and status() says how
 *   far along the radio is.
 * - A connection that drops is the port's business to retry.  Above this layer
 *   there is no state machine for it, and there should not be: the reason it
 *   dropped is a radio fact.
 * - scan() blocks - it takes a second or two, because it has to - and returns
 *   the number found, which may be more than `max`.  It is not callable while
 *   an association attempt is in flight; that answers -AG_EBUSY.
 * - The password is never read back.  status() carries the SSID and never the
 *   key, because everything that asks for status prints it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_WIFI_H
#define ARGON_PORT_WIFI_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

#include <argon/port/impl/wifi.h>

#define AG_WIFI_SSID_MAX 32
#define AG_WIFI_PASS_MAX 63

/* How a network is protected.  Ordered by nothing; compared by equality. */
typedef enum {
    AG_WIFI_OPEN = 0,
    AG_WIFI_WEP,
    AG_WIFI_WPA,
    AG_WIFI_WPA2,
    AG_WIFI_WPA3,
    AG_WIFI_ENTERPRISE,
    AG_WIFI_AUTH_OTHER,
} ag_wifi_auth_t;

typedef struct {
    char           ssid[AG_WIFI_SSID_MAX + 1];
    uint8_t        bssid[6];
    int8_t         rssi;    /* dBm, negative                               */
    uint8_t        channel;
    ag_wifi_auth_t auth;
} ag_port_wifi_ap_t;

typedef enum {
    AG_WIFI_OFF = 0,   /* radio not started                                */
    AG_WIFI_IDLE,      /* on, not joined to anything                       */
    AG_WIFI_JOINING,   /* association or DHCP in progress                  */
    AG_WIFI_JOINED,    /* associated; an address may still be coming       */
} ag_wifi_state_t;

typedef struct {
    ag_wifi_state_t state;
    char            ssid[AG_WIFI_SSID_MAX + 1]; /* joined or being joined  */
    int8_t          rssi;
    uint8_t         channel;
    uint32_t        attempts;   /* association attempts since the last join */
    int             last_reason; /* the port's own disconnect reason code   */
} ag_port_wifi_status_t;

#if AG_PORT_HAS_WIFI

ag_err_t ag_port_wifi_start(void);
ag_err_t ag_port_wifi_scan(ag_port_wifi_ap_t *out, uint32_t max,
                           uint32_t *found);
ag_err_t ag_port_wifi_connect(const char *ssid, const char *pass);
ag_err_t ag_port_wifi_disconnect(void);
ag_err_t ag_port_wifi_status(ag_port_wifi_status_t *out);

/* Human text for a disconnect reason, for the one line that reports it. */
const char *ag_port_wifi_reason(int reason);

#endif /* AG_PORT_HAS_WIFI */

#endif /* ARGON_PORT_WIFI_H */
