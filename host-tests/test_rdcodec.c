/*
 * Host tests for how a run of pixels is packed for a neighbour's screen
 * (components/argon_kernel/src/net/rdcodec.c).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "test.h"

#include "net/rdcodec.h"

static uint32_t s_seed = 99u;

static uint16_t rnd16(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (uint16_t)(s_seed >> 16);
}

/* Packed, unpacked, and the same pixels; returns the packed size. */
static uint32_t round_trip(const uint16_t *px, uint32_t n)
{
    static uint8_t  buf[RDC_PACK_MAX(640)];
    static uint16_t back[640];
    const uint32_t  len = rdc_pack(px, n, buf, sizeof(buf));
    AG_CHECK(len > 0u && len <= RDC_PACK_MAX(n));
    memset(back, 0xEE, sizeof(back));
    AG_CHECK_INT((int)rdc_unpack(buf, len, back, n), (int)len);
    AG_CHECK(memcmp(back, px, n * 2u) == 0);
    return len;
}

static void flat_packs_to_almost_nothing(void)
{
    uint16_t px[320];
    for (int i = 0; i < 320; i++) {
        px[i] = 0x1234;
    }
    /* 320 = 130 + 130 + 60: three runs of three bytes */
    AG_CHECK_INT((int)round_trip(px, 320), 9);
}

static void noise_costs_one_byte_in_257(void)
{
    uint16_t px[640];
    for (int i = 0; i < 640; i++) {
        px[i] = (uint16_t)(rnd16() | 1u); /* no runs by construction below */
        if (i > 0 && px[i] == px[i - 1]) {
            px[i] ^= 2u;
        }
    }
    AG_CHECK_INT((int)round_trip(px, 640), (int)(2u * 640u + 5u));
}

static void mixed_rows_come_back(void)
{
    uint16_t px[640];
    for (int trial = 0; trial < 500; trial++) {
        const uint32_t n = 1u + rnd16() % 640u;
        uint16_t       v = rnd16();
        for (uint32_t i = 0; i < n; i++) {
            if (rnd16() % 4u == 0u) {
                v = rnd16() % 8u; /* few values: runs of every length */
            }
            px[i] = v;
        }
        (void)round_trip(px, n);
    }
    /* the edges of the run lengths: 1, 2, 3, 130, 131 of one value */
    const uint32_t lens[] = {1, 2, 3, 4, 129, 130, 131, 132, 260, 261};
    for (unsigned k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
        for (uint32_t i = 0; i < lens[k]; i++) {
            px[i] = 7;
        }
        (void)round_trip(px, lens[k]);
    }
}

static void too_little_room_is_refused(void)
{
    uint16_t px[64];
    uint8_t  buf[200];
    for (int i = 0; i < 64; i++) {
        px[i] = (uint16_t)(i * 977);
    }
    AG_CHECK_INT((int)rdc_pack(px, 64, buf, 128), 0); /* needs 129 */
    AG_CHECK_INT((int)rdc_pack(px, 64, buf, 129), 129);
}

static void a_damaged_packing_is_not_pixels(void)
{
    uint16_t px[100], back[100];
    uint8_t  buf[RDC_PACK_MAX(100)];
    for (int i = 0; i < 100; i++) {
        px[i] = (uint16_t)((i / 10) * 3);
    }
    const uint32_t len = rdc_pack(px, 100, buf, sizeof(buf));
    AG_CHECK(len > 0u);
    /* short of its bytes, or asked for more or fewer pixels than it holds */
    AG_CHECK_INT((int)rdc_unpack(buf, len - 1u, back, 100), 0);
    AG_CHECK_INT((int)rdc_unpack(buf, len, back, 101), 0);
    AG_CHECK_INT((int)rdc_unpack(buf, len, back, 99), 0);
}

/*
 * COPIES (rdc_pack8z): a rectangle of indices packed row by row the way the
 * painter does - each row after the ones before it, in as many pieces as a
 * small `cap` forces - and unpacked whole; the same pixels, and the size.
 */
static uint32_t z_round_trip(const uint8_t *ix, uint32_t w, uint32_t h,
                             uint32_t cap)
{
    static rdc_zstate_t z;
    static uint8_t      buf[RDC_Z_WIN * 2u];
    static uint16_t     pal[256], back[RDC_Z_WIN], want[RDC_Z_WIN];
    for (int k = 0; k < 256; k++) {
        pal[k] = (uint16_t)(k * 251u + 7u);
    }
    rdc_z_reset(&z);
    uint32_t len = 0;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t done = 0;
        while (done < w) {
            uint32_t       taken = 0;
            const uint32_t room = (cap < sizeof(buf) - len) ? cap
                                                            : sizeof(buf) - len;
            const uint32_t n = rdc_pack8z_some(&z, ix, y * w + done, w - done,
                                               buf + len, room, &taken);
            AG_CHECK(taken > 0u || room < 3u);
            if (taken == 0u) {
                return 0u;
            }
            len += n;
            done += taken;
        }
    }
    for (uint32_t i = 0; i < w * h; i++) {
        want[i] = pal[ix[i]];
    }
    memset(back, 0xEE, sizeof(back));
    AG_CHECK_INT((int)rdc_unpack8z(buf, len, pal, back, w * h), (int)len);
    AG_CHECK(memcmp(back, want, w * h * 2u) == 0);
    return len;
}

static void copies_come_back_and_are_smaller(void)
{
    static uint8_t ix[RDC_Z_WIN];
    /* noise: no worse than a byte in 128 over the indices */
    for (uint32_t i = 0; i < 320u * 6u; i++) {
        ix[i] = (uint8_t)rnd16();
    }
    const uint32_t noise = z_round_trip(ix, 320, 6, 4096);
    AG_CHECK(noise <= 6u * RDC_PACK8Z_MAX(320u)); /* a row a call */
    /* a tile 37 wide repeating along the rows, the rows shifted by one: a
     * map; PackBits sees literals, copies see the tile */
    for (uint32_t y = 0; y < 12u; y++) {
        for (uint32_t x = 0; x < 320u; x++) {
            ix[y * 320u + x] = (uint8_t)(((x + y) % 37u) * 5u + (y & 1u));
        }
    }
    const uint32_t tiles = z_round_trip(ix, 320, 12, 4096);
    AG_CHECK(tiles * 4u < 320u * 12u); /* under a quarter of the indices */
    /* the same in small pieces, as a nearly full message cuts it */
    AG_CHECK(z_round_trip(ix, 320, 12, 17) > 0u);
    /* runs and flats */
    memset(ix, 9, 320u * 12u);
    AG_CHECK(z_round_trip(ix, 320, 12, 4096) < 200u);
    /* a damaged packing is not pixels: an offset reaching before the start */
    static uint16_t back[16], pal[256];
    const uint8_t   bad[] = {0xC0u, 5u, 0u};
    AG_CHECK_INT((int)rdc_unpack8z(bad, sizeof(bad), pal, back, 3), 0);
}

void run_rdcodec_tests(void)
{
    copies_come_back_and_are_smaller();
    flat_packs_to_almost_nothing();
    noise_costs_one_byte_in_257();
    mixed_rows_come_back();
    too_little_room_is_refused();
    a_damaged_packing_is_not_pixels();
}
