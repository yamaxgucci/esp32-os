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
 *   ag_err_t ag_port_wifi_connect(const char *ssid, const char *pass,
 *                                 const uint8_t *bssid)
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
 * - connect() joins a *network*: bssid NULL means any access point answering to
 *   that name, which is what a network with more than one of them is for, and
 *   the radio is then free to take the best.  A caller that passes six bytes is
 *   asking for one particular access point and nothing else - worth having,
 *   because "the network" can mean two boxes on two channels with thirty
 *   decibels between them, and then which one the board took is the difference
 *   between a link that works and a link that half works.  It is not the
 *   default: a pinned board whose access point is switched off stays off the
 *   network rather than joining the other one.
 * - An empty pass, for the network the radio is already set to, means the key
 *   it already has rather than no key.  That is what lets a caller change the
 *   access point without the key passing through a console again; an open
 *   network is unaffected, having no key either way.
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
    uint8_t         bssid[6];   /* the access point actually joined, or 0s  */
    bool            pinned;     /* this access point was asked for by name  */
    int8_t          rssi;
    uint8_t         channel;
    uint32_t        attempts;   /* association attempts since the last join */
    int             last_reason; /* the port's own disconnect reason code   */
} ag_port_wifi_status_t;

/*
 * What the access point half is doing, when the board is running one.  Separate
 * from the station status above because the two are answers to different
 * questions - "which network did I join" against "what network am I offering" -
 * and on this chip a board can be doing both at once.
 */
typedef struct {
    bool     on;                        /* the point is up                  */
    char     ssid[AG_WIFI_SSID_MAX + 1];/* the name it is broadcasting      */
    uint8_t  channel;                   /* the one it is actually on        */
    bool     hidden;                    /* not naming itself in the beacon  */
    bool     secured;                   /* WPA2 with a key, not open         */
    uint32_t clients;                   /* stations associated right now    */
    uint32_t ip;                        /* the board's own address on it,
                                         * host-order IPv4 (192.168.4.1)     */
} ag_port_wifi_ap_status_t;

#if AG_PORT_HAS_WIFI

ag_err_t ag_port_wifi_start(void);
ag_err_t ag_port_wifi_stop(void);
ag_err_t ag_port_wifi_scan(ag_port_wifi_ap_t *out, uint32_t max,
                           uint32_t *found);
ag_err_t ag_port_wifi_connect(const char *ssid, const char *pass,
                              const uint8_t *bssid);
ag_err_t ag_port_wifi_disconnect(void);
ag_err_t ag_port_wifi_status(ag_port_wifi_status_t *out);

/* Human text for a disconnect reason, for the one line that reports it. */
const char *ag_port_wifi_reason(int reason);

/*
 * The other direction: a network this board offers, not one it joins.
 *
 * The file this contract opens by saying the radio is a machine that joins a
 * network "so that the network exists at all".  That is still the reason the
 * station half is the important one - but it is not the only thing a radio can
 * do, and a board with no other network in reach can make one: a phone joins
 * it, opens the card in a browser, and no router was involved.  That is a
 * service the machine offers, in the same sense a socket it listens on is, and
 * this is where the machine offers it.
 *
 * Available only when the build asked for it (AG_PORT_WIFI_HAS_AP): running a
 * point is a second netif and a DHCP server, a few kilobytes a board that only
 * joins networks has no reason to spend.  See argon/port/impl/wifi.h.
 *
 * Contract, not advice:
 *
 * - ap_start() needs the radio already started (ag_port_wifi_start, which
 *   ag_port_net_start does): it turns the point on, it does not turn the radio
 *   on.  -AG_ENODEV when the radio is off.
 * - The point and a joined network coexist, because a board that serves a page
 *   is often a board that also fetched it.  But this chip has one radio: while
 *   the station is associated the point is forced onto the station's channel,
 *   whatever channel was asked for, and there is nothing the driver can do about
 *   it but say so.  ap_status() reports the channel it ended up on.
 * - An empty pass is an open network - deliberately, and the caller was told.
 *   A key means WPA2, and a key is eight to sixty-three characters: shorter is
 *   -AG_EINVAL rather than an open point nobody meant to run.
 * - channel 0 means "pick one" (1); otherwise 1..13, and out of range is the
 *   driver's to clamp, not to refuse.
 * - The board's own address on the point is fixed (192.168.4.1) and does not
 *   arrive by DHCP - the board is the thing handing leases out.  So
 *   ag_port_net_ready(), which answers "did DHCP give me an address", stays
 *   false for an AP-only board, and that is correct: it has an address, it did
 *   not lease one.  ap_status() carries that address.
 * - ap_stop() gives the netif and its buffers back.  Stopping the radio
 *   (ag_port_wifi_stop) stops the point too; it does not leak.
 */
#if AG_PORT_WIFI_HAS_AP

ag_err_t ag_port_wifi_ap_start(const char *ssid, const char *pass,
                               uint8_t channel, bool hidden);
ag_err_t ag_port_wifi_ap_stop(void);
ag_err_t ag_port_wifi_ap_status(ag_port_wifi_ap_status_t *out);

#endif /* AG_PORT_WIFI_HAS_AP */

#endif /* AG_PORT_HAS_WIFI */

#endif /* ARGON_PORT_WIFI_H */
