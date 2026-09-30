/*
 * ArgonOS - how a run of pixels is packed for a neighbour's screen.  See
 * rdcodec.h for the format.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "net/rdcodec.h"

#include <string.h>

#define RDC_LIT_MAX 128u /* c = 0..127: c + 1 literals */
#define RDC_RUN_MIN 3u   /* c = 128..255: a run of c - 125 */
#define RDC_RUN_MAX 130u

static void put_px(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_px(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t pack(const uint16_t *px, uint32_t n, uint8_t *out,
                     uint32_t cap, uint32_t *taken)
{
    uint32_t at = 0;
    uint32_t i = 0;
    while (i < n) {
        /* how far the pixel at i repeats */
        uint32_t run = 1;
        while (i + run < n && run < RDC_RUN_MAX && px[i + run] == px[i]) {
            run++;
        }
        if (run >= RDC_RUN_MIN) {
            if (at + 3u > cap) {
                break;
            }
            out[at] = (uint8_t)(run + 125u);
            put_px(out + at + 1u, px[i]);
            at += 3u;
            i += run;
            continue;
        }
        /* literals up to the next run of three, or 128 of them */
        uint32_t lit = 0;
        while (i + lit < n && lit < RDC_LIT_MAX) {
            const uint32_t j = i + lit;
            if (j + 2u < n && px[j] == px[j + 1u] && px[j] == px[j + 2u]) {
                break;
            }
            lit++;
        }
        if (at + 1u + 2u * lit > cap) {
            /* as many of them as there is room for, and no more */
            lit = (at + 3u <= cap) ? (cap - at - 1u) / 2u : 0u;
            if (lit == 0u) {
                break;
            }
        }
        out[at++] = (uint8_t)(lit - 1u);
        for (uint32_t k = 0; k < lit; k++) {
            put_px(out + at, px[i + k]);
            at += 2u;
        }
        i += lit;
    }
    *taken = i;
    return at;
}

uint32_t rdc_pack(const uint16_t *px, uint32_t n, uint8_t *out, uint32_t cap)
{
    uint32_t       taken = 0;
    const uint32_t at = pack(px, n, out, cap, &taken);
    return (taken == n) ? at : 0u;
}

uint32_t rdc_pack_some(const uint16_t *px, uint32_t n, uint8_t *out,
                       uint32_t cap, uint32_t *taken)
{
    return pack(px, n, out, cap, taken);
}

uint32_t rdc_unpack(const uint8_t *in, uint32_t len, uint16_t *px, uint32_t n)
{
    uint32_t at = 0;
    uint32_t i = 0;
    while (i < n) {
        if (at >= len) {
            return 0u;
        }
        const uint32_t c = in[at++];
        if (c < 128u) {
            const uint32_t lit = c + 1u;
            if (i + lit > n || at + 2u * lit > len) {
                return 0u;
            }
            for (uint32_t k = 0; k < lit; k++) {
                px[i++] = get_px(in + at);
                at += 2u;
            }
        } else {
            const uint32_t run = c - 125u;
            if (i + run > n || at + 2u > len) {
                return 0u;
            }
            const uint16_t v = get_px(in + at);
            at += 2u;
            for (uint32_t k = 0; k < run; k++) {
                px[i++] = v;
            }
        }
    }
    return at;
}
