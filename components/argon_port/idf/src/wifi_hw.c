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

#include "net_hw.h"

/* What the radio is doing, kept here because nothing above wants to know. */
static volatile ag_wifi_state_t s_state = AG_WIFI_OFF;
static char                     s_ssid[AG_WIFI_SSID_MAX + 1];
static char                     s_pass[AG_WIFI_PASS_MAX + 1];
static volatile uint32_t        s_attempts;
static volatile int             s_last_reason;
static volatile bool            s_scanning;
static volatile bool            s_want_join;
static esp_netif_t             *s_sta_netif;

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
        if (s_want_join && !s_scanning) {
            s_state = AG_WIFI_JOINING;
            s_attempts++;
            (void)esp_wifi_connect();
        } else {
            s_state = AG_WIFI_IDLE;
        }
        break;
    }

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

ag_err_t ag_port_wifi_connect(const char *ssid, const char *pass)
{
    if (s_state == AG_WIFI_OFF) {
        return -AG_ENODEV;
    }
    if (ssid == NULL || ssid[0] == '\0') {
        return -AG_EINVAL;
    }

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", (pass != NULL) ? pass : "");

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

    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        return -AG_EINVAL;
    }

    s_want_join = true;
    s_attempts = 1;
    s_last_reason = 0;
    s_state = AG_WIFI_JOINING;

    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        s_state = AG_WIFI_IDLE;
        s_want_join = false;
        return -AG_EIO;
    }
    return AG_OK;
}

ag_err_t ag_port_wifi_disconnect(void)
{
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

    if (s_state == AG_WIFI_JOINED) {
        wifi_ap_record_t rec;
        if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
            out->rssi = rec.rssi;
            out->channel = rec.primary;
        }
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

#endif /* CONFIG_ARGON_NET_WIFI */
