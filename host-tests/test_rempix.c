/*
 * ArgonOS - the remote screen's codec, encoded and decoded on the host.
 *
 * This exists because of what the boards cannot check.  The receiver counts
 * frames whose checksum failed, so a corrupted wire is caught; a person looking
 * at the glass catches a picture that is visibly wrong.  Neither catches the
 * failure that matters most: an encoder and a decoder that agree with each
 * other and are both wrong.  A palette written one way and read another
 * produces a picture that looks plausible and is not the frame that was sent,
 * and nothing on either board would ever say so.
 *
 * So the two halves are compiled here, against each other, and asked to
 * reproduce a frame exactly.  Bit for bit, or it is a failure - this codec is
 * lossless by construction and a single wrong pixel means a bug and not a
 * rounding.
 *
 * The implementations are copied rather than shared, and that is a real cost
 * that has to be paid attention to: they live in apps/remdisp/remdisp.c and
 * apps/remterm/remterm.c, which are a .SYS and an .AXE for two different
 * instruction sets and cannot be linked into a host binary as they stand.  What
 * this test therefore proves is that the *algorithm pair* is sound.  A change to
 * either file that is not mirrored here will pass this test and break the
 * screen, so the header of each says to come here.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BAND_ROWS 8u
#define MAX_PX    (BAND_ROWS * 320u)
#define CAP       (MAX_PX * 2u + MAX_PX / 64u + 16u)

/* ---- the encoder, mirroring apps/remdisp/remdisp.c ---------------------- */

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

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

static uint32_t pack8(const uint8_t *b, uint32_t n, uint8_t *out, uint32_t cap)
{
    uint32_t at = 0;
    uint32_t i = 0;

    while (i < n) {
        uint32_t run = 1;
        while (run < 129u && i + run < n && b[i + run] == b[i]) {
            run++;
        }
        if (run >= 3u) {
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

#define PAL_HASH 1024u

static uint8_t  s_idx[MAX_PX];
static uint16_t s_pal[256];
static uint16_t s_hcol[PAL_HASH];
static uint16_t s_hgen[PAL_HASH];
static uint8_t  s_hidx[PAL_HASH];
static uint16_t s_gen;

static uint32_t encode_indexed(const uint16_t *px, uint32_t n, uint8_t *out,
                               uint32_t cap)
{
    if (++s_gen == 0) {
        memset(s_hgen, 0, sizeof(s_hgen));
        s_gen = 1;
    }

    uint16_t last_c = px[0];
    uint8_t  last_i = 0;
    bool     have_last = false;

    uint32_t ncol = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint16_t c = px[i];
        if (have_last && c == last_c) {
            s_idx[i] = last_i;
            continue;
        }
        uint32_t slot = ((uint32_t)c * 2654435761u) >> 20 & (PAL_HASH - 1u);
        for (;;) {
            if (s_hgen[slot] != s_gen) {
                if (ncol == 256u) {
                    return 0;
                }
                s_hgen[slot] = s_gen;
                s_hcol[slot] = c;
                s_hidx[slot] = (uint8_t)ncol;
                s_pal[ncol] = c;
                s_idx[i] = (uint8_t)ncol;
                last_c = c;
                last_i = (uint8_t)ncol;
                have_last = true;
                ncol++;
                break;
            }
            if (s_hcol[slot] == c) {
                s_idx[i] = s_hidx[slot];
                last_c = c;
                last_i = s_hidx[slot];
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
        put16(out + at, s_pal[i]);
        at += 2u;
    }

    uint32_t bytes = n;
    if (bpp == 4u) {
        bytes = (n + 1u) / 2u;
        for (uint32_t i = 0; i < bytes; i++) {
            const uint8_t lo = s_idx[i * 2u];
            const uint8_t hi = (i * 2u + 1u < n) ? s_idx[i * 2u + 1u] : 0u;
            s_idx[i] = (uint8_t)((hi << 4) | (lo & 0x0fu));
        }
    }

    const uint32_t packed = pack8(s_idx, bytes, out + at, cap - at);
    if (packed == 0) {
        return 0;
    }
    return at + packed;
}

/* ---- the decoder, mirroring apps/remterm/remterm.c ---------------------- */

static uint16_t d_px[MAX_PX];

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static bool unpack16(const uint8_t *in, uint32_t len, uint32_t npx)
{
    uint32_t at = 0;
    uint32_t out = 0;

    while (out < npx) {
        if (at >= len) {
            return false;
        }
        const uint8_t ctrl = in[at++];
        if (ctrl & 0x80u) {
            const uint32_t run = (uint32_t)(ctrl - 0x80u) + 2u;
            if (at + 2u > len || out + run > npx) {
                return false;
            }
            const uint16_t c = (uint16_t)rd16(in + at);
            at += 2u;
            for (uint32_t i = 0; i < run; i++) {
                d_px[out++] = c;
            }
        } else {
            const uint32_t lit = (uint32_t)ctrl + 1u;
            if (at + lit * 2u > len || out + lit > npx) {
                return false;
            }
            for (uint32_t i = 0; i < lit; i++) {
                d_px[out++] = (uint16_t)rd16(in + at);
                at += 2u;
            }
        }
    }
    return out == npx;
}

static bool unpack8(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t n)
{
    uint32_t at = 0;
    uint32_t o = 0;

    while (o < n) {
        if (at >= len) {
            return false;
        }
        const uint8_t ctrl = in[at++];
        if (ctrl & 0x80u) {
            const uint32_t run = (uint32_t)(ctrl - 0x80u) + 2u;
            if (at >= len || o + run > n) {
                return false;
            }
            const uint8_t b = in[at++];
            for (uint32_t i = 0; i < run; i++) {
                out[o++] = b;
            }
        } else {
            const uint32_t lit = (uint32_t)ctrl + 1u;
            if (at + lit > len || o + lit > n) {
                return false;
            }
            for (uint32_t i = 0; i < lit; i++) {
                out[o++] = in[at++];
            }
        }
    }
    return o == n;
}

static bool unpack_indexed(const uint8_t *in, uint32_t len, uint32_t npx)
{
    static uint8_t  idx[MAX_PX];
    static uint16_t pal[256];

    if (len < 2u) {
        return false;
    }
    const uint32_t bpp = in[0];
    const uint32_t ncol = (uint32_t)in[1] + 1u;
    if ((bpp != 4u && bpp != 8u) || 2u + ncol * 2u > len) {
        return false;
    }
    for (uint32_t i = 0; i < ncol; i++) {
        pal[i] = (uint16_t)rd16(in + 2u + i * 2u);
    }

    const uint32_t at = 2u + ncol * 2u;
    const uint32_t bytes = (bpp == 4u) ? (npx + 1u) / 2u : npx;
    if (bytes > sizeof(idx)) {
        return false;
    }
    if (!unpack8(in + at, len - at, idx, bytes)) {
        return false;
    }

    if (bpp == 4u) {
        for (uint32_t i = npx; i-- > 0;) {
            const uint8_t byte = idx[i / 2u];
            const uint8_t v =
                (i & 1u) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0fu);
            if (v >= ncol) {
                return false;
            }
            d_px[i] = pal[v];
        }
    } else {
        for (uint32_t i = 0; i < npx; i++) {
            if (idx[i] >= ncol) {
                return false;
            }
            d_px[i] = pal[idx[i]];
        }
    }
    return true;
}

/* ---- the round trip ---------------------------------------------------- */

/*
 * Encode with both encodings, decode with the matching decoder, and demand the
 * frame back exactly.  Returns the smaller encoded size, or 0 when neither
 * encoding would be used - which is itself a legitimate answer for a band of
 * noise, and is why the caller checks it rather than asserting on it.
 */
static uint32_t round_trip(const uint16_t *src, uint32_t n, const char *what)
{
    static uint8_t buf[CAP];
    uint32_t       smallest = 0;

    const uint32_t p = pack16(src, n, buf, sizeof(buf));
    if (p != 0) {
        memset(d_px, 0xAA, sizeof(d_px));
        const bool ok = unpack16(buf, p, n);
        if (!ok) {
            printf("FAIL %s: pack16 stream rejected by its own decoder\n", what);
            ag_test_failures++;
        }
        ag_test_checks++;
        if (ok && memcmp(d_px, src, (size_t)n * 2u) != 0) {
            printf("FAIL %s: pack16 round trip differs\n", what);
            ag_test_failures++;
        }
        ag_test_checks++;
        smallest = p;
    }

    const uint32_t q = encode_indexed(src, n, buf, sizeof(buf));
    if (q != 0) {
        memset(d_px, 0x55, sizeof(d_px));
        const bool ok = unpack_indexed(buf, q, n);
        if (!ok) {
            printf("FAIL %s: indexed stream rejected by its own decoder\n",
                   what);
            ag_test_failures++;
        }
        ag_test_checks++;
        if (ok && memcmp(d_px, src, (size_t)n * 2u) != 0) {
            printf("FAIL %s: indexed round trip differs\n", what);
            ag_test_failures++;
        }
        ag_test_checks++;
        if (smallest == 0 || q < smallest) {
            smallest = q;
        }
    }
    return smallest;
}

static uint32_t s_rand = 12345u;

static uint32_t next_rand(void)
{
    s_rand = s_rand * 1103515245u + 12345u;
    return s_rand >> 16;
}

void run_rempix_tests(void)
{
    static uint16_t band[MAX_PX];
    const uint32_t  n = MAX_PX;

    /* One colour: the best case, and the one a black border produces. */
    for (uint32_t i = 0; i < n; i++) {
        band[i] = 0x0000u;
    }
    uint32_t got = round_trip(band, n, "flat black");
    AG_CHECK(got != 0 && got < n / 8u); /* enormously smaller than 2 bytes/px */

    /* Two colours in wide stripes: what a panel of UI looks like. */
    for (uint32_t i = 0; i < n; i++) {
        band[i] = ((i / 40u) & 1u) ? 0xF800u : 0x001Fu;
    }
    got = round_trip(band, n, "wide stripes");
    AG_CHECK(got != 0 && got < n);

    /* Sixteen colours, so the indexed encoding takes four bits a pixel. */
    for (uint32_t i = 0; i < n; i++) {
        band[i] = (uint16_t)((i % 16u) * 0x1111u);
    }
    got = round_trip(band, n, "sixteen colours");
    AG_CHECK(got != 0 && got < n); /* under one byte a pixel */

    /* Every other pixel different: the worst case for a run encoder. */
    for (uint32_t i = 0; i < n; i++) {
        band[i] = (uint16_t)(i & 1u ? 0u : 0xFFFFu);
    }
    (void)round_trip(band, n, "alternating");

    /* Noise, which no encoding here should claim to shrink - and must still
     * come back exactly from whichever one is tried. */
    for (uint32_t i = 0; i < n; i++) {
        band[i] = (uint16_t)next_rand();
    }
    (void)round_trip(band, n, "noise");

    /*
     * A gradient: more than 256 colours, so the indexed encoding must refuse
     * rather than produce something.  Refusing is the tested behaviour - an
     * encoder that quietly wrapped its palette would corrupt the picture.
     */
    for (uint32_t i = 0; i < n; i++) {
        band[i] = (uint16_t)(i * 7u);
    }
    static uint8_t buf[CAP];
    AG_CHECK_INT((int)encode_indexed(band, n, buf, sizeof(buf)), 0);
    (void)round_trip(band, n, "gradient");

    /* Short bands, including the odd length that the 4-bit packing pads. */
    static const uint32_t k_lens[] = {1u, 2u, 3u, 15u, 16u, 17u, 129u, 130u};
    for (uint32_t li = 0; li < sizeof(k_lens) / sizeof(k_lens[0]); li++) {
        const uint32_t len = k_lens[li];
        for (uint32_t i = 0; i < len; i++) {
            band[i] = (uint16_t)((i % 3u) * 0x2222u);
        }
        (void)round_trip(band, len, "short band");
    }

    /*
     * A run longer than one control byte can describe, on both sides of the
     * boundary: 129 is the largest a repeat carries and 130 has to split.
     */
    for (uint32_t rl = 128u; rl <= 131u; rl++) {
        for (uint32_t i = 0; i < n; i++) {
            band[i] = (i < rl) ? 0x1234u : (uint16_t)(0x4000u + i);
        }
        (void)round_trip(band, n, "run at the control-byte boundary");
    }
}
