/*
 * ArgonOS port: ESP-IDF - ESP-NOW, the radio talking to another board.
 *
 * The whole of it is a thin skin over esp_now: the interesting decisions are in
 * the contract (argon/port/espnow.h), and what is left here is turning the
 * IDF's shapes into the port's.  It rides the transceiver that wifi_hw.c starts;
 * it does not start one of its own.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_NET_ESPNOW) && CONFIG_ARGON_NET_ESPNOW

#include <argon/port/espnow.h>

#include <string.h>

#include "esp_now.h"
#include "esp_wifi.h"

static volatile bool          s_on;
static ag_port_espnow_recv_fn s_recv_fn;

/*
 * On the radio's task, one call per frame.  Everything it may do is written in
 * the contract: hand the bytes up and return.  The kernel's callback copies
 * them into a queue; nothing slow happens here.
 */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data,
                    int len)
{
    if (s_recv_fn != NULL && info != NULL && info->src_addr != NULL &&
        len >= 0) {
        s_recv_fn(info->src_addr, data, (uint32_t)len);
    }
}

void ag_port_espnow_on_recv(ag_port_espnow_recv_fn fn) { s_recv_fn = fn; }

ag_err_t ag_port_espnow_start(void)
{
    if (s_on) {
        return AG_OK;
    }
    /*
     * esp_now_init refuses until the radio is started, which from up here is
     * exactly "there is no device to do this on".
     */
    if (esp_now_init() != ESP_OK) {
        return -AG_ENODEV;
    }
    if (esp_now_register_recv_cb(&on_recv) != ESP_OK) {
        (void)esp_now_deinit();
        return -AG_EIO;
    }

    /*
     * The broadcast address as a peer, straight away.  A board has to be able to
     * be heard before anyone knows its address, and a frame to a peer that was
     * never added is simply dropped - so discovery would be impossible without
     * this one.
     */
    esp_now_peer_info_t bcast;
    memset(&bcast, 0, sizeof(bcast));
    memset(bcast.peer_addr, 0xff, sizeof(bcast.peer_addr));
    bcast.ifidx = WIFI_IF_STA;
    bcast.channel = 0;
    bcast.encrypt = false;
    (void)esp_now_add_peer(&bcast);

    s_on = true;
    return AG_OK;
}

ag_err_t ag_port_espnow_stop(void)
{
    if (!s_on) {
        return AG_OK;
    }
    (void)esp_now_unregister_recv_cb();
    (void)esp_now_deinit();
    s_on = false;
    return AG_OK;
}

ag_err_t ag_port_espnow_self(uint8_t out[6])
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    return (esp_wifi_get_mac(WIFI_IF_STA, out) == ESP_OK) ? AG_OK : -AG_ENODEV;
}

ag_err_t ag_port_espnow_peer_add(const uint8_t mac[6], uint8_t channel,
                                 const uint8_t *key)
{
    if (mac == NULL) {
        return -AG_EINVAL;
    }
    if (!s_on) {
        return -AG_ENODEV;
    }

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    peer.ifidx = WIFI_IF_STA;
    peer.channel = channel;
    if (key != NULL) {
        /* One key for the board, set the first time one is offered, and the
         * same key per peer - the simple shared-secret case two boards want.
         * A scheme with a different key per peer is a layer this does not try
         * to be. */
        (void)esp_now_set_pmk(key);
        memcpy(peer.lmk, key, AG_ESPNOW_KEY);
        peer.encrypt = true;
    }

    const esp_err_t err = esp_now_add_peer(&peer);
    if (err == ESP_ERR_ESPNOW_EXIST) {
        /* Already known: take the new channel/key rather than refuse, so that
         * "peer" twice is a way to change one, not an error. */
        return (esp_now_mod_peer(&peer) == ESP_OK) ? AG_OK : -AG_EIO;
    }
    return (err == ESP_OK) ? AG_OK : -AG_EIO;
}

ag_err_t ag_port_espnow_peer_del(const uint8_t mac[6])
{
    if (mac == NULL) {
        return -AG_EINVAL;
    }
    if (!s_on) {
        return -AG_ENODEV;
    }
    return (esp_now_del_peer(mac) == ESP_OK) ? AG_OK : -AG_ENOENT;
}

ag_err_t ag_port_espnow_send(const uint8_t mac[6], const void *data,
                             uint32_t len)
{
    if (mac == NULL || (data == NULL && len > 0u)) {
        return -AG_EINVAL;
    }
    if (!s_on) {
        return -AG_ENODEV;
    }
    if (len > AG_ESPNOW_MAX) {
        return -AG_EINVAL;
    }
    const esp_err_t err = esp_now_send(mac, (const uint8_t *)data, len);
    if (err == ESP_ERR_ESPNOW_NOT_FOUND) {
        return -AG_ENOENT; /* not a peer yet - peer_add first */
    }
    return (err == ESP_OK) ? AG_OK : -AG_EIO;
}

#endif /* CONFIG_ARGON_NET_ESPNOW */
