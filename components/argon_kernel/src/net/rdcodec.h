/*
 * ArgonOS - how a run of pixels is packed for a neighbour's screen.
 *
 * RGB565 pixels, sixteen bits each, PackBits-like on the pixel rather than
 * the byte: a control byte c below 128 is followed by c + 1 pixels as they
 * are; c of 128 or more is followed by one pixel that repeats c - 125 times
 * (3 to 130).  A flat stretch of screen - a window, a menu, a background -
 * packs to three bytes in 260; noise costs one byte in 257 more than it was.
 * No memory of the previous frame is needed to unpack it, which is what lets
 * the receiving board be a panel with no framebuffer at all.
 *
 * Pure: no port, no kernel.  The host tests run it (test_rdcodec.c).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NET_RDCODEC_H
#define ARGON_NET_RDCODEC_H

#include <stdint.h>

/* The worst case of rdc_pack for n pixels: every one a literal. */
#define RDC_PACK_MAX(n) (2u * (n) + ((n) + 127u) / 128u)

/* n pixels into out; the bytes written, or 0 if they do not fit in cap. */
uint32_t rdc_pack(const uint16_t *px, uint32_t n, uint8_t *out, uint32_t cap);

/* As many of the n pixels as fit in cap, whole ones from the start: the
 * bytes written, and in *taken how many pixels they are (0 if none fit). */
uint32_t rdc_pack_some(const uint16_t *px, uint32_t n, uint8_t *out,
                       uint32_t cap, uint32_t *taken);

/* Exactly n pixels out of in[0..len); the bytes read, or 0 if in is not n
 * pixels' worth of a packing (short, long, or damaged). */
uint32_t rdc_unpack(const uint8_t *in, uint32_t len, uint16_t *px, uint32_t n);

/*
 * THE SAME ON INDICES INTO A PALETTE (30 Sep 2026): a byte a pixel.  A
 * painted game has at most 256 colours on the screen, and a picture that
 * changes everywhere - its map scrolling - packs to almost as much as it
 * was in RGB565: the byte halves it.  Control bytes as above, the literals
 * and the repeated value one byte each.
 */
#define RDC_PACK8_MAX(n) ((n) + ((n) + 127u) / 128u)
uint32_t rdc_pack8_some(const uint8_t *ix, uint32_t n, uint8_t *out,
                        uint32_t cap, uint32_t *taken);
/* Exactly n pixels out of in[0..len), each index looked up in pal; the bytes
 * read, or 0 if in is not n pixels' worth of a packing. */
uint32_t rdc_unpack8(const uint8_t *in, uint32_t len, const uint16_t *pal,
                     uint16_t *px, uint32_t n);

/*
 * AND COPIES (3 Oct 2026): the same indices, and besides a literal and a run
 * a COPY of what the rectangle already holds - a stretch that is the same as
 * one earlier in it, a row above or a tile to the left.  A screen with no
 * memory of the last frame can still undo it, from the pixels it has just
 * unpacked.  Control bytes:
 *   0x00..0x7F  c + 1 literals follow
 *   0x80..0xBF  one index follows, repeated (c & 0x3F) + 3 times (3..66)
 *   0xC0..0xFF  (c & 0x3F) + 3 pixels (3..66) copied from as far back as
 *               the two bytes that follow say (1..RDC_Z_WIN-1), overlap and all
 * The packer keeps a hash of the last three indices seen at each place
 * (rdc_zstate_t, one per rectangle, reset with rdc_z_reset).
 */
#define RDC_Z_WIN  4096u /* the most pixels a rectangle has, and so a copy's reach */
#define RDC_Z_HASH 1024u
#define RDC_PACK8Z_MAX(n) RDC_PACK8_MAX(n)
typedef struct {
    uint16_t head[RDC_Z_HASH]; /* the latest place + 1 with this hash, 0 none */
    uint16_t prev[RDC_Z_WIN];  /* the place + 1 before it with the same hash */
    uint32_t known;            /* places [0, known) are in the hash */
} rdc_zstate_t;
void rdc_z_reset(rdc_zstate_t *z);
/* Packs ix[from, from + n) of a rectangle whose ix[0, from) went before it
 * (copies reach back into that): as many whole pixels as fit in cap, the
 * bytes written, and in *taken how many pixels. */
uint32_t rdc_pack8z_some(rdc_zstate_t *z, const uint8_t *ix, uint32_t from,
                         uint32_t n, uint8_t *out, uint32_t cap,
                         uint32_t *taken);
/* Exactly n pixels out of in[0..len), indices looked up in pal, copies from
 * px itself; the bytes read, or 0 if in is not n pixels' worth of it. */
uint32_t rdc_unpack8z(const uint8_t *in, uint32_t len, const uint16_t *pal,
                      uint16_t *px, uint32_t n);

#endif /* ARGON_NET_RDCODEC_H */
