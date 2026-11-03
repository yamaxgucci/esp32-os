/*
 * ArgonOS - SHA-256 (FIPS 180-4), streaming - the kernel's copy.
 *
 * A kernel library since ABI 0.71: an image cannot afford its own, and
 * this one is reached through api->crypto.  FIPS 180-4.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/crypto.h>

#include <stddef.h>

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

static uint32_t rotr(uint32_t v, uint32_t n)
{
    return (v >> n) | (v << (32u - n));
}

static uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void block(ag_sha256_t *ctx, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = load_be32(p + 4 * i);
    }
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 =
            rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 =
            rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->h[0];
    uint32_t b = ctx->h[1];
    uint32_t c = ctx->h[2];
    uint32_t d = ctx->h[3];
    uint32_t e = ctx->h[4];
    uint32_t f = ctx->h[5];
    uint32_t g = ctx->h[6];
    uint32_t h = ctx->h[7];

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

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
    ctx->h[5] += f;
    ctx->h[6] += g;
    ctx->h[7] += h;
}

void ag_crypto_sha256_init(ag_sha256_t *ctx)
{
    ctx->h[0] = 0x6a09e667u;
    ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u;
    ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu;
    ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu;
    ctx->h[7] = 0x5be0cd19u;
    ctx->bytes = 0;
    ctx->have = 0;
}

void ag_crypto_sha256_update(ag_sha256_t *ctx, const void *data, size_t len)
{
    if (ctx == NULL || (data == NULL && len != 0)) {
        return;
    }
    const uint8_t *p = (const uint8_t *)data;
    ctx->bytes += (uint64_t)len;

    if (ctx->have != 0) {
        while (len != 0 && ctx->have < 64) {
            ctx->buf[ctx->have++] = *p++;
            len--;
        }
        if (ctx->have == 64) {
            block(ctx, ctx->buf);
            ctx->have = 0;
        }
    }

    while (len >= 64) {
        block(ctx, p);
        p += 64;
        len -= 64;
    }

    while (len > 0) {
        ctx->buf[ctx->have++] = *p++;
        len--;
    }
}

void ag_crypto_sha256_final(ag_sha256_t *ctx, uint8_t out[32])
{
    if (ctx == NULL || out == NULL) {
        return;
    }
    const uint64_t bits = ctx->bytes * 8u;

    /* 0x80, then zeroes, then the length: one block if it fits, two if not. */
    ctx->buf[ctx->have++] = 0x80;
    if (ctx->have > 56) {
        while (ctx->have < 64) {
            ctx->buf[ctx->have++] = 0;
        }
        block(ctx, ctx->buf);
        ctx->have = 0;
    }
    while (ctx->have < 56) {
        ctx->buf[ctx->have++] = 0;
    }
    store_be32(ctx->buf + 56, (uint32_t)(bits >> 32));
    store_be32(ctx->buf + 60, (uint32_t)bits);
    block(ctx, ctx->buf);

    for (int i = 0; i < 8; i++) {
        store_be32(out + 4 * i, ctx->h[i]);
    }
}
