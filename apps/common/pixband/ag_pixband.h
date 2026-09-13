/*
 * ArgonOS - a band of pixels on a wire, encoded three ways and sent the
 * smallest.
 *
 * This is the codec REMDISP grew for the serial link to the CYD, lifted out of
 * it so that a second transport does not need a second copy.  PHONE.SYS sends
 * the same bands over a WebSocket to a browser; the encoding has nothing to do
 * with either wire.
 *
 * Three encodings, and the choice is made per band by measuring all three:
 *
 *   RAW   the pixels as they are, RGB565 little-endian.  The floor, and what
 *         wins on a photograph or a gradient.
 *   PACK  PackBits over 16-bit pixels: flat panels, black borders, skies.
 *   IDX   a palette and indices, then PackBits over the indices.  Eight-bit
 *         game art has at most 256 colours in a whole frame and far fewer in
 *         one strip of one scene, so sixteen colours is four bits a pixel
 *         before a single run is looked at - and the runs in an index stream
 *         are longer than the runs in a stream of colour pairs.  The two
 *         multiply, which is where the 7.8x on Fallout comes from.
 *
 * Lossless by construction.  A single wrong pixel out of the decoder is a bug
 * and not a rounding, which is what host-tests/test_pixband.c asserts: both
 * halves are compiled here for the host and asked to reproduce a frame bit for
 * bit.  That test is the reason this file exists as a file - the encoder and
 * decoder used to be two copies in two images for two instruction sets, and
 * the one failure neither board can catch is a pair that agrees with itself
 * and is wrong.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_PIXBAND_H
#define AG_PIXBAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The op byte that says which of the three a payload is. */
#define AG_PIXBAND_RAW 'B'
#define AG_PIXBAND_PACK 'P'
#define AG_PIXBAND_IDX 'I'

/*
 * Working memory for the encoder, sized for the largest band it will be asked
 * for.  The caller allocates one block and keeps it: the hash table alone is
 * six kilobytes, and a codec that allocates per band is a codec that allocates
 * inside a display callback - where, on this system, the memory is charged to
 * whichever process happened to draw.
 *
 *   size_t n = ag_pixband_size(max_px);
 *   ag_pixband_ctx_t *ctx = ag_malloc(n);
 *   ag_pixband_init(ctx, n, max_px);
 *
 * Not reentrant: one band at a time per context.
 */
typedef struct ag_pixband_ctx ag_pixband_ctx_t;

/* Bytes needed for a context that can encode up to `max_px` pixels at once. */
size_t ag_pixband_size(uint32_t max_px);

/* Lay a context out in `bytes` of memory at `mem`.  False if it is too small. */
bool ag_pixband_init(void *mem, size_t bytes, uint32_t max_px);

/*
 * Encode `n` pixels (n <= max_px) into `out`, at most `cap` bytes.
 *
 * Returns the payload length and sets *op to the encoding chosen, or 0 when
 * nothing fits in `cap` - a caller that gets 0 has asked for a band larger than
 * the buffer it offered, which is its bug and not a picture to be truncated.
 *
 * The geometry is NOT part of this.  A transport puts x/y/w/h where its own
 * framing wants them; only the pixels are here.
 */
uint32_t ag_pixband_encode(ag_pixband_ctx_t *ctx, const uint16_t *px,
                           uint32_t n, uint8_t *out, uint32_t cap, uint8_t *op);

/*
 * Undo it: `n` pixels out of `len` bytes of `in`.  False when the payload is
 * malformed or does not contain exactly n pixels - a short or long stream is a
 * corrupt one, and half a band drawn is worse than a band not drawn.
 *
 * No context: decoding needs no working memory.  Here for the host test and for
 * a receiver written in C; the browser client mirrors it in JavaScript, and
 * apps/phone/web/index.html says so at the top of the function.
 */
bool ag_pixband_decode(uint8_t op, const uint8_t *in, uint32_t len,
                       uint16_t *px, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* AG_PIXBAND_H */
