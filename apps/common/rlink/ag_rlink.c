/*
 * ArgonOS - RLINK codec.  See ag_rlink.h for the protocol; this file is only
 * the byte layout, and it is deliberately the only place that knows it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ag_rlink.h"

#include <string.h>

/* Little-endian put/get, done by hand so the layout does not depend on the
 * host's struct packing or byte order. */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void ag_rlink_pack(uint8_t out[RL_HDR_SIZE], const ag_rlink_hdr_t *h)
{
    put_u32(out + 0, RL_MAGIC);
    out[4] = h->op;
    out[5] = h->flags;
    put_u16(out + 6, h->seq);
    put_u32(out + 8, (uint32_t)h->status);
    put_u32(out + 12, (uint32_t)h->ch);
    put_u32(out + 16, h->a0);
    put_u32(out + 20, h->a1);
    put_u32(out + 24, h->len);
}

bool ag_rlink_unpack(ag_rlink_hdr_t *h, const uint8_t in[RL_HDR_SIZE])
{
    const uint32_t magic = get_u32(in + 0);
    if (magic != RL_MAGIC) {
        return false;
    }
    const uint32_t len = get_u32(in + 24);
    if (len > RL_MAX_PAYLOAD) {
        return false;
    }
    h->magic = magic;
    h->op = in[4];
    h->flags = in[5];
    h->seq = get_u16(in + 6);
    h->status = (int32_t)get_u32(in + 8);
    h->ch = (int32_t)get_u32(in + 12);
    h->a0 = get_u32(in + 16);
    h->a1 = get_u32(in + 20);
    h->len = len;
    return true;
}

void ag_rlink_req(ag_rlink_hdr_t *h, uint8_t op, uint16_t seq)
{
    memset(h, 0, sizeof(*h));
    h->magic = RL_MAGIC;
    h->op = op;
    h->seq = seq;
    h->ch = -1;
}

void ag_rlink_req_connect(ag_rlink_hdr_t *h, uint16_t seq, uint32_t addr,
                          uint16_t port, uint32_t timeout_ms)
{
    ag_rlink_req(h, RL_OP_CONNECT, seq);
    h->a0 = addr;
    h->ch = (int32_t)port;
    h->a1 = timeout_ms;
}

void ag_rlink_req_accept(ag_rlink_hdr_t *h, uint16_t seq, int32_t listen_ch,
                         uint32_t timeout_ms)
{
    ag_rlink_req(h, RL_OP_ACCEPT, seq);
    h->ch = listen_ch;
    h->a1 = timeout_ms;
}

void ag_rlink_req_send(ag_rlink_hdr_t *h, uint16_t seq, int32_t ch,
                       uint32_t len)
{
    ag_rlink_req(h, RL_OP_SEND, seq);
    h->ch = ch;
    h->len = len;
}

/* ------------------------------------------------------------------------ */
/* The station half                                                         */
/* ------------------------------------------------------------------------ */

/*
 * A fixed-width text field: `n` bytes, NUL-padded, never NUL-counted.  Copying
 * with strncpy would be the same bytes and a compiler warning about the
 * truncation that is the point here, so it is spelled out.
 */
static void put_field(uint8_t *p, size_t n, const char *s)
{
    size_t i = 0;
    if (s != NULL) {
        for (; i < n && s[i] != '\0'; i++) {
            p[i] = (uint8_t)s[i];
        }
    }
    for (; i < n; i++) {
        p[i] = 0;
    }
}

/* The reverse, terminating for the reader whatever the far end padded with. */
static void get_field(char *out, size_t cap, const uint8_t *p, size_t n)
{
    const size_t take = (n < cap - 1u) ? n : cap - 1u;
    memcpy(out, p, take);
    out[take] = '\0';
}

/* True when every byte of `s` fits in a field of `n` (its NUL need not). */
static bool fits(const char *s, size_t n)
{
    if (s == NULL) {
        return true;
    }
    size_t i = 0;
    while (s[i] != '\0') {
        if (++i > n) {
            return false;
        }
    }
    return true;
}

void ag_rlink_pack_ap(uint8_t out[RL_AP_SIZE], const ag_rlink_ap_t *ap)
{
    put_field(out + 0, RL_SSID_MAX + 1u, ap->ssid);
    memcpy(out + 33, ap->bssid, 6);
    out[39] = (uint8_t)ap->rssi;
    out[40] = ap->channel;
    out[41] = ap->auth;
}

bool ag_rlink_unpack_ap(ag_rlink_ap_t *ap, const uint8_t in[RL_AP_SIZE])
{
    get_field(ap->ssid, sizeof(ap->ssid), in + 0, RL_SSID_MAX + 1u);
    memcpy(ap->bssid, in + 33, 6);
    ap->rssi = (int8_t)in[39];
    ap->channel = in[40];
    /* AG_WIFI_SEC_OTHER (6) is the enum's own answer for "something else". */
    ap->auth = (in[41] <= 6u) ? in[41] : 6u;
    return true;
}

void ag_rlink_pack_status(uint8_t out[RL_STATUS_SIZE],
                          const ag_rlink_status_t *st)
{
    out[0] = st->state;
    put_field(out + 1, RL_SSID_MAX + 1u, st->ssid);
    memcpy(out + 34, st->bssid, 6);
    out[40] = st->pinned ? 1u : 0u;
    out[41] = (uint8_t)st->rssi;
    out[42] = st->channel;
    put_u32(out + 43, st->attempts);
    put_u32(out + 47, (uint32_t)st->last_reason);
}

bool ag_rlink_unpack_status(ag_rlink_status_t *st,
                            const uint8_t in[RL_STATUS_SIZE])
{
    /* AG_WIFI_ST_* is closed at four; a fifth is a coprocessor this build does
     * not understand, and there is no state to fall back to that is not a lie. */
    if (in[0] > 3u) {
        return false;
    }
    st->state = in[0];
    get_field(st->ssid, sizeof(st->ssid), in + 1, RL_SSID_MAX + 1u);
    memcpy(st->bssid, in + 34, 6);
    st->pinned = in[40] ? 1u : 0u;
    st->rssi = (int8_t)in[41];
    st->channel = in[42];
    st->attempts = get_u32(in + 43);
    st->last_reason = (int32_t)get_u32(in + 47);
    return true;
}

bool ag_rlink_pack_join(uint8_t out[RL_JOIN_SIZE], const char *ssid,
                        const char *pass, const uint8_t bssid[6])
{
    /*
     * Refuse rather than truncate.  A truncated key is not a shorter key: it
     * joins nothing, and it does it looking exactly like a wrong password, so
     * the person retypes the right one for as long as their patience lasts.
     */
    if (ssid == NULL || ssid[0] == '\0' || !fits(ssid, RL_SSID_MAX) ||
        !fits(pass, RL_PASS_MAX)) {
        return false;
    }
    put_field(out + 0, RL_SSID_MAX + 1u, ssid);
    put_field(out + 33, RL_PASS_MAX + 1u, pass);
    if (bssid != NULL) {
        memcpy(out + 97, bssid, 6);
    } else {
        memset(out + 97, 0, 6);
    }
    return true;
}

bool ag_rlink_unpack_join(char ssid[RL_SSID_MAX + 1], char pass[RL_PASS_MAX + 1],
                          uint8_t bssid[6], const uint8_t in[RL_JOIN_SIZE])
{
    get_field(ssid, RL_SSID_MAX + 1u, in + 0, RL_SSID_MAX + 1u);
    get_field(pass, RL_PASS_MAX + 1u, in + 33, RL_PASS_MAX + 1u);
    memcpy(bssid, in + 97, 6);
    return ssid[0] != '\0';
}
