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

    /*
     * SHA-1 on its own, because the password challenge uses it directly and a
     * hash that is only ever checked through base64 can be wrong in ways the
     * handshake test would not separate.  The three vectors are FIPS 180-1's
     * own, including the empty string - which is the case a padding bug hits
     * first and the one a handshake never exercises.
     */
    static const struct {
        const char *in;
        const char *hex;
    } k_sha1[] = {
        {"", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
        {"abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
    };
    for (uint32_t i = 0; i < sizeof(k_sha1) / sizeof(k_sha1[0]); i++) {
        uint8_t digest[20];
        char    hex[41];
        ag_ws_sha1(k_sha1[i].in, strlen(k_sha1[i].in), digest);
        for (uint32_t j = 0; j < 20u; j++) {
            static const char d[] = "0123456789abcdef";
            hex[j * 2u] = d[digest[j] >> 4];
            hex[j * 2u + 1u] = d[digest[j] & 15u];
        }
        hex[40] = '\0';
        AG_CHECK_STR(hex, k_sha1[i].hex);
    }

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

/*
 * The client half, which exists so that a board can test this link without a
 * person holding a phone.  Every defect here is a parse or a hash, and both
 * are catchable on the PC - which is the whole reason ag_ws has no socket in
 * it.
 */
static void ws_client_tests(void)
{
    /*
     * RFC 6455's own example, read backwards: the key in section 1.3 is the
     * base64 of sixteen bytes, and those bytes are "the sample nonce".  So
     * building the key from them must produce exactly the key the server-side
     * test above is fed - the two halves meet on a value neither of them
     * invented.
     */
    static const uint8_t k_nonce[16] = {
        't', 'h', 'e', ' ', 's', 'a', 'm', 'p',
        'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e',
    };
    char key[AG_WS_KEY_LEN];
    AG_CHECK(ag_ws_client_key(k_nonce, key));
    AG_CHECK_STR(key, "dGhlIHNhbXBsZSBub25jZQ==");

    AG_CHECK(!ag_ws_client_key(NULL, key));

    /* And the answer that key must come back with. */
    AG_CHECK(ag_ws_accept_ok(key, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));

    /*
     * The failures matter more than the success: a client that accepts any
     * answer has not checked anything, and the ways a wrong answer arrives are
     * short, long, and right-length-wrong-content.
     */
    AG_CHECK(!ag_ws_accept_ok(key, "s3pPLMBiTxaQ9kYGzzhZRbK+xOp="));
    AG_CHECK(!ag_ws_accept_ok(key, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo"));
    AG_CHECK(!ag_ws_accept_ok(key, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=="));
    AG_CHECK(!ag_ws_accept_ok(key, ""));
    AG_CHECK(!ag_ws_accept_ok(key, NULL));

    /*
     * A masked frame this side builds must be one the server side parses, and
     * the payload must come back out of it unchanged.  That round trip is the
     * only thing that proves the mask bit, the four mask bytes and the header
     * length all agree - each is easy to get right alone and easy to get wrong
     * together.
     */
    static const uint8_t k_mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    uint8_t frame[64];
    const char *msg = "Hello";
    const uint32_t n = (uint32_t)strlen(msg);

    const uint32_t hdr = ag_ws_hdr_build_masked(frame, AG_WS_TEXT, n, true,
                                                k_mask);
    AG_CHECK_INT(hdr, 6u);          /* two bytes plus four of mask */
    AG_CHECK((frame[1] & 0x80u) != 0u);
    memcpy(frame + hdr, msg, n);
    ag_ws_mask(frame + hdr, n, k_mask);

    /* Which is the wire, byte for byte, of RFC 6455's masked example. */
    static const uint8_t k_wire[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                     0x7f, 0x9f, 0x4d, 0x51, 0x58};
    AG_CHECK(memcmp(frame, k_wire, sizeof(k_wire)) == 0);

    ag_ws_hdr_t h;
    AG_CHECK_INT(ag_ws_hdr_parse(frame, hdr + n, 1024u, &h), (int32_t)hdr);
    AG_CHECK(h.masked);
    AG_CHECK_INT(h.len, n);
    uint8_t body[8];
    memcpy(body, frame + h.hdr, h.len);
    ag_ws_unmask(body, h.len, h.mask);
    body[h.len] = '\0';
    AG_CHECK_STR((const char *)body, "Hello");

    /* A length that needs the two-byte form still leaves room for the mask. */
    AG_CHECK_INT(ag_ws_hdr_build_masked(frame, AG_WS_BIN, 200u, true, k_mask),
                 8u);
    AG_CHECK_INT(ag_ws_hdr_build_masked(frame, AG_WS_BIN, 70000u, true,
                                        k_mask), 14u);
}

void run_phonelink_tests(void)
{
    pixband_tests();
    ws_tests();
    ws_client_tests();
}
