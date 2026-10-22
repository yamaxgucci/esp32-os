/*
 * ArgonOS - the WebSocket the phone talks over, on the host.
 *
 * The handshake either works or the browser refuses the connection, so that
 * half is self-announcing.  The framing is not: a length field read one byte
 * short, or a mask applied from the wrong offset, produces a connection that
 * works perfectly until an event happens to be 126 bytes long or a browser
 * happens to send a ping.  Both are rare enough to be found months later and on
 * a board.  So they are found here.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include "ag_kvmws.h"

#include <string.h>

static void hex(const uint8_t *p, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = d[p[i] >> 4];
        out[i * 2 + 1] = d[p[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

static void sha1_of(const char *s, char *out_hex)
{
    ag_sha1_t st;
    uint8_t   d[20];
    ag_sha1_init(&st);
    ag_sha1_update(&st, s, strlen(s));
    ag_sha1_final(&st, d);
    hex(d, 20, out_hex);
}

static void test_sha1(void)
{
    char h[41];

    /* FIPS 180-4 examples, plus the empty string, which is the one that finds
     * a padding loop that assumes there is something to pad. */
    sha1_of("", h);
    AG_CHECK_STR(h, "da39a3ee5e6b4b0d3255bfef95601890afd80709");

    sha1_of("abc", h);
    AG_CHECK_STR(h, "a9993e364706816aba3e25717850c26c9cd0d89d");

    sha1_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", h);
    AG_CHECK_STR(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    /*
     * Fifty-six bytes: the length at which the padding needs a second block,
     * which is the case a hand-written final() gets wrong.
     */
    sha1_of("01234567890123456789012345678901234567890123456789012345", h);
    AG_CHECK_STR(h, "0a40b8fbdaafb7c29651618ac15d27e772287130");

    /* Fed one byte at a time it must produce the same digest: the block
     * buffering is the other half of the code. */
    {
        ag_sha1_t st;
        uint8_t   d[20];
        char      one[41];
        const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        ag_sha1_init(&st);
        for (const char *p = msg; *p != '\0'; p++) {
            ag_sha1_update(&st, p, 1);
        }
        ag_sha1_final(&st, d);
        hex(d, 20, one);
        AG_CHECK_STR(one, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    }
}

static void test_base64(void)
{
    char out[64];

    AG_CHECK_INT(ag_base64("", 0, out, sizeof(out)), 0);
    AG_CHECK_STR(out, "");
    AG_CHECK_INT(ag_base64("f", 1, out, sizeof(out)), 4);
    AG_CHECK_STR(out, "Zg==");
    AG_CHECK_INT(ag_base64("fo", 2, out, sizeof(out)), 4);
    AG_CHECK_STR(out, "Zm8=");
    AG_CHECK_INT(ag_base64("foo", 3, out, sizeof(out)), 4);
    AG_CHECK_STR(out, "Zm9v");
    AG_CHECK_INT(ag_base64("foob", 4, out, sizeof(out)), 8);
    AG_CHECK_STR(out, "Zm9vYg==");
    AG_CHECK_INT(ag_base64("fooba", 5, out, sizeof(out)), 8);
    AG_CHECK_STR(out, "Zm9vYmE=");
    AG_CHECK_INT(ag_base64("foobar", 6, out, sizeof(out)), 8);
    AG_CHECK_STR(out, "Zm9vYmFy");

    /* A buffer one byte short writes nothing rather than a truncated string. */
    AG_CHECK_INT(ag_base64("foobar", 6, out, 8), 0);
}

static void test_handshake(void)
{
    char accept[AG_KVMWS_ACCEPT_LEN];

    /* RFC 6455 §1.3, the worked example. */
    AG_CHECK(ag_kvmws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", 24, accept));
    AG_CHECK_STR(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    AG_CHECK(!ag_kvmws_accept_key("", 0, accept));
    AG_CHECK(!ag_kvmws_accept_key(NULL, 4, accept));
}

/* Builds a client frame the way a browser does: masked, one piece. */
static size_t make_frame(uint8_t *buf, uint8_t opcode, const void *payload,
                         size_t len, const uint8_t mask[4])
{
    size_t pos = 0;

    buf[pos++] = (uint8_t)(0x80u | opcode);
    if (len < 126u) {
        buf[pos++] = (uint8_t)(0x80u | len);
    } else {
        buf[pos++] = (uint8_t)(0x80u | 126u);
        buf[pos++] = (uint8_t)(len >> 8);
        buf[pos++] = (uint8_t)(len & 0xFF);
    }
    memcpy(buf + pos, mask, 4);
    pos += 4;
    for (size_t i = 0; i < len; i++) {
        buf[pos + i] = (uint8_t)(((const uint8_t *)payload)[i] ^ mask[i & 3]);
    }
    return pos + len;
}

static void test_decode(void)
{
    static const uint8_t mask[4] = {0x37, 0xFA, 0x21, 0x3D};
    uint8_t              buf[512];
    ag_kvmws_frame_t        f;

    /* A short text frame, which is what an event is. */
    const char    *line = "M 3 -4 0 0";
    const size_t   n = make_frame(buf, AG_KVMWS_TEXT, line, strlen(line), mask);
    const int32_t  used = ag_kvmws_decode(buf, n, &f);
    AG_CHECK_INT(used, (int)n);
    AG_CHECK_INT(f.opcode, AG_KVMWS_TEXT);
    AG_CHECK(f.fin);
    AG_CHECK_INT(f.len, (int)strlen(line));
    AG_CHECK(memcmp(f.payload, line, f.len) == 0);

    /* Half a frame is not an error, it is "come back with more". */
    AG_CHECK_INT(ag_kvmws_decode(buf, 1, &f), 0);
    AG_CHECK_INT(ag_kvmws_decode(buf, n - 1, &f), 0);

    /* Exactly 126 bytes: the length grows a two-byte field, and this is the
     * boundary a hand-written parser reads one byte short at. */
    {
        char big[200];
        memset(big, 'a', sizeof(big));
        const size_t bn = make_frame(buf, AG_KVMWS_TEXT, big, 126, mask);
        AG_CHECK_INT(bn, 2 + 2 + 4 + 126);
        AG_CHECK_INT(ag_kvmws_decode(buf, bn, &f), (int)bn);
        AG_CHECK_INT(f.len, 126);
        AG_CHECK_INT(f.payload[0], 'a');
        AG_CHECK_INT(f.payload[125], 'a');

        /* And 125, which still fits the short field. */
        const size_t sn = make_frame(buf, AG_KVMWS_TEXT, big, 125, mask);
        AG_CHECK_INT(sn, 2 + 4 + 125);
        AG_CHECK_INT(ag_kvmws_decode(buf, sn, &f), (int)sn);
        AG_CHECK_INT(f.len, 125);
    }

    /* A ping is a control frame and must be answered, so it has to arrive. */
    {
        const size_t pn = make_frame(buf, AG_KVMWS_PING, "hi", 2, mask);
        AG_CHECK_INT(ag_kvmws_decode(buf, pn, &f), (int)pn);
        AG_CHECK_INT(f.opcode, AG_KVMWS_PING);
        AG_CHECK_INT(f.len, 2);
    }

    /* Two frames back to back: the decoder must report what it consumed so the
     * caller can find the second one. */
    {
        const size_t a = make_frame(buf, AG_KVMWS_TEXT, "K d 4 0", 7, mask);
        const size_t b = make_frame(buf + a, AG_KVMWS_TEXT, "K u 4 0", 7, mask);
        AG_CHECK_INT(ag_kvmws_decode(buf, a + b, &f), (int)a);
        AG_CHECK(memcmp(f.payload, "K d 4 0", 7) == 0);
        AG_CHECK_INT(ag_kvmws_decode(buf + a, b, &f), (int)b);
        AG_CHECK(memcmp(f.payload, "K u 4 0", 7) == 0);
    }
}

static void test_decode_refusals(void)
{
    static const uint8_t mask[4] = {1, 2, 3, 4};
    uint8_t              buf[64];
    ag_kvmws_frame_t        f;

    /* Unmasked from a client: RFC 6455 says fail the connection, and masking
     * is the thing that stops a page steering a proxy - so not "be lenient". */
    buf[0] = 0x81;
    buf[1] = 0x02;
    buf[2] = 'h';
    buf[3] = 'i';
    AG_CHECK_INT(ag_kvmws_decode(buf, 4, &f), AG_KVMWS_ERR_UNMASKED);

    /* A fragment.  Nothing here sends one, and reassembling would mean holding
     * a buffer open at a remote party's discretion. */
    const size_t n = make_frame(buf, AG_KVMWS_TEXT, "hi", 2, mask);
    buf[0] &= (uint8_t)~0x80u; /* clear FIN */
    AG_CHECK_INT(ag_kvmws_decode(buf, n, &f), AG_KVMWS_ERR_FRAGMENT);

    /* A 64-bit length is refused outright rather than parsed and rejected
     * later: the point is to never size a buffer from it. */
    buf[0] = 0x81;
    buf[1] = (uint8_t)(0x80u | 127u);
    memset(buf + 2, 0, 8);
    AG_CHECK_INT(ag_kvmws_decode(buf, 20, &f), AG_KVMWS_ERR_TOOBIG);
}

static void test_header(void)
{
    uint8_t hdr[4];

    AG_CHECK_INT(ag_kvmws_header(hdr, AG_KVMWS_TEXT, 5), 2);
    AG_CHECK_INT(hdr[0], 0x81);
    AG_CHECK_INT(hdr[1], 5); /* no mask bit: a server never masks */

    AG_CHECK_INT(ag_kvmws_header(hdr, AG_KVMWS_TEXT, 125), 2);
    AG_CHECK_INT(hdr[1], 125);

    AG_CHECK_INT(ag_kvmws_header(hdr, AG_KVMWS_BIN, 126), 4);
    AG_CHECK_INT(hdr[0], 0x82);
    AG_CHECK_INT(hdr[1], 126);
    AG_CHECK_INT(hdr[2], 0);
    AG_CHECK_INT(hdr[3], 126);

    AG_CHECK_INT(ag_kvmws_header(hdr, AG_KVMWS_TEXT, 65535), 4);
    AG_CHECK_INT(hdr[2], 0xFF);
    AG_CHECK_INT(hdr[3], 0xFF);

    AG_CHECK_INT(ag_kvmws_header(hdr, AG_KVMWS_TEXT, 65536), 0);

    AG_CHECK_INT(ag_kvmws_header(hdr, AG_KVMWS_PONG, 0), 2);
    AG_CHECK_INT(hdr[0], 0x8A);
    AG_CHECK_INT(hdr[1], 0);
}

void run_ws_tests(void)
{
    test_sha1();
    test_base64();
    test_handshake();
    test_decode();
    test_decode_refusals();
    test_header();
}
