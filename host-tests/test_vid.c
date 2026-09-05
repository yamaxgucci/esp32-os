/*
 * ArgonOS - the framer over the cable, on the host.
 *
 * The picture arrives as messages on a serial port, and a serial port has no
 * message boundaries and no way to tell you it dropped some bytes.  So the
 * framer has to work out where a message starts, and it has to survive the day
 * it finds rubbish there instead - which will happen, because a full receive
 * buffer discards silently.
 *
 * That failure cannot be produced on a board on purpose and it cannot be seen
 * when it happens: the picture would just stop, or tear, and the cable would be
 * blamed.  Here it is produced on purpose, byte by byte.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include "ag_vid.h"

#include <string.h>

/* A whole message into `out`; returns its length. */
static uint32_t build(uint8_t *out, uint8_t type, const void *payload,
                      uint32_t len)
{
    AG_CHECK(ag_vid_header(out, type, 0, len));
    if (len > 0) {
        memcpy(out + AG_VID_HDR, payload, len);
    }
    return AG_VID_HDR + len;
}

static void test_header(void)
{
    uint8_t hdr[AG_VID_HDR];

    AG_CHECK(ag_vid_header(hdr, AG_VID_TILE, 0, 300));
    AG_CHECK_INT(hdr[0], 'A');
    AG_CHECK_INT(hdr[1], 'G');
    AG_CHECK_INT(hdr[2], AG_VID_TILE);
    AG_CHECK_INT(hdr[3], 0);
    AG_CHECK_INT(ag_vid_u32(hdr + 4), 300);

    /* A payload the board could not hold is refused where it is built, not
     * where it arrives. */
    AG_CHECK(!ag_vid_header(hdr, AG_VID_TILE, 0, AG_VID_MAX_PAYLOAD + 1));
    AG_CHECK(ag_vid_header(hdr, AG_VID_TILE, 0, AG_VID_MAX_PAYLOAD));
}

static void test_one_message(void)
{
    uint8_t      wire[256];
    uint8_t      buf[AG_VID_MAX_MSG];
    ag_vid_rx_t  rx;
    ag_vid_msg_t msg;

    const uint32_t n = build(wire, AG_VID_TILE, "hello", 5);
    ag_vid_rx_init(&rx, buf, sizeof(buf));

    AG_CHECK_INT(ag_vid_rx_feed(&rx, wire, n), (int)n);
    AG_CHECK(ag_vid_rx_next(&rx, &msg));
    AG_CHECK_INT(msg.type, AG_VID_TILE);
    AG_CHECK_INT(msg.len, 5);
    AG_CHECK(memcmp(msg.payload, "hello", 5) == 0);
    /* The whole thing, header included, is what gets forwarded to the phone. */
    AG_CHECK_INT(msg.raw_len, (int)n);
    AG_CHECK_INT(msg.raw[0], 'A');

    AG_CHECK(!ag_vid_rx_next(&rx, &msg));
    AG_CHECK_INT(rx.msgs, 1);
    AG_CHECK_INT(rx.resyncs, 0);
}

static void test_split_and_batched(void)
{
    uint8_t      wire[512];
    uint8_t      buf[AG_VID_MAX_MSG];
    ag_vid_rx_t  rx;
    ag_vid_msg_t msg;

    const uint32_t a = build(wire, AG_VID_TILE, "one", 3);
    const uint32_t b = build(wire + a, AG_VID_TILE, "two", 3);

    /* One byte at a time, which is what a slow serial port looks like.  The
     * framer must not produce a message until the last byte of it. */
    ag_vid_rx_init(&rx, buf, sizeof(buf));
    for (uint32_t i = 0; i < a + b; i++) {
        AG_CHECK_INT(ag_vid_rx_feed(&rx, wire + i, 1), 1);
        const bool got = ag_vid_rx_next(&rx, &msg);
        if (i == a - 1) {
            AG_CHECK(got);
            AG_CHECK(memcmp(msg.payload, "one", 3) == 0);
        } else if (i == a + b - 1) {
            AG_CHECK(got);
            AG_CHECK(memcmp(msg.payload, "two", 3) == 0);
        } else {
            AG_CHECK(!got);
        }
    }
    AG_CHECK_INT(rx.msgs, 2);
    AG_CHECK_INT(rx.resyncs, 0);

    /* Both at once: two messages out of one read, which is the normal case
     * when the far machine sends a burst of tiles. */
    ag_vid_rx_init(&rx, buf, sizeof(buf));
    AG_CHECK_INT(ag_vid_rx_feed(&rx, wire, a + b), (int)(a + b));
    AG_CHECK(ag_vid_rx_next(&rx, &msg));
    AG_CHECK(memcmp(msg.payload, "one", 3) == 0);
    AG_CHECK(ag_vid_rx_next(&rx, &msg));
    AG_CHECK(memcmp(msg.payload, "two", 3) == 0);
    AG_CHECK(!ag_vid_rx_next(&rx, &msg));
}

static void test_lost_bytes(void)
{
    uint8_t      wire[512];
    uint8_t      buf[AG_VID_MAX_MSG];
    ag_vid_rx_t  rx;
    ag_vid_msg_t msg;

    const uint32_t a = build(wire, AG_VID_TILE, "aaaa", 4);
    const uint32_t b = build(wire + a, AG_VID_TILE, "bbbb", 4);

    /* The middle of the first message is eaten, exactly as a full receive
     * buffer eats it: no gap, no marker, the bytes are simply not there. */
    ag_vid_rx_init(&rx, buf, sizeof(buf));
    AG_CHECK_INT(ag_vid_rx_feed(&rx, wire, 5), 5);        /* half of the first */
    AG_CHECK_INT(ag_vid_rx_feed(&rx, wire + a, b), (int)b); /* all of the second */

    /* What comes out is the second message, and the framer says it had to
     * hunt for it.  What must not happen is a message made of the two halves. */
    AG_CHECK(ag_vid_rx_next(&rx, &msg));
    AG_CHECK_INT(msg.len, 4);
    AG_CHECK(memcmp(msg.payload, "bbbb", 4) == 0);
    AG_CHECK(rx.resyncs > 0);

    /* Rubbish in front of a good message: the same answer. */
    ag_vid_rx_init(&rx, buf, sizeof(buf));
    AG_CHECK_INT(ag_vid_rx_feed(&rx, "\x01\x02\x03\x04", 4), 4);
    AG_CHECK_INT(ag_vid_rx_feed(&rx, wire, a), (int)a);
    AG_CHECK(ag_vid_rx_next(&rx, &msg));
    AG_CHECK(memcmp(msg.payload, "aaaa", 4) == 0);
    AG_CHECK_INT(rx.resyncs, 1);

    /* A lone 'A' at the end of what has arrived is not rubbish - it may be the
     * first half of the next magic - so it must be kept rather than eaten. */
    ag_vid_rx_init(&rx, buf, sizeof(buf));
    AG_CHECK_INT(ag_vid_rx_feed(&rx, "\xff\x41", 2), 2);
    AG_CHECK(!ag_vid_rx_next(&rx, &msg));
    AG_CHECK_INT(ag_vid_rx_feed(&rx, wire + 1, a - 1), (int)(a - 1));
    AG_CHECK(ag_vid_rx_next(&rx, &msg));
    AG_CHECK(memcmp(msg.payload, "aaaa", 4) == 0);
}

static void test_absurd_length(void)
{
    uint8_t      buf[AG_VID_MAX_MSG];
    uint8_t      wire[64];
    ag_vid_rx_t  rx;
    ag_vid_msg_t msg;

    /* A header that survived the magic but whose length is impossible.  It
     * must never size anything: the framer treats it as noise and hunts on. */
    wire[0] = 'A';
    wire[1] = 'G';
    wire[2] = AG_VID_TILE;
    wire[3] = 0;
    wire[4] = 0xFF;
    wire[5] = 0xFF;
    wire[6] = 0xFF;
    wire[7] = 0xFF;
    const uint32_t a = build(wire + 8, AG_VID_TILE, "ok", 2);

    ag_vid_rx_init(&rx, buf, sizeof(buf));
    AG_CHECK_INT(ag_vid_rx_feed(&rx, wire, 8 + a), (int)(8 + a));
    AG_CHECK(ag_vid_rx_next(&rx, &msg));
    AG_CHECK(memcmp(msg.payload, "ok", 2) == 0);
    AG_CHECK(rx.resyncs > 0);
}

static void test_full_buffer(void)
{
    uint8_t     buf[AG_VID_MAX_MSG];
    uint8_t     chunk[1024];
    ag_vid_rx_t rx;

    memset(chunk, 0, sizeof(chunk));
    ag_vid_rx_init(&rx, buf, sizeof(buf));

    /* Feeding more than the buffer holds takes what fits and says so, rather
     * than writing past the end of somebody else's memory. */
    uint32_t total = 0;
    for (int i = 0; i < 20; i++) {
        total += ag_vid_rx_feed(&rx, chunk, sizeof(chunk));
    }
    AG_CHECK_INT(total, (int)sizeof(buf));
    AG_CHECK_INT(ag_vid_rx_feed(&rx, chunk, sizeof(chunk)), 0);
}

/*
 * The discipline the caller has to keep, and the bug it exists to stop.
 *
 * A framer whose buffer is finite will take less than it is offered, and the
 * difference is simply gone.  A reader that grabs a fixed chunk off the wire
 * and hands it over regardless loses the tail of every read once the buffer is
 * anywhere near full - and because messages average less than a chunk, a loop
 * that reads one chunk per message emitted fills it within a second.  What that
 * looks like from outside is not a slow picture: it is no picture, and a resync
 * counter climbing forever.  It cost an afternoon in the emulator.
 *
 * So: drain, ask how much room there is, read that much.  This is that loop,
 * against a stream long enough to have overrun the buffer many times over.
 */
static void test_room_discipline(void)
{
    uint8_t      wire[64 * 1024];
    uint8_t      buf[AG_VID_MAX_MSG];
    uint8_t      body[900];
    ag_vid_rx_t  rx;
    ag_vid_msg_t msg;

    memset(body, 'x', sizeof(body));

    /* Sixty messages of about a kilobyte: seven times the buffer. */
    uint32_t total = 0;
    int      built = 0;
    while (total + AG_VID_HDR + sizeof(body) < sizeof(wire) && built < 60) {
        body[0] = (uint8_t)built;
        total += build(wire + total, AG_VID_TILE, body, (uint32_t)sizeof(body));
        built++;
    }

    ag_vid_rx_init(&rx, buf, sizeof(buf));

    uint32_t off = 0;
    int      got = 0;
    while (off < total || rx.have > 0) {
        /* Drain. */
        bool any = false;
        while (ag_vid_rx_next(&rx, &msg)) {
            AG_CHECK_INT(msg.payload[0], got);
            AG_CHECK_INT(msg.len, (int)sizeof(body));
            got++;
            any = true;
        }
        if (off >= total) {
            if (!any) {
                break;
            }
            continue;
        }
        /* Read what fits, and no more. */
        uint32_t room = ag_vid_rx_room(&rx);
        AG_CHECK(room > 0);
        if (room > 1024) {
            room = 1024;
        }
        if (room > total - off) {
            room = total - off;
        }
        AG_CHECK_INT(ag_vid_rx_feed(&rx, wire + off, room), (int)room);
        off += room;
    }

    AG_CHECK_INT(got, built);
    AG_CHECK_INT(rx.resyncs, 0);
}

static void test_payload_shapes(void)
{
    uint8_t        payload[64];
    ag_vid_hello_t hello;
    ag_vid_rect_t  rect;
    const uint8_t *png = NULL;
    uint32_t       png_len = 0;

    /* HELLO: version, flags, then three sixteen-bit numbers little endian. */
    payload[0] = 1;
    payload[1] = 0;
    payload[2] = 0x00;
    payload[3] = 0x05; /* 1280 */
    payload[4] = 0xD0;
    payload[5] = 0x02; /* 720  */
    payload[6] = 0x40;
    payload[7] = 0x00; /* 64   */
    AG_CHECK(ag_vid_parse_hello(payload, 8, &hello));
    AG_CHECK_INT(hello.width, 1280);
    AG_CHECK_INT(hello.height, 720);
    AG_CHECK_INT(hello.tile, 64);

    /* A version this does not know, and a screen of no size, are both refused
     * rather than believed. */
    payload[0] = 2;
    AG_CHECK(!ag_vid_parse_hello(payload, 8, &hello));
    payload[0] = 1;
    payload[2] = 0;
    payload[3] = 0;
    AG_CHECK(!ag_vid_parse_hello(payload, 8, &hello));
    AG_CHECK(!ag_vid_parse_hello(payload, 7, &hello));

    /* TILE: a rectangle, then the image. */
    memset(payload, 0, sizeof(payload));
    payload[0] = 0x40; /* x = 64  */
    payload[2] = 0x80; /* y = 128 */
    payload[4] = 0x20; /* w = 32  */
    payload[6] = 0x20; /* h = 32  */
    memcpy(payload + 8, "\x89PNG", 4);
    AG_CHECK(ag_vid_parse_tile(payload, 12, &rect, &png, &png_len));
    AG_CHECK_INT(rect.x, 64);
    AG_CHECK_INT(rect.y, 128);
    AG_CHECK_INT(rect.w, 32);
    AG_CHECK_INT(rect.h, 32);
    AG_CHECK_INT(png_len, 4);
    AG_CHECK_INT(png[0], 0x89);

    /* A tile of no size is not a tile. */
    payload[4] = 0;
    AG_CHECK(!ag_vid_parse_tile(payload, 12, &rect, &png, &png_len));
    AG_CHECK(!ag_vid_parse_tile(payload, 4, &rect, &png, &png_len));
}

void run_vid_tests(void)
{
    test_header();
    test_one_message();
    test_split_and_batched();
    test_lost_bytes();
    test_absurd_length();
    test_full_buffer();
    test_room_discipline();
    test_payload_shapes();
}
