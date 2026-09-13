/*
 * ArgonOS - the two halves of the link to a phone, checked against each other.
 *
 * The board has no way to catch either of the failures that matter here.  A
 * band encoder and a band decoder that agree with each other and are both wrong
 * produce a picture that looks plausible and is not the frame that was sent; a
 * WebSocket handshake computed wrongly is a browser that says "connection
 * failed" and names nothing.  Neither shows up as a crash, and looking at the
 * glass does not settle either.
 *
 * So both are compiled here and asked to reproduce their input exactly.
 * Unlike test_rempix.c - which mirrors REMDISP's encoder in a copy, because a
 * .SYS for Xtensa cannot be linked into a host binary - these are the shipped
 * files: apps/common/pixband/ag_pixband.c and apps/common/ws/ag_ws.c are plain
 * C with no ABI in them, so what runs here is what runs on the board.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include <stdlib.h>

#include "ag_pixband.h"
#include "ag_ws.h"

#define MAX_PX 2560u /* 320 x 8, one band */

static uint32_t s_rand = 12345u;

static uint32_t rnd(void)
{
    s_rand = s_rand * 1103515245u + 12345u;
    return (s_rand >> 8);
}

/*
 * One round trip, and the assertion that matters: bit for bit.
 *
 * Returns the encoded length so a caller can also say something about size -
 * "it came back right" and "it was worth sending" are two different claims and
 * a codec has to make both.
 */
static uint32_t round_trip(ag_pixband_ctx_t *ctx, const uint16_t *px,
                           uint32_t n, uint8_t *want_op)
{
    uint8_t  enc[MAX_PX * 2u + MAX_PX / 64u + 512u];
    uint16_t back[MAX_PX];
    uint8_t  op = 0;

    const uint32_t len = ag_pixband_encode(ctx, px, n, enc, sizeof(enc), &op);
    AG_CHECK(len > 0u);
    if (len == 0u) {
        return 0;
    }
    if (want_op != NULL) {
        *want_op = op;
    }

    memset(back, 0xa5, sizeof(back));
    AG_CHECK(ag_pixband_decode(op, enc, len, back, n));
    AG_CHECK(memcmp(px, back, (size_t)n * sizeof(uint16_t)) == 0);
    return len;
}

static void pixband_tests(void)
{
    void *mem = malloc(ag_pixband_size(MAX_PX));
    AG_CHECK(mem != NULL);
    AG_CHECK(ag_pixband_init(mem, ag_pixband_size(MAX_PX), MAX_PX));

    ag_pixband_ctx_t *ctx = (ag_pixband_ctx_t *)mem;
    static uint16_t   px[MAX_PX];

    /* A flat band: runs all the way, so PackBits or indices, never raw. */
    for (uint32_t i = 0; i < MAX_PX; i++) {
        px[i] = 0x1234u;
    }
    uint8_t op = 0;
    uint32_t len = round_trip(ctx, px, MAX_PX, &op);
    AG_CHECK(op != AG_PIXBAND_RAW);
    AG_CHECK(len < MAX_PX * 2u / 10u);

    /*
     * Sixteen colours: the case the indexed encoding exists for.  Four bits a
     * pixel before a run is looked at, so it must beat raw by a lot and it must
     * be the one chosen.
     */
    for (uint32_t i = 0; i < MAX_PX; i++) {
        px[i] = (uint16_t)((rnd() & 0x0fu) * 0x1111u);
    }
    len = round_trip(ctx, px, MAX_PX, &op);
    AG_CHECK_INT(op, AG_PIXBAND_IDX);
    AG_CHECK(len < MAX_PX);

    /* Exactly 256 colours still fits a byte of index. */
    for (uint32_t i = 0; i < MAX_PX; i++) {
        px[i] = (uint16_t)((i % 256u) * 257u);
    }
    len = round_trip(ctx, px, MAX_PX, &op);
    AG_CHECK(len > 0u);

    /*
     * Noise: more than 256 colours, no runs.  The indexed encoding must decline
     * and PackBits must not win, so this is where raw is correct - and a codec
     * that "compresses" this would be growing the payload.
     */
    for (uint32_t i = 0; i < MAX_PX; i++) {
        px[i] = (uint16_t)rnd();
    }
    len = round_trip(ctx, px, MAX_PX, &op);
    AG_CHECK_INT(op, AG_PIXBAND_RAW);
    AG_CHECK_INT(len, MAX_PX * 2u);

    /* A gradient - many colours, long-ish runs at the boundaries. */
    for (uint32_t i = 0; i < MAX_PX; i++) {
        px[i] = (uint16_t)(i * 7u);
    }
    (void)round_trip(ctx, px, MAX_PX, &op);

    /* Odd sizes, including the ones where 4-bit packing has half a byte spare. */
    for (uint32_t n = 1u; n <= 9u; n++) {
        for (uint32_t i = 0; i < n; i++) {
            px[i] = (uint16_t)(i & 7u);
        }
        (void)round_trip(ctx, px, n, &op);
    }

    /* One pixel, and a band of two identical pixels (the shortest run). */
    px[0] = 0xbeefu;
    (void)round_trip(ctx, px, 1u, &op);
    px[1] = 0xbeefu;
    (void)round_trip(ctx, px, 2u, &op);

    /*
     * A buffer too small is refused, not half-filled.  The caller that gets 0
     * has asked for a band larger than the space it offered.
     */
    uint8_t tiny[8];
    for (uint32_t i = 0; i < MAX_PX; i++) {
        px[i] = (uint16_t)rnd();
    }
    AG_CHECK_INT(ag_pixband_encode(ctx, px, MAX_PX, tiny, sizeof(tiny), &op),
                 0);

    /* Corrupt payloads are refused rather than trusted. */
    uint16_t back[16];
    const uint8_t bad_op[] = {0, 0, 0, 0};
    AG_CHECK(!ag_pixband_decode('Z', bad_op, sizeof(bad_op), back, 2u));
    /* RAW with the wrong number of bytes for the pixels asked for. */
    AG_CHECK(!ag_pixband_decode(AG_PIXBAND_RAW, bad_op, 3u, back, 2u));
    /* IDX naming an index past the end of its own palette. */
    const uint8_t bad_idx[] = {8u, 0u, 0x34u, 0x12u, 0x00u, 0x05u};
    AG_CHECK(!ag_pixband_decode(AG_PIXBAND_IDX, bad_idx, sizeof(bad_idx), back,
                                1u));

    free(mem);
}

static void ws_tests(void)
{
    char accept[AG_WS_ACCEPT_LEN];

    /*
     * RFC 6455 section 1.3 works the example through by hand, which is what
     * makes it worth having: a SHA-1 or a base64 that is subtly wrong produces
     * a string of the right length and shape, and only a known answer catches
     * it.
     */
    AG_CHECK(ag_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept));
    AG_CHECK_STR(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    /* A second vector, so a hash that happens to match one input cannot pass. */
    AG_CHECK(ag_ws_accept_key("x3JJHMbDL1EzLkh9GBhXDw==", accept));
    AG_CHECK_STR(accept, "HSmrc0sMlYUkAGmm5OPpG2HaGWk=");

    AG_CHECK(!ag_ws_accept_key("", accept));
    AG_CHECK(!ag_ws_accept_key(NULL, accept));

    /* ---- frame headers, built and parsed ------------------------------- */

    ag_ws_hdr_t h;
    uint8_t     buf[16];

    /* Short form: a length below 126 lives in the second byte. */
    AG_CHECK_INT(ag_ws_hdr_build(buf, AG_WS_BIN, 5u, true), 2u);
    AG_CHECK_INT(buf[0], 0x82u);
    AG_CHECK_INT(buf[1], 5u);
    AG_CHECK_INT(ag_ws_hdr_parse(buf, 2u, 4096u, &h), 2);
    AG_CHECK_INT(h.opcode, AG_WS_BIN);
    AG_CHECK_INT(h.len, 5u);
    AG_CHECK(h.fin);
    AG_CHECK(!h.masked);

    /* The 126 boundary is the one an off-by-one lands on. */
    AG_CHECK_INT(ag_ws_hdr_build(buf, AG_WS_BIN, 125u, true), 2u);
    AG_CHECK_INT(ag_ws_hdr_build(buf, AG_WS_BIN, 126u, true), 4u);
    AG_CHECK_INT(ag_ws_hdr_parse(buf, 4u, 4096u, &h), 4);
    AG_CHECK_INT(h.len, 126u);

    AG_CHECK_INT(ag_ws_hdr_build(buf, AG_WS_BIN, 65535u, true), 4u);
    AG_CHECK_INT(ag_ws_hdr_parse(buf, 4u, 70000u, &h), 4);
    AG_CHECK_INT(h.len, 65535u);

    AG_CHECK_INT(ag_ws_hdr_build(buf, AG_WS_BIN, 65536u, true), 10u);
    AG_CHECK_INT(ag_ws_hdr_parse(buf, 10u, 70000u, &h), 10);
    AG_CHECK_INT(h.len, 65536u);

    /* A partial header asks for more rather than guessing. */
    AG_CHECK_INT(ag_ws_hdr_parse(buf, 1u, 70000u, &h), 0);
    AG_CHECK_INT(ag_ws_hdr_parse(buf, 4u, 70000u, &h), 0);

    /* Longer than the caller will hold is a close, not a truncation. */
    AG_CHECK(ag_ws_hdr_parse(buf, 10u, 100u, &h) < 0);

    /* A reserved bit means an extension nobody negotiated. */
    const uint8_t rsv[] = {0xc2u, 0x00u};
    AG_CHECK(ag_ws_hdr_parse(rsv, sizeof(rsv), 4096u, &h) < 0);

    /* Four gigabytes claimed in the high word: refuse. */
    const uint8_t huge[] = {0x82u, 0x7fu, 0x00u, 0x00u, 0x00u,
                            0x01u, 0x00u, 0x00u, 0x00u, 0x00u};
    AG_CHECK(ag_ws_hdr_parse(huge, sizeof(huge), 0xffffffffu, &h) < 0);

    /*
     * A client's frame, masked, as a browser actually sends it: the header
     * carries the key and the payload comes back out by xor.
     */
    const uint8_t masked[] = {0x81u, 0x85u, 0x37u, 0xfau, 0x21u, 0x3du,
                              0x7fu, 0x9fu, 0x4du, 0x51u, 0x58u};
    AG_CHECK_INT(ag_ws_hdr_parse(masked, sizeof(masked), 4096u, &h), 6);
    AG_CHECK(h.masked);
    AG_CHECK_INT(h.opcode, AG_WS_TEXT);
    AG_CHECK_INT(h.len, 5u);

    uint8_t payload[8];
    memcpy(payload, masked + h.hdr, h.len);
    ag_ws_unmask(payload, h.len, h.mask);
    payload[h.len] = '\0';
    AG_CHECK_STR((const char *)payload, "Hello");

    /* Unmasking twice is the identity, which is what makes in-place safe. */
    ag_ws_unmask(payload, h.len, h.mask);
    AG_CHECK(memcmp(payload, masked + h.hdr, h.len) == 0);
}

void run_phonelink_tests(void)
{
    pixband_tests();
    ws_tests();
}
