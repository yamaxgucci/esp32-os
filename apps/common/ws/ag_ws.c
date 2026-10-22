/*
 * ArgonOS - the WebSocket protocol, without a socket in sight.  See ag_ws.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "ag_ws.h"

#include <string.h>

/* ---- SHA-1 -------------------------------------------------------------- */

/*
 * Written out here rather than taken from mbedTLS, and that wants a reason
 * given the kernel already links one: this file is an application's, built by
 * mkaxe into a .SYS that has the ABI and nothing else.  The ABI offers SHA-256
 * (the SSH host key needs it) and not SHA-1, because SHA-1 is not a hash
 * anybody should be offered for a new purpose.
 *
 * It is the right hash for exactly this and for nothing else.  RFC 6455 froze
 * it into the handshake in 2011; it is a constant folded with a client-supplied
 * nonce, not a signature, and its collision resistance is not load-bearing.
 * Sixty lines here is cheaper than an ABI slot that would then exist for anyone
 * to misuse.
 */

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t  buf[64];
    uint32_t at;
} sha1_t;

static uint32_t rol(uint32_t v, unsigned n)
{
    return (v << n) | (v >> (32u - n));
}

static void sha1_block(sha1_t *s, const uint8_t *p)
{
    uint32_t w[80];

    for (uint32_t i = 0; i < 16u; i++) {
        w[i] = ((uint32_t)p[i * 4u] << 24) | ((uint32_t)p[i * 4u + 1u] << 16) |
               ((uint32_t)p[i * 4u + 2u] << 8) | (uint32_t)p[i * 4u + 3u];
    }
    for (uint32_t i = 16u; i < 80u; i++) {
        w[i] = rol(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1);
    }

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];

    for (uint32_t i = 0; i < 80u; i++) {
        uint32_t f, k;
        if (i < 20u) {
            f = (b & c) | (~b & d);
            k = 0x5a827999u;
        } else if (i < 40u) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60u) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
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

static void sha1_init(sha1_t *s)
{
    s->h[0] = 0x67452301u;
    s->h[1] = 0xefcdab89u;
    s->h[2] = 0x98badcfeu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xc3d2e1f0u;
    s->len = 0;
    s->at = 0;
}

static void sha1_update(sha1_t *s, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;

    s->len += (uint64_t)n;
    while (n > 0u) {
        const size_t take = (64u - s->at < n) ? (64u - s->at) : n;
        memcpy(s->buf + s->at, p, take);
        s->at += (uint32_t)take;
        p += take;
        n -= take;
        if (s->at == 64u) {
            sha1_block(s, s->buf);
            s->at = 0;
        }
    }
}

static void sha1_final(sha1_t *s, uint8_t out[20])
{
    const uint64_t bits = s->len * 8u;

    const uint8_t pad = 0x80u;
    sha1_update(s, &pad, 1u);
    const uint8_t zero = 0u;
    while (s->at != 56u) {
        sha1_update(s, &zero, 1u);
    }
    uint8_t tail[8];
    for (uint32_t i = 0; i < 8u; i++) {
        tail[i] = (uint8_t)(bits >> (56u - i * 8u));
    }
    sha1_update(s, tail, 8u);

    for (uint32_t i = 0; i < 5u; i++) {
        out[i * 4u] = (uint8_t)(s->h[i] >> 24);
        out[i * 4u + 1u] = (uint8_t)(s->h[i] >> 16);
        out[i * 4u + 2u] = (uint8_t)(s->h[i] >> 8);
        out[i * 4u + 3u] = (uint8_t)s->h[i];
    }
}

void ag_ws_sha1(const void *data, size_t len, uint8_t out[20])
{
    sha1_t s;

    if (out == NULL) {
        return;
    }
    sha1_init(&s);
    if (data != NULL && len > 0u) {
        sha1_update(&s, data, len);
    }
    sha1_final(&s, out);
}

/* ---- base64, encode only ------------------------------------------------ */

static const char k_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const uint8_t *in, uint32_t n, char *out)
{
    uint32_t at = 0;

    for (uint32_t i = 0; i < n; i += 3u) {
        const uint32_t b0 = in[i];
        const uint32_t b1 = (i + 1u < n) ? in[i + 1u] : 0u;
        const uint32_t b2 = (i + 2u < n) ? in[i + 2u] : 0u;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;

        out[at++] = k_b64[(v >> 18) & 0x3fu];
        out[at++] = k_b64[(v >> 12) & 0x3fu];
        out[at++] = (i + 1u < n) ? k_b64[(v >> 6) & 0x3fu] : '=';
        out[at++] = (i + 2u < n) ? k_b64[v & 0x3fu] : '=';
    }
    out[at] = '\0';
}

/* ---- the handshake answer ----------------------------------------------- */

bool ag_ws_accept_key(const char *key, char out[AG_WS_ACCEPT_LEN])
{
    /* RFC 6455's magic, and the only reason SHA-1 is in this file. */
    static const char k_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    if (key == NULL || out == NULL) {
        return false;
    }
    const size_t n = strlen(key);
    if (n == 0u || n > 128u) {
        return false;
    }

    sha1_t s;
    uint8_t digest[20];

    sha1_init(&s);
    sha1_update(&s, key, n);
    sha1_update(&s, k_guid, sizeof(k_guid) - 1u);
    sha1_final(&s, digest);

    b64_encode(digest, sizeof(digest), out);
    return true;
}

/* ---- frames ------------------------------------------------------------- */

int32_t ag_ws_hdr_parse(const uint8_t *in, uint32_t len, uint32_t max_payload,
                        ag_ws_hdr_t *out)
{
    if (in == NULL || out == NULL) {
        return -1;
    }
    if (len < 2u) {
        return 0;
    }

    const uint8_t b0 = in[0];
    const uint8_t b1 = in[1];

    /*
     * RSV1..3 must be zero without a negotiated extension, and nothing here
     * negotiates one.  A peer that sets them is speaking something this is not,
     * and carrying on would mean reading its payload as ours.
     */
    if (b0 & 0x70u) {
        return -1;
    }

    out->fin = (b0 & 0x80u) != 0u;
    out->opcode = (uint8_t)(b0 & 0x0fu);
    out->masked = (b1 & 0x80u) != 0u;

    uint32_t at = 2u;
    uint32_t plen = (uint32_t)(b1 & 0x7fu);

    if (plen == 126u) {
        if (len < at + 2u) {
            return 0;
        }
        plen = ((uint32_t)in[at] << 8) | in[at + 1u];
        at += 2u;
    } else if (plen == 127u) {
        if (len < at + 8u) {
            return 0;
        }
        /*
         * The high word must be zero.  Not because 64 bits are hard, but
         * because a length this side cannot hold is a length this side must not
         * try to allocate - and the honest answer to a peer claiming four
         * gigabytes is to close, not to truncate.
         */
        for (uint32_t i = 0; i < 4u; i++) {
            if (in[at + i] != 0u) {
                return -1;
            }
        }
        plen = ((uint32_t)in[at + 4u] << 24) | ((uint32_t)in[at + 5u] << 16) |
               ((uint32_t)in[at + 6u] << 8) | (uint32_t)in[at + 7u];
        at += 8u;
    }

    if (plen > max_payload) {
        return -1;
    }

    if (out->masked) {
        if (len < at + 4u) {
            return 0;
        }
        memcpy(out->mask, in + at, 4u);
        at += 4u;
    } else {
        memset(out->mask, 0, sizeof(out->mask));
    }

    out->len = plen;
    out->hdr = at;
    return (int32_t)at;
}

void ag_ws_unmask(uint8_t *p, uint32_t len, const uint8_t mask[4])
{
    if (p == NULL || mask == NULL) {
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        p[i] = (uint8_t)(p[i] ^ mask[i & 3u]);
    }
}

uint32_t ag_ws_hdr_build(uint8_t *out, uint8_t opcode, uint32_t len, bool fin)
{
    if (out == NULL) {
        return 0;
    }
    out[0] = (uint8_t)((fin ? 0x80u : 0u) | (opcode & 0x0fu));

    if (len < 126u) {
        out[1] = (uint8_t)len;
        return 2u;
    }
    if (len <= 0xffffu) {
        out[1] = 126u;
        out[2] = (uint8_t)(len >> 8);
        out[3] = (uint8_t)len;
        return 4u;
    }
    out[1] = 127u;
    out[2] = 0u;
    out[3] = 0u;
    out[4] = 0u;
    out[5] = 0u;
    out[6] = (uint8_t)(len >> 24);
    out[7] = (uint8_t)(len >> 16);
    out[8] = (uint8_t)(len >> 8);
    out[9] = (uint8_t)len;
    return 10u;
}

/* ---- the client half ---------------------------------------------------- */

bool ag_ws_client_key(const uint8_t nonce[16], char out[AG_WS_KEY_LEN])
{
    if (nonce == NULL || out == NULL) {
        return false;
    }
    b64_encode(nonce, 16u, out);  /* which terminates for us */
    return true;
}

bool ag_ws_accept_ok(const char *sent_key, const char *accept)
{
    char want[AG_WS_ACCEPT_LEN];

    if (accept == NULL || !ag_ws_accept_key(sent_key, want)) {
        return false;
    }
    /*
     * Length first, then content: strcmp on a header value a peer chose is one
     * missing terminator away from reading somebody else's memory, and this
     * runs against whatever answered the port.
     */
    const size_t n = strlen(accept);
    if (n != AG_WS_ACCEPT_LEN - 1u) {
        return false;
    }
    return memcmp(want, accept, n) == 0;
}

uint32_t ag_ws_hdr_build_masked(uint8_t *out, uint8_t opcode, uint32_t len,
                                bool fin, const uint8_t mask[4])
{
    if (out == NULL || mask == NULL) {
        return 0;
    }
    const uint32_t n = ag_ws_hdr_build(out, opcode, len, fin);
    if (n == 0u) {
        return 0;
    }
    out[1] |= 0x80u; /* the mask bit, which is what makes this a client */
    for (uint32_t i = 0; i < 4u; i++) {
        out[n + i] = mask[i];
    }
    return n + 4u;
}

void ag_ws_mask(uint8_t *p, uint32_t len, const uint8_t mask[4])
{
    ag_ws_unmask(p, len, mask);
}
