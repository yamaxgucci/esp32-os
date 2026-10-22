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

uint32_t rdc_pack8_some(const uint8_t *ix, uint32_t n, uint8_t *out,
                        uint32_t cap, uint32_t *taken)
{
    uint32_t at = 0;
    uint32_t i = 0;
    while (i < n) {
        uint32_t run = 1;
        while (i + run < n && run < RDC_RUN_MAX && ix[i + run] == ix[i]) {
            run++;
        }
        if (run >= RDC_RUN_MIN) {
            if (at + 2u > cap) {
                break;
            }
            out[at] = (uint8_t)(run + 125u);
            out[at + 1u] = ix[i];
            at += 2u;
            i += run;
            continue;
        }
        uint32_t lit = 0;
        while (i + lit < n && lit < RDC_LIT_MAX) {
            const uint32_t j = i + lit;
            if (j + 2u < n && ix[j] == ix[j + 1u] && ix[j] == ix[j + 2u]) {
                break;
            }
            lit++;
        }
        if (at + 1u + lit > cap) {
            lit = (at + 2u <= cap) ? cap - at - 1u : 0u;
            if (lit == 0u) {
                break;
            }
        }
        out[at++] = (uint8_t)(lit - 1u);
        memcpy(out + at, ix + i, lit);
        at += lit;
        i += lit;
    }
    *taken = i;
    return at;
}

uint32_t rdc_unpack8(const uint8_t *in, uint32_t len, const uint16_t *pal,
                     uint16_t *px, uint32_t n)
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
            if (i + lit > n || at + lit > len) {
                return 0u;
            }
            for (uint32_t k = 0; k < lit; k++) {
                px[i++] = pal[in[at++]];
            }
        } else {
            const uint32_t run = c - 125u;
            if (i + run > n || at + 1u > len) {
                return 0u;
            }
            const uint16_t v = pal[in[at++]];
            for (uint32_t k = 0; k < run; k++) {
                px[i++] = v;
            }
        }
    }
    return at;
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

/* ---- copies (rdc_pack8z) ------------------------------------------------ */

#define RDC_Z_RUN_MAX  66u
#define RDC_Z_COPY_MAX 66u
/* candidates looked at a place: speed against size.  8 cost the S3 339 ms
 * of every second under Fallout's scroll (6 Oct 2026) */
#define RDC_Z_DEPTH    2u

void rdc_z_reset(rdc_zstate_t *z)
{
    memset(z->head, 0, sizeof(z->head));
    z->known = 0;
}

static uint32_t z_hash(const uint8_t *p)
{
    return (((uint32_t)p[0] << 6) ^ ((uint32_t)p[1] << 3) ^ p[2]) &
           (RDC_Z_HASH - 1u);
}

/* Every place up to `upto` (exclusive) with three indices after it known. */
static void z_insert(rdc_zstate_t *z, const uint8_t *ix, uint32_t upto,
                     uint32_t end)
{
    while (z->known < upto && z->known + 2u < end && z->known < RDC_Z_WIN) {
        const uint32_t h = z_hash(ix + z->known);
        z->prev[z->known] = z->head[h];
        z->head[h] = (uint16_t)(z->known + 1u);
        z->known++;
    }
}

/* Literals ix[from, from + lit) as one or more groups, as many as fit in
 * cap from `at`; how many were written. */
static uint32_t z_lits(const uint8_t *ix, uint32_t from, uint32_t lit,
                       uint8_t *out, uint32_t *at, uint32_t cap)
{
    uint32_t done = 0;
    while (done < lit && *at + 2u <= cap) {
        uint32_t k = lit - done;
        if (k > RDC_LIT_MAX) {
            k = RDC_LIT_MAX;
        }
        if (*at + 1u + k > cap) {
            k = cap - *at - 1u;
        }
        out[(*at)++] = (uint8_t)(k - 1u);
        memcpy(out + *at, ix + from + done, k);
        *at += k;
        done += k;
    }
    return done;
}

uint32_t rdc_pack8z_some(rdc_zstate_t *z, const uint8_t *ix, uint32_t from,
                         uint32_t n, uint8_t *out, uint32_t cap,
                         uint32_t *taken)
{
    const uint32_t end = from + n;
    uint32_t       at = 0;
    uint32_t       i = from;
    uint32_t       lit_at = from; /* literals [lit_at, i) not yet written */

    z_insert(z, ix, from, end);
    while (i < end) {
        /* a run here */
        uint32_t run = 1u;
        while (i + run < end && run < RDC_Z_RUN_MAX && ix[i + run] == ix[i]) {
            run++;
        }
        /* the longest copy here; four at least - three cost what three
         * literals do and break their group */
        uint32_t best = 0u, best_off = 0u;
        if (i + 3u < end) {
            z_insert(z, ix, i, end);
            uint32_t cand = z->head[z_hash(ix + i)];
            for (uint32_t d = 0; cand != 0u && d < RDC_Z_DEPTH; d++) {
                const uint32_t q = cand - 1u;
                if (q >= i || i - q >= RDC_Z_WIN) {
                    break;
                }
                uint32_t len = 0u;
                while (i + len < end && len < RDC_Z_COPY_MAX &&
                       ix[q + len] == ix[i + len]) {
                    len++;
                }
                if (len > best) {
                    best = len;
                    best_off = i - q;
                }
                cand = z->prev[q];
            }
        }
        uint8_t  tok[3];
        uint32_t tok_n = 0u, step = 0u;
        if (best >= 4u && best > run) {
            tok[0] = (uint8_t)(0xC0u | (best - 3u));
            tok[1] = (uint8_t)best_off;
            tok[2] = (uint8_t)(best_off >> 8);
            tok_n = 3u;
            step = best;
        } else if (run >= 3u) {
            tok[0] = (uint8_t)(0x80u | (run - 3u));
            tok[1] = ix[i];
            tok_n = 2u;
            step = run;
        }
        if (tok_n == 0u) {
            i++; /* a literal, written with its neighbours */
            continue;
        }
        /* the literals before it, then it - or stop where the room ends */
        const uint32_t lit = i - lit_at;
        const uint32_t put = z_lits(ix, lit_at, lit, out, &at, cap);
        lit_at += put;
        if (put < lit || at + tok_n > cap) {
            i = lit_at;
            *taken = i - from;
            return at;
        }
        memcpy(out + at, tok, tok_n);
        at += tok_n;
        i += step;
        lit_at = i;
    }
    lit_at += z_lits(ix, lit_at, i - lit_at, out, &at, cap);
    *taken = lit_at - from;
    return at;
}

uint32_t rdc_unpack8z(const uint8_t *in, uint32_t len, const uint16_t *pal,
                      uint16_t *px, uint32_t n)
{
    uint32_t at = 0;
    uint32_t i = 0;
    while (i < n) {
        if (at >= len) {
            return 0u;
        }
        const uint32_t c = in[at++];
        if (c < 0x80u) {
            const uint32_t lit = c + 1u;
            if (i + lit > n || at + lit > len) {
                return 0u;
            }
            for (uint32_t k = 0; k < lit; k++) {
                px[i++] = pal[in[at++]];
            }
        } else if (c < 0xC0u) {
            const uint32_t run = (c & 0x3Fu) + 3u;
            if (i + run > n || at + 1u > len) {
                return 0u;
            }
            const uint16_t v = pal[in[at++]];
            for (uint32_t k = 0; k < run; k++) {
                px[i++] = v;
            }
        } else {
            const uint32_t cnt = (c & 0x3Fu) + 3u;
            if (i + cnt > n || at + 2u > len) {
                return 0u;
            }
            const uint32_t off = (uint32_t)in[at] | ((uint32_t)in[at + 1u] << 8);
            at += 2u;
            if (off == 0u || off > i) {
                return 0u;
            }
            for (uint32_t k = 0; k < cnt; k++, i++) {
                px[i] = px[i - off];
            }
        }
    }
    return at;
}
