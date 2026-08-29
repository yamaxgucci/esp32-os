/*
 * ArgonOS port: ESP-IDF - the radio.
 *
 * Station mode only, and that is not a limitation being apologised for: an
 * access point is a service a machine offers, and this is a machine that
 * joins one so that the network exists at all.  Everything above here sees
 * ag_port_net_* and cannot tell what the interface is made of.
 *
 * The link half of ag_port_net_start lives in net_hw.c and dispatches here
 * when this backend is built.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_NET_WIFI) && CONFIG_ARGON_NET_WIFI

#include <argon/port/wifi.h>

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#if AG_PORT_WIFI_HAS_AP
#include "lwip/inet.h" /* ntohl, for the point's own address in ap_status */
#endif

#include "net_hw.h"

/* What the radio is doing, kept here because nothing above wants to know. */
static volatile ag_wifi_state_t s_state = AG_WIFI_OFF;
static char                     s_ssid[AG_WIFI_SSID_MAX + 1];
static char                     s_pass[AG_WIFI_PASS_MAX + 1];
static volatile uint32_t        s_attempts;
static volatile int             s_last_reason;
static volatile bool            s_scanning;
static volatile bool            s_want_join;
/* One particular access point, when the caller named one.  Kept because the
 * retry has to ask for the same thing the first attempt did. */
static uint8_t                  s_bssid[6];
static volatile bool            s_bssid_set;
/* Set while a deliberate change of network or access point is under way, so
 * that the disconnect it starts is not treated as one to recover from. */
static volatile bool            s_switching;
static esp_netif_t             *s_sta_netif;

#if AG_PORT_WIFI_HAS_AP
/*
 * The access point half.  Its netif is made the first time a point is asked
 * for and given back when the last one stops, because it is the netif and its
 * DHCP server that cost the memory - the code is free once the radio is linked.
 */
static esp_netif_t     *s_ap_netif;
static volatile bool    s_ap_on;
static char             s_ap_ssid[AG_WIFI_SSID_MAX + 1];
static volatile uint8_t s_ap_channel;
static volatile bool    s_ap_hidden;
static volatile bool    s_ap_secured;
static volatile uint32_t s_ap_clients;

/*
 * The mode the one radio must be in for what is wanted right now.  There is a
 * single transceiver, so "station" and "access point" are not two devices to
 * turn on independently - they are one device told to do one thing, the other,
 * or both, and every place that changes what is wanted has to say so here.
 */
static wifi_mode_t desired_mode(void)
{
    if (s_ap_on && s_want_join) {
        return WIFI_MODE_APSTA;
    }
    if (s_ap_on) {
        return WIFI_MODE_AP;
    }
    return WIFI_MODE_STA;
}

static esp_err_t apply_mode(void) { return esp_wifi_set_mode(desired_mode()); }
#endif /* AG_PORT_WIFI_HAS_AP */

static ag_wifi_auth_t map_auth(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return AG_WIFI_OPEN;
    case WIFI_AUTH_WEP:             return AG_WIFI_WEP;
    case WIFI_AUTH_WPA_PSK:         return AG_WIFI_WPA;
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:    return AG_WIFI_WPA2;
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:   return AG_WIFI_WPA3;
    case WIFI_AUTH_WPA2_ENTERPRISE: return AG_WIFI_ENTERPRISE;
    default:                        return AG_WIFI_AUTH_OTHER;
    }
}

/*
 * Retrying is the port's job (see the contract in argon/port/wifi.h), and the
 * retry is immediate rather than backed off: the two reasons an association
 * fails on a board are a wrong key, which no delay will fix and which the
 * reason code names, and an access point that has not finished booting, which
 * a second attempt fixes.  What must not happen is a silent loop, so the count
 * is kept and status() reports it.
 */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                          void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        s_state = s_want_join ? AG_WIFI_JOINING : AG_WIFI_IDLE;
        if (s_want_join) {
            s_attempts++;
            (void)esp_wifi_connect();
        }
        break;

    case WIFI_EVENT_STA_CONNECTED:
        s_state = AG_WIFI_JOINED;
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ev =
            (const wifi_event_sta_disconnected_t *)data;
        if (ev != NULL) {
            s_last_reason = ev->reason;
        }
        ag_port_net_link_down();
        if (s_want_join && !s_scanning && !s_switching) {
            s_state = AG_WIFI_JOINING;
            s_attempts++;
            (void)esp_wifi_connect();
        } else {
            s_state = AG_WIFI_IDLE;
        }
        break;
    }

#if AG_PORT_WIFI_HAS_AP
    /*
     * How many stations are on the point right now, kept by counting the ones
     * that join and leave rather than asking the driver each time status is
     * read.  The count is the one thing about a point that changes without
     * anyone here doing anything, so it is the one thing worth an event.
     */
    case WIFI_EVENT_AP_STACONNECTED:
        s_ap_clients++;
        break;

    case WIFI_EVENT_AP_STADISCONNECTED:
        if (s_ap_clients > 0) {
            s_ap_clients--;
        }
        break;
#endif

    default:
        break;
    }
}

ag_err_t ag_port_wifi_start(void)
{
    if (s_state != AG_WIFI_OFF) {
        return AG_OK;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        return -AG_ENOMEM;
    }
    ag_port_net_set_netif(s_sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        return -AG_ENOMEM;
    }

    (void)esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     &on_wifi_event, NULL);

    /*
     * The calibration data and the last network live in NVS by default.  The
     * first is worth keeping - it saves a second of every start - and the
     * second is not: this system decides what to join from its own config, and
     * a radio that remembers something else joins it before anyone has asked.
     */
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        return -AG_EIO;
    }
    if (esp_wifi_start() != ESP_OK) {
        return -AG_EIO;
    }

    /* ESP-IDF narrates association at INFO; the kernel prints its own line. */
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);

    if (s_state == AG_WIFI_OFF) {
        s_state = AG_WIFI_IDLE;
    }
    return AG_OK;
}

/*
 * Off, and giving the memory back.
 *
 * This is what makes both radios possible on a chip that cannot afford both:
 * linked is cheap, running is not.  esp_wifi_deinit releases the buffers, the
 * task stacks and the driver state - about forty kilobytes - and what is left
 * is the code in flash, which costs no RAM at all.  The netif and the event
 * loop stay: they belong to whatever link is up next, and rebuilding them is
 * where the ordering bugs live.
 */
ag_err_t ag_port_wifi_stop(void)
{
    if (s_state == AG_WIFI_OFF) {
        return AG_OK;
    }
    s_want_join = false;
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    s_bssid_set = false;

#if AG_PORT_WIFI_HAS_AP
    /* The point goes down with the radio, or its netif would outlive the driver
     * that feeds it - a leak the next `wifi on` cannot undo. */
    s_ap_on = false;
    s_ap_clients = 0;
    s_ap_ssid[0] = '\0';
#endif

    (void)esp_wifi_disconnect();
    ag_port_net_link_down();
    (void)esp_wifi_stop();
    if (esp_wifi_deinit() != ESP_OK) {
        return -AG_EIO;
    }
    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
        ag_port_net_set_netif(NULL);
    }
#if AG_PORT_WIFI_HAS_AP
    if (s_ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
#endif

    s_state = AG_WIFI_OFF;
    s_attempts = 0;
    s_last_reason = 0;
    return AG_OK;
}

ag_err_t ag_port_wifi_scan(ag_port_wifi_ap_t *out, uint32_t max,
                           uint32_t *found)
{
    if (s_state == AG_WIFI_OFF) {
        return -AG_ENODEV;
    }
    if (s_scanning) {
        return -AG_EBUSY;
    }

    /*
     * Scanning while an association is in flight makes the radio leave the
     * channel it is trying to associate on, and the attempt fails with a
     * reason code that says nothing about the real cause.  Say no instead.
     */
    if (s_state == AG_WIFI_JOINING) {
        return -AG_EBUSY;
    }

    s_scanning = true;
    const esp_err_t err = esp_wifi_scan_start(NULL, true); /* blocking */
    if (err != ESP_OK) {
        s_scanning = false;
        return -AG_EIO;
    }

    /*
     * How many first, records second, and in that order: fetching the records
     * releases the scan result list, so asking for the count afterwards
     * answers zero.  It looks exactly like a scan that found nothing, on a
     * desk surrounded by access points.
     */
    uint16_t total = 0;
    (void)esp_wifi_scan_get_ap_num(&total);
    if (found != NULL) {
        *found = total;
    }

    uint16_t got = (uint16_t)((max > 0xffffu) ? 0xffffu : max);
    wifi_ap_record_t recs[16];
    if (got > (uint16_t)(sizeof(recs) / sizeof(recs[0]))) {
        got = (uint16_t)(sizeof(recs) / sizeof(recs[0]));
    }
    if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) {
        s_scanning = false;
        return -AG_EIO;
    }

    for (uint16_t i = 0; i < got && i < max; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        memcpy(out[i].ssid, recs[i].ssid, AG_WIFI_SSID_MAX);
        out[i].ssid[AG_WIFI_SSID_MAX] = '\0';
        memcpy(out[i].bssid, recs[i].bssid, sizeof(out[i].bssid));
        out[i].rssi = recs[i].rssi;
        out[i].channel = recs[i].primary;
        out[i].auth = map_auth(recs[i].authmode);
    }

    s_scanning = false;
    return AG_OK;
}

ag_err_t ag_port_wifi_connect(const char *ssid, const char *pass,
                              const uint8_t *bssid)
{
    if (s_state == AG_WIFI_OFF) {
        return -AG_ENODEV;
    }
    if (ssid == NULL || ssid[0] == '\0') {
        return -AG_EINVAL;
    }

    /*
     * Let go of the current access point first.
     *
     * The driver refuses outright otherwise - "sta is connected, disconnect
     * before connecting to new ap" - and the refusal is a log line, not an
     * error return, so from up here the command looks as if it worked and the
     * board stays where it was.  That is exactly how the first attempt at
     * pinning to a nearer access point did nothing at all.
     *
     * The disconnect this causes is ours, so the retry in the event handler is
     * held off until the new association has been asked for.
     */
    if (s_state == AG_WIFI_JOINED || s_state == AG_WIFI_JOINING) {
        s_switching = true;
        (void)esp_wifi_disconnect();
    }

#if AG_PORT_WIFI_HAS_AP
    /*
     * A point that is up stays up.  Joining a network while running one means
     * both at once, and the mode has to say so before the station is configured
     * below: ap_start with nothing joined left the radio in AP-only mode, and
     * setting a station config in that mode is setting a station the radio is
     * not in the mode to have.
     */
    if (s_ap_on) {
        (void)esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
#endif

    /*
     * An empty key means "the one you already have", when the network is the
     * same one.  That is what makes it possible to change *which access point*
     * without the key being typed again - and on a board whose screen is in a
     * room with people in it, not typing it again is the point.
     *
     * Safe for an open network: there the radio has no key either, and an empty
     * one is what it keeps.
     */
    const bool same_net = (strcmp(s_ssid, ssid) == 0);
    const bool keep_key = same_net && (pass == NULL || pass[0] == '\0') &&
                          (s_pass[0] != '\0');

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    if (!keep_key) {
        snprintf(s_pass, sizeof(s_pass), "%s", (pass != NULL) ? pass : "");
    }
    s_bssid_set = (bssid != NULL);
    if (bssid != NULL) {
        memcpy(s_bssid, bssid, sizeof(s_bssid));
    }

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /*
     * Copied, not printed into: these fields are exactly 32 and 64 bytes and
     * hold no terminator, so a name of the full length is a name and not an
     * overflow.  The struct was zeroed above, which is what ends a shorter one.
     */
    memcpy(cfg.sta.ssid, s_ssid, strnlen(s_ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, s_pass, strnlen(s_pass, sizeof(cfg.sta.password)));
    /*
     * Any authentication at all, including none.  Pinning a minimum here is a
     * policy decision, and the place for it is the network's own settings: a
     * board that refuses to join the network it was pointed at, without saying
     * why, is worse than one that joins an open network it was told to join.
     */
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    /*
     * Pinned or not.  With bssid_set the driver will associate with that one
     * access point and nothing else - including on every retry, which is the
     * point: a board pinned to the strong box next door must not drift back to
     * the weak one two rooms away the first time the link stutters.
     */
    if (s_bssid_set) {
        cfg.sta.bssid_set = true;
        memcpy(cfg.sta.bssid, s_bssid, sizeof(cfg.sta.bssid));
    }

    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        s_switching = false;
        return -AG_EINVAL;
    }

    s_want_join = true;
    s_attempts = 1;
    s_last_reason = 0;
    s_state = AG_WIFI_JOINING;

    const esp_err_t err = esp_wifi_connect();

    /*
     * The new attempt is in flight, so a disconnect from here on is a real one
     * and the handler may recover from it again.  A late event from the
     * disconnect above finds this clear and retries - which is harmless, since
     * what it retries is the configuration just installed.
     */
    s_switching = false;

    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        s_state = AG_WIFI_IDLE;
        s_want_join = false;
        return -AG_EIO;
    }
    return AG_OK;
}

ag_err_t ag_port_wifi_disconnect(void)
{
    s_bssid_set = false;
    if (s_state == AG_WIFI_OFF) {
        return -AG_ENODEV;
    }
    s_want_join = false;
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    (void)esp_wifi_disconnect();
    ag_port_net_link_down();
    s_state = AG_WIFI_IDLE;
    return AG_OK;
}

ag_err_t ag_port_wifi_status(ag_port_wifi_status_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    out->attempts = s_attempts;
    out->last_reason = s_last_reason;
    snprintf(out->ssid, sizeof(out->ssid), "%s", s_ssid);
    out->pinned = s_bssid_set;

    if (s_state == AG_WIFI_JOINED) {
        wifi_ap_record_t rec;
        if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
            out->rssi = rec.rssi;
            out->channel = rec.primary;
            /* Which box answered, which is not the same question as which
             * network: two of them can share one name. */
            memcpy(out->bssid, rec.bssid, sizeof(out->bssid));
        }
    } else if (s_bssid_set) {
        memcpy(out->bssid, s_bssid, sizeof(out->bssid));
    }
    return AG_OK;
}

const char *ag_port_wifi_reason(int reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "wrong key, or the network refused this station";
    case WIFI_REASON_NO_AP_FOUND:
        return "no access point with that name is in range";
    case WIFI_REASON_ASSOC_LEAVE:
        return "left the network";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "the access point stopped answering";
    case 0:
        return "";
    default:
        return "the radio gave up";
    }
}

#if AG_PORT_WIFI_HAS_AP

/*
 * Turn the point on, or change what it is.
 *
 * Calling it again with a different name or key reconfigures the running point
 * rather than refusing: the netif is already there, the driver takes the new
 * config, and clients reassociate.  That is why the netif is kept between calls
 * and only the config is rewritten.
 */
ag_err_t ag_port_wifi_ap_start(const char *ssid, const char *pass,
                               uint8_t channel, bool hidden)
{
    if (s_state == AG_WIFI_OFF) {
        return -AG_ENODEV;
    }
    if (ssid == NULL || ssid[0] == '\0') {
        return -AG_EINVAL;
    }

    const size_t plen =
        (pass != NULL) ? strnlen(pass, AG_WIFI_PASS_MAX + 1u) : 0u;
    const bool secured = (plen > 0u);
    /*
     * Eight is WPA2's floor and sixty-three its ceiling.  A key of one to seven
     * characters is a mistake, not an open network, and turning it into one
     * silently is how a point meant to be private ends up not being.
     */
    if (secured && (plen < 8u || plen > AG_WIFI_PASS_MAX)) {
        return -AG_EINVAL;
    }

    const bool created_now = (s_ap_netif == NULL);
    if (created_now) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            return -AG_ENOMEM;
        }
    }

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* Copied, not printed: ssid is a fixed field with a separate length and no
     * terminator, exactly as the station side above. */
    const size_t sl = strnlen(ssid, sizeof(cfg.ap.ssid));
    memcpy(cfg.ap.ssid, ssid, sl);
    cfg.ap.ssid_len = (uint8_t)sl;
    cfg.ap.channel = (channel == 0u) ? 1u : channel;
    cfg.ap.ssid_hidden = hidden ? 1 : 0;
    /*
     * Four at once.  Every associated station is a lease and a receive window's
     * worth of memory in flight, and this chip has already been run out of heap
     * by a browser opening six sockets - a point that lets ten devices on is a
     * point that dies when they arrive.  Four is a phone, a laptop and room.
     */
    cfg.ap.max_connection = 4;
    if (secured) {
        memcpy(cfg.ap.password, pass, plen);
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    s_ap_on = true;
    if (apply_mode() != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &cfg) != ESP_OK) {
        /* Undo cleanly: a half-started point that reports itself on is worse
         * than one that failed and said so. */
        s_ap_on = false;
        (void)apply_mode();
        if (created_now) {
            esp_netif_destroy_default_wifi(s_ap_netif);
            s_ap_netif = NULL;
        }
        return -AG_EIO;
    }

    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", ssid);
    s_ap_channel = cfg.ap.channel;
    s_ap_hidden = hidden;
    s_ap_secured = secured;
    s_ap_clients = 0;
    return AG_OK;
}

ag_err_t ag_port_wifi_ap_stop(void)
{
    if (!s_ap_on) {
        return AG_OK;
    }
    s_ap_on = false;
    (void)apply_mode(); /* back to station-only, or station if one is joined */

    if (s_ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    s_ap_clients = 0;
    s_ap_ssid[0] = '\0';
    return AG_OK;
}

ag_err_t ag_port_wifi_ap_status(ag_port_wifi_ap_status_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->on = s_ap_on;
    if (!s_ap_on) {
        return AG_OK;
    }

    snprintf(out->ssid, sizeof(out->ssid), "%s", s_ap_ssid);
    out->hidden = s_ap_hidden;
    out->secured = s_ap_secured;
    out->clients = s_ap_clients;

    /*
     * The channel it is really on, not the one that was asked for.  With a
     * station associated the driver drags the point onto the station's channel,
     * and asking the driver is the only way to know which that turned out to be.
     */
    uint8_t            primary = s_ap_channel;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK && primary != 0u) {
        out->channel = primary;
    } else {
        out->channel = s_ap_channel;
    }

    if (s_ap_netif != NULL) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
            out->ip = ntohl(ip.ip.addr);
        }
    }
    return AG_OK;
}

#endif /* AG_PORT_WIFI_HAS_AP */

#endif /* CONFIG_ARGON_NET_WIFI */
