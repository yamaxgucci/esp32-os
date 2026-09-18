/*
 * ArgonOS - a picture, encoded by the real encoder, for the page to decode.
 *
 *   build-host/pixband_sample [out.bands [width height]]
 *
 * Writes the bands of a test picture exactly as PHONE.SYS would put them on
 * the wire: the op byte the encoder chose, then x, y, w, h, then its payload.
 * tools/phonepage.py replays the file to a browser, so the page's three
 * decoders can be looked at without a board, without Wi-Fi and without a
 * phone.
 *
 * Why it exists: the page was checked with raw pixels and the board sends
 * palette-and-indices - measured, 522 bands of 522 - so the decoder a phone
 * actually uses was the one nobody had tried.  The obvious fix, writing the
 * encoder a second time in Python, is the mistake ag_pixband.h is a file to
 * avoid: "the one failure neither board can catch is a pair that agrees with
 * itself and is wrong."  So the bytes come from the same encoder the board
 * runs, and the page is the only thing under test.
 *
 * The picture is GFXPIX's: a circle, two diagonals and a border.  A circle is
 * the point - a text mode cannot draw one, so "graphics" or "characters" is
 * answerable at a glance.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag_pixband.h"

#define BAND_ROWS 8

static uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "build/sample.bands";
    const int   w = (argc > 3) ? atoi(argv[2]) : 160;
    const int   h = (argc > 3) ? atoi(argv[3]) : 144;

    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
        fprintf(stderr, "pixband_sample: silly size %dx%d\n", w, h);
        return 2;
    }

    uint16_t *px = (uint16_t *)malloc((size_t)w * h * sizeof(uint16_t));
    if (px == NULL) {
        return 2;
    }

    const int cx = w / 2, cy = h / 2;
    const int r = ((w < h) ? w : h) / 2 - 2;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const int dx = x - cx, dy = y - cy;
            uint16_t  c;
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) {
                c = rgb565(255, 255, 255);
            } else if (x * h == y * w || (w - 1 - x) * h == y * w) {
                c = rgb565(255, 255, 255);
            } else if (dx * dx + dy * dy <= r * r) {
                c = rgb565(224, 192, 64);
            } else {
                c = rgb565(0, 0, 128);
            }
            px[(size_t)y * w + x] = c;
        }
    }

    const uint32_t max_px = (uint32_t)w * BAND_ROWS;
    void          *mem = malloc(ag_pixband_size(max_px));
    if (mem == NULL || !ag_pixband_init(mem, ag_pixband_size(max_px), max_px)) {
        fprintf(stderr, "pixband_sample: no encoder\n");
        return 2;
    }
    ag_pixband_ctx_t *ctx = (ag_pixband_ctx_t *)mem;

    /* Worst case for the payload is raw, plus PackBits' control bytes. */
    const uint32_t cap = max_px * 2u + max_px / 64u + 64u;
    unsigned char *out = (unsigned char *)malloc(cap);
    if (out == NULL) {
        return 2;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "pixband_sample: cannot write %s\n", path);
        return 2;
    }

    unsigned raw = 0, pack = 0, idx = 0, bytes = 0, bands = 0;
    for (int y = 0; y < h; y += BAND_ROWS) {
        const int rows = (h - y < BAND_ROWS) ? (h - y) : BAND_ROWS;
        uint8_t   op = 0;
        const uint32_t len =
            ag_pixband_encode(ctx, px + (size_t)y * w, (uint32_t)(w * rows),
                              out, cap, &op);
        if (len == 0u) {
            fprintf(stderr, "pixband_sample: the encoder refused row %d\n", y);
            return 2;
        }
        /*
         * One record per band, so the replayer needs to know nothing about the
         * encoding: a length, then exactly the bytes PHONE.SYS puts in a
         * WebSocket frame - op, x, y, w, h, payload.
         */
        unsigned char head[4 + 1 + 8];
        const unsigned total = 1u + 8u + len;
        put16(head + 0, total & 0xffffu);
        put16(head + 2, total >> 16);
        head[4] = op;
        put16(head + 5, 0);
        put16(head + 7, (unsigned)y);
        put16(head + 9, (unsigned)w);
        put16(head + 11, (unsigned)rows);
        fwrite(head, 1, sizeof(head), f);
        fwrite(out, 1, len, f);

        bands++;
        bytes += len;
        if (op == AG_PIXBAND_RAW) {
            raw++;
        } else if (op == AG_PIXBAND_PACK) {
            pack++;
        } else {
            idx++;
        }
    }
    fclose(f);

    printf("%s: %ux%u, %u bands, %u bytes (raw %u, packed %u, indexed %u), "
           "%.1fx\n",
           path, (unsigned)w, (unsigned)h, bands, bytes, raw, pack, idx,
           (double)(w * h * 2) / (double)(bytes ? bytes : 1));
    return 0;
}
