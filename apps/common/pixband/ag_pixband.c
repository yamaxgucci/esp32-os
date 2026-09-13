/*
 * ArgonOS - a band of pixels on a wire.  See ag_pixband.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "ag_pixband.h"

#include <string.h>

/*
 * A hash from colour to palette slot, so building a palette is one pass rather
 * than a search per pixel.  A linear scan would be up to 256 comparisons for
 * each of 2560 pixels, which is most of a millisecond per band - more than the
 * wire time it saves.  Open addressing, power-of-two table, no deletion: the
 * table is rebuilt for every band, and clearing it is what the generation
 * counter avoids.
 */
#define PAL_HASH 1024u

struct ag_pixband_ctx {
    uint32_t max_px;
    uint16_t gen;
    uint16_t hcol[PAL_HASH];
    uint16_t hgen[PAL_HASH];
    uint8_t  hidx[PAL_HASH];
    uint16_t pal[256];
    /*
     * Two tails, laid out after the struct by ag_pixband_init:
     *   idx[max_px]        one palette index per pixel, before packing
     *   alt[alt_cap]       the runner-up encoding, while it is being measured
     */
    uint32_t alt_cap;
    uint8_t  tail[];
};

/*
 * Worst case for PackBits is one control byte per 128 units plus the units
 * themselves.  A codec whose output does not fit the buffer it was handed is a
 * codec that corrupts the picture on exactly the frames that compress badly, so
 * the runner-up buffer is sized for that and not for the average.
 */
static uint32_t alt_cap_for(uint32_t max_px)
{
    return max_px * 2u + max_px / 64u + 512u;
}

size_t ag_pixband_size(uint32_t max_px)
{
    return sizeof(struct ag_pixband_ctx) + (size_t)max_px +
           (size_t)alt_cap_for(max_px);
}

bool ag_pixband_init(void *mem, size_t bytes, uint32_t max_px)
{
    if (mem == NULL || max_px == 0u || bytes < ag_pixband_size(max_px)) {
        return false;
    }
    struct ag_pixband_ctx *c = (struct ag_pixband_ctx *)mem;
    memset(c, 0, sizeof(*c));
    c->max_px = max_px;
    c->alt_cap = alt_cap_for(max_px);
    c->gen = 0;
    return true;
}

static uint8_t *ctx_idx(ag_pixband_ctx_t *c) { return c->tail; }
static uint8_t *ctx_alt(ag_pixband_ctx_t *c) { return c->tail + c->max_px; }

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* ---- PackBits ---------------------------------------------------------- */

/*
 * A control byte, then either that many literal units or that many copies of
 * one:
 *
 *   0x00..0x7F   (n + 1) literals               1..128
 *   0x80..0xFF   (n - 0x80 + 2) copies of one   2..129
 *
 * One pass, no memory.  Returns 0 when it would not fit in `cap`, which the
 * caller reads as "this encoding is not the one".
 */
static uint32_t pack16(const uint16_t *px, uint32_t n, uint8_t *out,
                       uint32_t cap)
{
    uint32_t at = 0;
    uint32_t i = 0;

    while (i < n) {
        uint32_t run = 1;
        while (run < 129u && i + run < n && px[i + run] == px[i]) {
            run++;
        }
        if (run >= 2u) {
            if (at + 3u > cap) {
                return 0;
            }
            out[at++] = (uint8_t)(0x80u + (run - 2u));
            put16(out + at, px[i]);
            at += 2u;
            i += run;
            continue;
        }
        uint32_t lit = 1;
        while (lit < 128u && i + lit + 1u < n &&
               px[i + lit] != px[i + lit + 1u]) {
            lit++;
        }
        if (i + lit > n) {
            lit = n - i;
        }
        if (at + 1u + lit * 2u > cap) {
            return 0;
        }
        out[at++] = (uint8_t)(lit - 1u);
        for (uint32_t k = 0; k < lit; k++) {
            put16(out + at, px[i + k]);
            at += 2u;
        }
        i += lit;
    }
    return at;
}

static bool unpack16(const uint8_t *in, uint32_t len, uint16_t *px, uint32_t n)
{
    uint32_t at = 0;
    uint32_t out = 0;

    while (at < len) {
        const uint8_t ctl = in[at++];
        if (ctl & 0x80u) {
            const uint32_t run = (uint32_t)(ctl - 0x80u) + 2u;
            if (at + 2u > len || out + run > n) {
                return false;
            }
            const uint16_t v = get16(in + at);
            at += 2u;
            for (uint32_t k = 0; k < run; k++) {
                px[out++] = v;
            }
        } else {
            const uint32_t lit = (uint32_t)ctl + 1u;
            if (at + lit * 2u > len || out + lit > n) {
                return false;
            }
            for (uint32_t k = 0; k < lit; k++) {
                px[out++] = get16(in + at);
                at += 2u;
            }
        }
    }
    return out == n;
}

/* The same shape over bytes, for the index stream. */
static uint32_t pack8(const uint8_t *b, uint32_t n, uint8_t *out, uint32_t cap)
{
    uint32_t at = 0;
    uint32_t i = 0;

    while (i < n) {
        uint32_t run = 1;
        while (run < 129u && i + run < n && b[i + run] == b[i]) {
            run++;
        }
        if (run >= 3u) { /* below three a literal is no worse and often better */
            if (at + 2u > cap) {
                return 0;
            }
            out[at++] = (uint8_t)(0x80u + (run - 2u));
            out[at++] = b[i];
            i += run;
            continue;
        }
        uint32_t lit = 1;
        while (lit < 128u && i + lit + 2u < n &&
               !(b[i + lit] == b[i + lit + 1u] &&
                 b[i + lit] == b[i + lit + 2u])) {
            lit++;
        }
        if (i + lit > n) {
            lit = n - i;
        }
        if (at + 1u + lit > cap) {
            return 0;
        }
        out[at++] = (uint8_t)(lit - 1u);
        for (uint32_t k = 0; k < lit; k++) {
            out[at++] = b[i + k];
        }
        i += lit;
    }
    return at;
}

static bool unpack8(const uint8_t *in, uint32_t len, uint8_t *b, uint32_t n)
{
    uint32_t at = 0;
    uint32_t out = 0;

    while (at < len) {
        const uint8_t ctl = in[at++];
        if (ctl & 0x80u) {
            const uint32_t run = (uint32_t)(ctl - 0x80u) + 2u;
            if (at >= len || out + run > n) {
                return false;
            }
            const uint8_t v = in[at++];
            for (uint32_t k = 0; k < run; k++) {
                b[out++] = v;
            }
        } else {
            const uint32_t lit = (uint32_t)ctl + 1u;
            if (at + lit > len || out + lit > n) {
                return false;
            }
            for (uint32_t k = 0; k < lit; k++) {
                b[out++] = in[at++];
            }
        }
    }
    return out == n;
}

/* ---- palette and indices ----------------------------------------------- */

/*
 * Layout: bpp, count-1, the palette, then pack8 over the packed indices.
 * Returns 0 when the band has more than 256 colours (a photograph, a gradient)
 * and this is the wrong tool, or when the result would not fit.
 */
static uint32_t encode_indexed(ag_pixband_ctx_t *c, const uint16_t *px,
                               uint32_t n, uint8_t *out, uint32_t cap)
{
    uint8_t *idx = ctx_idx(c);

    if (++c->gen == 0) {
        /* Wrapped: every stale mark now reads as current, so start over. */
        memset(c->hgen, 0, sizeof(c->hgen));
        c->gen = 1;
    }

    /*
     * One-entry cache in front of the hash, and it is not a micro-optimisation.
     * The whole reason this encoding wins on this content is that the same
     * colour repeats - and a repeat used to cost a multiply, a mask and a probe
     * each time.  On a measured full frame the encoder was thirteen
     * milliseconds against thirty-seven of wire, so it was the second largest
     * cost in the picture.
     */
    uint16_t last_c = px[0];
    uint8_t  last_i = 0;
    bool     have_last = false;

    uint32_t ncol = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint16_t cc = px[i];
        if (have_last && cc == last_c) {
            idx[i] = last_i;
            continue;
        }
        uint32_t slot = ((uint32_t)cc * 2654435761u) >> 20 & (PAL_HASH - 1u);
        for (;;) {
            if (c->hgen[slot] != c->gen) {
                if (ncol == 256u) {
                    return 0; /* too many colours for a byte of index */
                }
                c->hgen[slot] = c->gen;
                c->hcol[slot] = cc;
                c->hidx[slot] = (uint8_t)ncol;
                c->pal[ncol] = cc;
                idx[i] = (uint8_t)ncol;
                last_c = cc;
                last_i = (uint8_t)ncol;
                have_last = true;
                ncol++;
                break;
            }
            if (c->hcol[slot] == cc) {
                idx[i] = c->hidx[slot];
                last_c = cc;
                last_i = c->hidx[slot];
                have_last = true;
                break;
            }
            slot = (slot + 1u) & (PAL_HASH - 1u);
        }
    }

    const uint32_t bpp = (ncol <= 16u) ? 4u : 8u;
    uint32_t       at = 0;

    if (2u + ncol * 2u > cap) {
        return 0;
    }
    out[at++] = (uint8_t)bpp;
    out[at++] = (uint8_t)(ncol - 1u);
    for (uint32_t i = 0; i < ncol; i++) {
        put16(out + at, c->pal[i]);
        at += 2u;
    }

    /* Pack the indices down to bpp, in place. */
    uint32_t bytes = n;
    if (bpp == 4u) {
        bytes = (n + 1u) / 2u;
        for (uint32_t i = 0; i < bytes; i++) {
            const uint8_t lo = idx[i * 2u];
            const uint8_t hi = (i * 2u + 1u < n) ? idx[i * 2u + 1u] : 0u;
            idx[i] = (uint8_t)((hi << 4) | (lo & 0x0fu));
        }
    }

    const uint32_t packed = pack8(idx, bytes, out + at, cap - at);
    if (packed == 0) {
        return 0;
    }
    return at + packed;
}

static bool decode_indexed(const uint8_t *in, uint32_t len, uint16_t *px,
                           uint32_t n)
{
    if (len < 2u) {
        return false;
    }
    const uint32_t bpp = in[0];
    const uint32_t ncol = (uint32_t)in[1] + 1u;

    if ((bpp != 4u && bpp != 8u) || (bpp == 4u && ncol > 16u)) {
        return false;
    }
    uint32_t at = 2u;
    if (at + ncol * 2u > len) {
        return false;
    }
    uint16_t pal[256];
    for (uint32_t i = 0; i < ncol; i++) {
        pal[i] = get16(in + at);
        at += 2u;
    }

    const uint32_t bytes = (bpp == 4u) ? ((n + 1u) / 2u) : n;

    /*
     * The indices are unpacked straight into the caller's pixel buffer, at its
     * far end, and then read back out of it while the colours are written over
     * them.  A band of 640x8 is ten kilobytes; a scratch buffer for it would
     * have to be either a second allocation on the decode side or a fixed array
     * as large as the largest band anybody ever sends, and this needs neither.
     *
     * Safe because a pixel is two bytes and an index at most one: the write
     * head (i) never catches the read head, which sits at n*2 - bytes + i/2 or
     * further.
     */
    if (bytes > n * 2u) {
        return false;
    }
    uint8_t *raw = (uint8_t *)px + (n * 2u - bytes);
    if (!unpack8(in + at, len - at, raw, bytes)) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v;
        if (bpp == 4u) {
            const uint8_t byte = raw[i / 2u];
            v = (i & 1u) ? (uint32_t)(byte >> 4) : (uint32_t)(byte & 0x0fu);
        } else {
            v = raw[i];
        }
        if (v >= ncol) {
            return false;
        }
        px[i] = pal[v];
    }
    return true;
}

/* ---- the three, measured ----------------------------------------------- */

uint32_t ag_pixband_encode(ag_pixband_ctx_t *c, const uint16_t *px, uint32_t n,
                           uint8_t *out, uint32_t cap, uint8_t *op)
{
    if (c == NULL || px == NULL || out == NULL || op == NULL || n == 0u ||
        n > c->max_px) {
        return 0;
    }

    const uint32_t raw = n * 2u;
    uint32_t       best = (raw <= cap) ? raw : 0xffffffffu;
    uint8_t        best_op = AG_PIXBAND_RAW;
    uint32_t       best_len = raw;

    uint8_t       *alt = ctx_alt(c);
    const uint32_t alt_cap = c->alt_cap;

    const uint32_t p = pack16(px, n, alt, alt_cap);
    if (p != 0 && p < best && p <= cap) {
        memcpy(out, alt, p);
        best = p;
        best_op = AG_PIXBAND_PACK;
        best_len = p;
    }

    const uint32_t q = encode_indexed(c, px, n, alt, alt_cap);
    if (q != 0 && q < best && q <= cap) {
        memcpy(out, alt, q);
        best = q;
        best_op = AG_PIXBAND_IDX;
        best_len = q;
    }

    if (best == 0xffffffffu) {
        return 0; /* nothing fits in what the caller offered */
    }
    if (best_op == AG_PIXBAND_RAW) {
        for (uint32_t i = 0; i < n; i++) {
            put16(out + i * 2u, px[i]);
        }
    }
    *op = best_op;
    return best_len;
}

bool ag_pixband_decode(uint8_t op, const uint8_t *in, uint32_t len,
                       uint16_t *px, uint32_t n)
{
    if (in == NULL || px == NULL || n == 0u) {
        return false;
    }
    switch (op) {
    case AG_PIXBAND_RAW:
        if (len != n * 2u) {
            return false;
        }
        for (uint32_t i = 0; i < n; i++) {
            px[i] = get16(in + i * 2u);
        }
        return true;
    case AG_PIXBAND_PACK:
        return unpack16(in, len, px, n);
    case AG_PIXBAND_IDX:
        return decode_indexed(in, len, px, n);
    default:
        return false;
    }
}
