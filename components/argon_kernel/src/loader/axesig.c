/*
 * ArgonOS - HMAC-SHA256-128 over .AXE images (header reserved[6]).
 *
 * Layout of reserved[6] (24 bytes, little-endian words):
 *   [0] algo    AG_AXE_SIG_ALGO_*
 *   [1] key_id  AG_AXE_SIG_KEY_*
 *   [2..5] tag  first 16 bytes of HMAC-SHA256
 *
 * MAC covers the whole file with those 24 bytes treated as zero, so the tag
 * does not cover itself.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/axesig.h>

#include <argon/axe.h>

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Development key only — replace for any real distribution path. */
static const uint8_t s_dev_key[32] = {
    'A', 'r', 'g', 'o', 'n', 'O', 'S', '-', 'd', 'e', 'v', '-', 'h', 'm',
    'a', 'c', '-', 'k', 'e', 'y', '-', 'v', '1', '!', '!', '!', '!', '!',
    '!', '!', '!', '!',
};

/* ---- compact SHA-256 (FIPS 180-4) -------------------------------------- */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   buflen;
} sha256_ctx_t;

static uint32_t rotr(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t block[64])
{
    static const uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
        0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
        0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
        0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
        0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
        0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
        0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
        0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };

    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 =
            rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 =
            rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + S1 + ch + K[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    while (len > 0) {
        const size_t n = (len < (64u - ctx->buflen)) ? len : (64u - ctx->buflen);
        memcpy(ctx->buf + ctx->buflen, data, n);
        ctx->buflen += n;
        data += n;
        len -= n;
        if (ctx->buflen == 64u) {
            sha256_transform(ctx, ctx->buf);
            ctx->bitlen += 512u;
            ctx->buflen = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[32])
{
    ctx->bitlen += (uint64_t)ctx->buflen * 8u;
    ctx->buf[ctx->buflen++] = 0x80u;
    if (ctx->buflen > 56u) {
        while (ctx->buflen < 64u) {
            ctx->buf[ctx->buflen++] = 0;
        }
        sha256_transform(ctx, ctx->buf);
        ctx->buflen = 0;
    }
    while (ctx->buflen < 56u) {
        ctx->buf[ctx->buflen++] = 0;
    }
    for (int i = 7; i >= 0; i--) {
        ctx->buf[ctx->buflen++] = (uint8_t)((ctx->bitlen >> (i * 8)) & 0xffu);
    }
    sha256_transform(ctx, ctx->buf);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

/*
 * MAC material = header_prefix ‖ 24 zero bytes ‖ file_tail
 * (reserved itself is not covered by the tag).
 */
static void hmac_axe_image(const uint8_t *file, size_t file_bytes,
                           const uint8_t *key, size_t key_len, uint8_t out[32])
{
    static const uint8_t zeros[24] = {0};
    const size_t   prefix = offsetof(ag_axe_header_t, reserved);
    const uint8_t *tail = file + sizeof(ag_axe_header_t);
    const size_t   tail_len =
        (file_bytes > sizeof(ag_axe_header_t))
            ? (file_bytes - sizeof(ag_axe_header_t))
            : 0;

    uint8_t k0[64];
    memset(k0, 0, sizeof(k0));
    if (key_len > 64u) {
        sha256_ctx_t t;
        sha256_init(&t);
        sha256_update(&t, key, key_len);
        sha256_final(&t, k0);
    } else {
        memcpy(k0, key, key_len);
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k0[i] ^ 0x36u);
        opad[i] = (uint8_t)(k0[i] ^ 0x5cu);
    }

    uint8_t inner[32];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    sha256_update(&ctx, file, prefix);
    sha256_update(&ctx, zeros, sizeof(zeros));
    if (tail_len > 0) {
        sha256_update(&ctx, tail, tail_len);
    }
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
}

static const uint8_t *key_for(uint32_t key_id, size_t *out_len)
{
    if (key_id == AG_AXE_SIG_KEY_DEV) {
        *out_len = sizeof(s_dev_key);
        return s_dev_key;
    }
    *out_len = 0;
    return NULL;
}

static bool reserved_is_unsigned(const ag_axe_header_t *h)
{
    for (int i = 0; i < 6; i++) {
        if (h->reserved[i] != 0u) {
            return false;
        }
    }
    return true;
}

static bool tag_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

ag_err_t ag_axe_check_sig(const void *file, size_t file_bytes)
{
    if (file == NULL) {
        return -AG_EINVAL;
    }
    if (file_bytes < sizeof(ag_axe_header_t)) {
        return -AG_EFORMAT;
    }

    const ag_axe_header_t *h = (const ag_axe_header_t *)file;
    if (reserved_is_unsigned(h)) {
        return AG_OK;
    }
    if (h->reserved[0] == AG_AXE_SIG_ALGO_NONE) {
        /* Non-zero reserved with algo 0 is garbage, not "unsigned". */
        return -AG_EFORMAT;
    }

    if (h->reserved[0] != AG_AXE_SIG_ALGO_HMAC_SHA256_128) {
        return -AG_ENOTSUP;
    }

    size_t         key_len = 0;
    const uint8_t *key = key_for(h->reserved[1], &key_len);
    if (key == NULL) {
        return -AG_ENOENT;
    }

    uint8_t mac[32];
    hmac_axe_image((const uint8_t *)file, file_bytes, key, key_len, mac);

    const uint8_t *tag = (const uint8_t *)&h->reserved[2];
    if (!tag_equal(tag, mac, AG_AXE_SIG_TAG_LEN)) {
        return -AG_EPERM;
    }
    return AG_OK;
}

ag_err_t ag_axe_sign(void *file, size_t file_bytes, uint32_t key_id)
{
    if (file == NULL || file_bytes < sizeof(ag_axe_header_t)) {
        return -AG_EINVAL;
    }

    size_t         key_len = 0;
    const uint8_t *key = key_for(key_id, &key_len);
    if (key == NULL) {
        return -AG_ENOENT;
    }

    ag_axe_header_t *h = (ag_axe_header_t *)file;
    memset(h->reserved, 0, sizeof(h->reserved));

    uint8_t mac[32];
    hmac_axe_image((const uint8_t *)file, file_bytes, key, key_len, mac);

    h->reserved[0] = AG_AXE_SIG_ALGO_HMAC_SHA256_128;
    h->reserved[1] = key_id;
    memcpy(&h->reserved[2], mac, AG_AXE_SIG_TAG_LEN);
    return AG_OK;
}
