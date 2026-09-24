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

/* Exactly n pixels out of in[0..len); the bytes read, or 0 if in is not n
 * pixels' worth of a packing (short, long, or damaged). */
uint32_t rdc_unpack(const uint8_t *in, uint32_t len, uint16_t *px, uint32_t n);

#endif /* ARGON_NET_RDCODEC_H */
