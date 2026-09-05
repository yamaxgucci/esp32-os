/*
 * ArgonOS - just enough WebSocket to carry a finger.  See ag_ws.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ag_ws.h"

#include <string.h>

/* ------------------------------------------------------------------------ */
/* SHA-1 (FIPS 180-4)                                                       */
/* ------------------------------------------------------------------------ */

static uint32_t rol(uint32_t v, uint32_t n)
{
    return (v << n) | (v >> (32u - n));
}

static void sha1_block(ag_sha1_t *s, const uint8_t *p)
{
    uint32_t w[80];

    for (uint32_t i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (uint32_t i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];

    for (uint32_t i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }

    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

void ag_sha1_init(ag_sha1_t *s)
{
    s->h[0] = 0x67452301u;
    s->h[1] = 0xEFCDAB89u;
    s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xC3D2E1F0u;
    s->bits = 0;
    s->used = 0;
}

void ag_sha1_update(ag_sha1_t *s, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    s->bits += (uint64_t)len * 8u;
    while (len > 0) {
        const uint32_t room = 64u - s->used;
        const uint32_t take = (len < room) ? (uint32_t)len : room;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        len -= take;
        if (s->used == 64u) {
            sha1_block(s, s->block);
            s->used = 0;
        }
    }
}

void ag_sha1_final(ag_sha1_t *s, uint8_t out[20])
{
    const uint64_t bits = s->bits;
    uint8_t        pad = 0x80;

    ag_sha1_update(s, &pad, 1);
    pad = 0x00;
    while (s->used != 56u) {
        ag_sha1_update(s, &pad, 1);
    }
    /* The length goes in as it was before the padding, so it is captured
     * above rather than read back out of s->bits, which the padding moved. */
    uint8_t len[8];
    for (uint32_t i = 0; i < 8; i++) {
        len[i] = (uint8_t)(bits >> (56u - 8u * i));
    }
    ag_sha1_update(s, len, 8);

    for (uint32_t i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

/* ------------------------------------------------------------------------ */
/* base64                                                                   */
/* ------------------------------------------------------------------------ */

static const char k_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t ag_base64(const void *data, size_t len, char *out, size_t out_size)
{
    const uint8_t *p = (const uint8_t *)data;
    const size_t   need = 4u * ((len + 2u) / 3u);
    size_t         n = 0;

    if (out == NULL || out_size < need + 1u) {
        return 0;
    }

    for (size_t i = 0; i < len; i += 3) {
        const uint32_t b0 = p[i];
        const uint32_t b1 = (i + 1 < len) ? p[i + 1] : 0u;
        const uint32_t b2 = (i + 2 < len) ? p[i + 2] : 0u;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;

        out[n++] = k_b64[(v >> 18) & 0x3Fu];
        out[n++] = k_b64[(v >> 12) & 0x3Fu];
        out[n++] = (i + 1 < len) ? k_b64[(v >> 6) & 0x3Fu] : '=';
        out[n++] = (i + 2 < len) ? k_b64[v & 0x3Fu] : '=';
    }
    out[n] = '\0';
    return n;
}

/* ------------------------------------------------------------------------ */
/* The handshake                                                            */
/* ------------------------------------------------------------------------ */

/* RFC 6455 §1.3: the one constant in the protocol. */
static const char k_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

bool ag_ws_accept_key(const char *key, size_t key_len,
                      char out[AG_WS_ACCEPT_LEN])
{
    if (key == NULL || key_len == 0 || out == NULL) {
        return false;
    }

    ag_sha1_t s;
    uint8_t   digest[20];

    ag_sha1_init(&s);
    ag_sha1_update(&s, key, key_len);
    ag_sha1_update(&s, k_guid, sizeof(k_guid) - 1u);
    ag_sha1_final(&s, digest);

    return ag_base64(digest, sizeof(digest), out, AG_WS_ACCEPT_LEN) == 28u;
}

/* ------------------------------------------------------------------------ */
/* Frames                                                                   */
/* ------------------------------------------------------------------------ */

int32_t ag_ws_decode(uint8_t *buf, size_t len, ag_ws_frame_t *out)
{
    if (buf == NULL || out == NULL || len < 2u) {
        return 0;
    }

    const uint8_t b0 = buf[0];
    const uint8_t b1 = buf[1];
    const bool    fin = (b0 & 0x80u) != 0u;
    const uint8_t opcode = (uint8_t)(b0 & 0x0Fu);
    const bool    masked = (b1 & 0x80u) != 0u;
    uint32_t      plen = (uint32_t)(b1 & 0x7Fu);
    size_t        pos = 2u;

    if (plen == 126u) {
        if (len < 4u) {
            return 0;
        }
        plen = ((uint32_t)buf[2] << 8) | buf[3];
        pos = 4u;
    } else if (plen == 127u) {
        /*
         * A 64-bit length.  Nothing this protocol carries is that long, and
         * accepting one would mean growing a buffer on a remote party's say-so.
         */
        return AG_WS_ERR_TOOBIG;
    }

    /*
     * RFC 6455 §5.1: a client must mask, and a server that receives an unmasked
     * frame must fail the connection.  That is not pedantry - masking is what
     * stops a hostile page from steering a proxy into forging a request - so an
     * unmasked frame is an error rather than something to be lenient about.
     */
    if (!masked) {
        return AG_WS_ERR_UNMASKED;
    }
    if (plen > AG_WS_MAX_PAYLOAD) {
        return AG_WS_ERR_TOOBIG;
    }

    /* A control frame is never fragmented and never longer than 125 bytes. */
    if ((opcode & 0x08u) != 0u && (!fin || plen > 125u)) {
        return AG_WS_ERR_FRAGMENT;
    }
    /* Data frames: this carries lines, and a line fits in a frame. */
    if ((opcode & 0x08u) == 0u && (!fin || opcode == AG_WS_CONT)) {
        return AG_WS_ERR_FRAGMENT;
    }

    if (len < pos + 4u + plen) {
        return 0;
    }

    uint8_t mask[4];
    memcpy(mask, buf + pos, 4);
    pos += 4u;

    for (uint32_t i = 0; i < plen; i++) {
        buf[pos + i] ^= mask[i & 3u];
    }

    out->opcode = opcode;
    out->fin = fin;
    out->payload = buf + pos;
    out->len = plen;
    return (int32_t)(pos + plen);
}

uint32_t ag_ws_header(uint8_t *hdr, uint8_t opcode, size_t payload_len)
{
    if (hdr == NULL || payload_len > AG_WS_MAX_PAYLOAD) {
        return 0;
    }
    hdr[0] = (uint8_t)(0x80u | (opcode & 0x0Fu)); /* FIN, one frame */
    if (payload_len < 126u) {
        hdr[1] = (uint8_t)payload_len;
        return 2u;
    }
    hdr[1] = 126u;
    hdr[2] = (uint8_t)(payload_len >> 8);
    hdr[3] = (uint8_t)(payload_len & 0xFFu);
    return 4u;
}
