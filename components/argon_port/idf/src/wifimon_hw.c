/*
 * ArgonOS port: ESP-IDF - monitor mode and raw injection.
 *
 * A thin skin over the promiscuous half of esp_wifi and esp_wifi_80211_tx.
 * The contract (argon/port/wifimon.h) is where the one decision that matters
 * lives: this captures and injects, and it does not know or care what a frame
 * means.  What is here is turning the driver's shapes into the port's and
 * nothing else.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_NET_WIFI_MON) && CONFIG_ARGON_NET_WIFI_MON

#include <argon/port/wifimon.h>

#include "esp_wifi.h"

static volatile bool       s_on;
static ag_port_wifi_mon_fn s_frame_fn;

/*
 * On the radio's task, once per frame, and short: a busy channel is thousands
 * of frames a second.  All this does is find the length, strip the FCS the
 * radio already checked, and hand the frame up.
 */
static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)type;
    if (s_frame_fn == NULL || buf == NULL) {
        return;
    }
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    uint32_t                      len = (uint32_t)pkt->rx_ctrl.sig_len;
    /* sig_len counts the four-byte FCS the radio verified; the frame proper is
     * what is left once it is taken off. */
    len = (len >= 4u) ? (len - 4u) : 0u;
    s_frame_fn(pkt->payload, len, (int8_t)pkt->rx_ctrl.rssi,
               (uint8_t)pkt->rx_ctrl.channel);
}

void ag_port_wifi_mon_on_frame(ag_port_wifi_mon_fn fn) { s_frame_fn = fn; }

static uint32_t to_idf_filter(uint32_t mask)
{
    uint32_t f = 0;
    if (mask & AG_WIFIMON_MGMT) {
        f |= WIFI_PROMIS_FILTER_MASK_MGMT;
    }
    if (mask & AG_WIFIMON_CTRL) {
        f |= WIFI_PROMIS_FILTER_MASK_CTRL;
    }
    if (mask & AG_WIFIMON_DATA) {
        f |= WIFI_PROMIS_FILTER_MASK_DATA;
    }
    if (mask & AG_WIFIMON_MISC) {
        f |= WIFI_PROMIS_FILTER_MASK_MISC;
    }
    return (f != 0u) ? f : (uint32_t)WIFI_PROMIS_FILTER_MASK_ALL;
}

ag_err_t ag_port_wifi_mon_start(void)
{
    if (s_on) {
        return AG_OK;
    }
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        return -AG_ENODEV; /* radio not started - nothing to listen on */
    }
    (void)esp_wifi_set_promiscuous_rx_cb(&promisc_cb);

    /* Management and data by default: the frames a person watching the air is
     * usually there for, without the control-frame fabric drowning them. */
    const wifi_promiscuous_filter_t f = {.filter_mask =
                                             WIFI_PROMIS_FILTER_MASK_MGMT |
                                             WIFI_PROMIS_FILTER_MASK_DATA};
    (void)esp_wifi_set_promiscuous_filter(&f);

    s_on = true;
    return AG_OK;
}

ag_err_t ag_port_wifi_mon_stop(void)
{
    if (!s_on) {
        return AG_OK;
    }
    (void)esp_wifi_set_promiscuous_rx_cb(NULL);
    (void)esp_wifi_set_promiscuous(false);
    s_on = false;
    return AG_OK;
}

ag_err_t ag_port_wifi_mon_channel(uint8_t primary)
{
    if (primary < 1u || primary > 14u) {
        return -AG_EINVAL;
    }
    return (esp_wifi_set_channel(primary, WIFI_SECOND_CHAN_NONE) == ESP_OK)
               ? AG_OK
               : -AG_EIO;
}

uint8_t ag_port_wifi_mon_get_channel(void)
{
    uint8_t            primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) != ESP_OK) {
        return 0;
    }
    return primary;
}

ag_err_t ag_port_wifi_mon_filter(uint32_t mask)
{
    const wifi_promiscuous_filter_t f = {.filter_mask = to_idf_filter(mask)};
    return (esp_wifi_set_promiscuous_filter(&f) == ESP_OK) ? AG_OK : -AG_EIO;
}

ag_err_t ag_port_wifi_tx_raw(const void *frame, uint32_t len)
{
    if (frame == NULL || len == 0u || len > AG_WIFIMON_TX_MAX) {
        return -AG_EINVAL;
    }
    /*
     * WIFI_IF_STA, and en_sys_seq false so the sequence-control field goes out
     * as the caller built it rather than being rewritten by the system - which
     * is what a caller injecting a specific frame wants.
     */
    const esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, (int)len, false);
    if (err == ESP_OK) {
        return AG_OK;
    }
    if (err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED) {
        return -AG_ENODEV;
    }
    return -AG_EIO;
}

#endif /* CONFIG_ARGON_NET_WIFI_MON */
