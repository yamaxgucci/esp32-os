/*
 * ArgonOS - NLINK codec and scheduler tests.
 *
 * The codec half is the usual: exact bytes, endianness, and the ways a frame is
 * rejected.  The scheduler half is the reason this file exists at all.
 *
 * Twice on this project a wire has starved something with a deadline, and both
 * times it was found on hardware, late, by watching a game stutter: once the
 * driver's task sat above applications and never blocked, once a 5 KB band of
 * pixels held the port for 13 ms while a 0.7 ms sound frame waited.  Neither
 * needed a board to find.  Both are statements about which queued frame goes
 * next, and a statement about that can be checked here, in milliseconds, before
 * anything is soldered.
 *
 * So the tests below assert the two properties in numbers rather than in hope:
 * a stream frame offered at the worst possible moment goes out within one
 * quantum, and bulk cannot push it past that however much of it there is.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include "nlink/ag_nlink.h"

/* ---- codec --------------------------------------------------------------- */

static void test_hdr_roundtrip(void)
{
    ag_nlink_hdr_t h = {
        .cls = NL_CLS_BULK,
        .flags = NL_F_PSUM | NL_F_EOM,
        .chan = 7,
        .seq = 0xBEEF,
        .ack = 0x1234,
        .credit = 300,
        .len = 200,
    };
    uint8_t buf[NL_HDR_SIZE];
    ag_nlink_pack(buf, &h);

    ag_nlink_hdr_t g;
    AG_CHECK(ag_nlink_unpack(&g, buf));
    AG_CHECK_INT(g.ver, NL_VERSION);
    AG_CHECK_INT(g.cls, NL_CLS_BULK);
    AG_CHECK_INT(g.flags, NL_F_PSUM | NL_F_EOM);
    AG_CHECK_INT(g.chan, 7);
    AG_CHECK_INT(g.seq, 0xBEEF);
    AG_CHECK_INT(g.ack, 0x1234);
    AG_CHECK_INT(g.credit, 300);
    AG_CHECK_INT(g.len, 200);
    AG_CHECK(g.crc == h.crc);
}

/* The layout, byte for byte.  A field that moves is a link that talks to
 * yesterday's firmware and means something different by every word. */
static void test_exact_bytes(void)
{
    ag_nlink_hdr_t h = {
        .cls = NL_CLS_STREAM,
        .flags = 0,
        .chan = 3,
        .seq = 0x0201,
        .ack = 0x0403,
        .credit = 0x0605,
        .len = 0x0008,
    };
    uint8_t b[NL_HDR_SIZE];
    ag_nlink_pack(b, &h);

    AG_CHECK_INT(b[0], 'N');
    AG_CHECK_INT(b[1], 'L');
    AG_CHECK_INT(b[2], 'N');
    AG_CHECK_INT(b[3], 'K');
    AG_CHECK_INT(b[4], NL_VERSION);
    AG_CHECK_INT(b[5], NL_CLS_STREAM);
    AG_CHECK_INT(b[6], 0);
    AG_CHECK_INT(b[7], 3);
    AG_CHECK_INT(b[8], 0x01); /* little-endian, low byte first */
    AG_CHECK_INT(b[9], 0x02);
    AG_CHECK_INT(b[10], 0x03);
    AG_CHECK_INT(b[11], 0x04);
    AG_CHECK_INT(b[12], 0x05);
    AG_CHECK_INT(b[13], 0x06);
    AG_CHECK_INT(b[14], 0x08);
    AG_CHECK_INT(b[15], 0x00);
}

static void test_reject(void)
{
    ag_nlink_hdr_t h = {.cls = NL_CLS_CONTROL, .len = 4};
    uint8_t        b[NL_HDR_SIZE];
    ag_nlink_hdr_t g;

    ag_nlink_pack(b, &h);
    AG_CHECK(ag_nlink_unpack(&g, b));

    /* magic */
    ag_nlink_pack(b, &h);
    b[0] ^= 0xFF;
    AG_CHECK(!ag_nlink_unpack(&g, b));

    /* a version we do not speak */
    ag_nlink_pack(b, &h);
    b[4] = NL_VERSION + 1u;
    AG_CHECK(!ag_nlink_unpack(&g, b));

    /* a class that does not exist */
    ag_nlink_pack(b, &h);
    b[5] = NL_CLASSES;
    AG_CHECK(!ag_nlink_unpack(&g, b));

    /* a length past the quantum - the field that must never be believed
     * before the CRC is */
    ag_nlink_pack(b, &h);
    b[14] = (uint8_t)((NL_QUANTUM + 1u) & 0xffu);
    b[15] = (uint8_t)((NL_QUANTUM + 1u) >> 8);
    AG_CHECK(!ag_nlink_unpack(&g, b));

    /* one flipped bit anywhere in the header */
    for (unsigned i = 0; i < 16u; i++) {
        ag_nlink_pack(b, &h);
        b[i] ^= 0x01u;
        AG_CHECK(!ag_nlink_unpack(&g, b));
    }
}

/*
 * The reason the header carries its own CRC and the payload carries another.
 * One CRC over both cannot be checked until the payload has been read, and how
 * much to read is the header field in question - so a corrupt length would be
 * believed exactly long enough to do damage with it.  Here the payload is
 * wrecked and the header still verifies, which is what lets a receiver know how
 * many bytes to throw away.
 */
static void test_header_survives_a_wrecked_payload(void)
{
    uint8_t body[64];
    for (unsigned i = 0; i < sizeof(body); i++) {
        body[i] = (uint8_t)i;
    }
    ag_nlink_hdr_t h = {
        .cls = NL_CLS_BULK,
        .flags = NL_F_PSUM,
        .chan = 1,
        .len = (uint16_t)sizeof(body),
    };
    uint8_t hdr[NL_HDR_SIZE];
    uint8_t psum[NL_PSUM_SIZE];
    ag_nlink_pack(hdr, &h);
    ag_nlink_put_psum(psum, body, h.len);

    ag_nlink_hdr_t g;
    AG_CHECK(ag_nlink_unpack(&g, hdr)); /* header good */
    AG_CHECK(ag_nlink_check_psum(&g, body, psum));
    AG_CHECK_INT(g.len, sizeof(body));

    body[31] ^= 0x80u;
    AG_CHECK(ag_nlink_unpack(&g, hdr));           /* still good */
    AG_CHECK(!ag_nlink_check_psum(&g, body, psum)); /* payload is not */
}

/* A frame with no payload checksum claims nothing, so nothing can disagree -
 * that is what makes a dropped-rather-than-retried stream frame cheap. */
static void test_no_psum_is_not_a_failure(void)
{
    uint8_t        body[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ag_nlink_hdr_t h = {.cls = NL_CLS_STREAM, .flags = 0, .len = 8};
    uint8_t        hdr[NL_HDR_SIZE];
    ag_nlink_pack(hdr, &h);

    ag_nlink_hdr_t g;
    AG_CHECK(ag_nlink_unpack(&g, hdr));
    const uint8_t junk[NL_PSUM_SIZE] = {0xDE, 0xAD, 0xBE, 0xEF};
    AG_CHECK(ag_nlink_check_psum(&g, body, junk));
}

/* ---- the scheduler ------------------------------------------------------- */

/* 44.1 kHz stereo 16-bit is 176400 B/s; over a 10 ms period, 1764 bytes. */
#define STREAM_RESERVE 1764u

static void test_control_goes_first(void)
{
    ag_nlink_sched_t s;
    ag_nlink_sched_init(&s, STREAM_RESERVE);

    ag_nlink_sched_offer(&s, NL_CLS_BULK, 100000u, NL_QUANTUM);
    ag_nlink_sched_offer(&s, NL_CLS_STREAM, 512u, 256u);
    ag_nlink_sched_offer(&s, NL_CLS_CONTROL, 32u, 32u);

    AG_CHECK_INT(ag_nlink_sched_pick(&s), NL_CLS_CONTROL);
    ag_nlink_sched_sent(&s, NL_CLS_CONTROL, 32u);
    ag_nlink_sched_offer(&s, NL_CLS_CONTROL, 0u, 0u);

    /* and stream before bulk, while its reservation holds */
    AG_CHECK_INT(ag_nlink_sched_pick(&s), NL_CLS_STREAM);
}

static void test_empty_picks_nothing(void)
{
    ag_nlink_sched_t s;
    ag_nlink_sched_init(&s, STREAM_RESERVE);
    AG_CHECK_INT(ag_nlink_sched_pick(&s), -1);
}

/*
 * THE TEST THIS FILE IS FOR.
 *
 * A megabyte of bulk is queued - a file, an application image - and then one
 * sound frame is offered at the worst possible moment.  It must go out within
 * one quantum of bytes, because a frame already on the wire cannot be
 * interrupted and nothing else may be started ahead of it.
 *
 * On the measured wire (400 KB/s) one quantum is 0.64 ms, inside the 0.73 ms a
 * 44.1 kHz frame period allows.  The 5 KB band that starved the sound on the
 * real board would be 13 ms - twenty periods - and this scheduler cannot emit
 * it, because NL_QUANTUM is a hard cap on a frame and not a suggestion.
 */
static void test_bulk_cannot_starve_a_stream_frame(void)
{
    ag_nlink_sched_t s;
    ag_nlink_sched_init(&s, STREAM_RESERVE);

    uint32_t bulk_left = 1024u * 1024u;
    ag_nlink_sched_offer(&s, NL_CLS_BULK, bulk_left, NL_QUANTUM);

    /* Let bulk get going, so the wire is as busy as it can be. */
    for (int i = 0; i < 50; i++) {
        AG_CHECK_INT(ag_nlink_sched_pick(&s), NL_CLS_BULK);
        ag_nlink_sched_sent(&s, NL_CLS_BULK, NL_QUANTUM);
        bulk_left -= NL_QUANTUM;
        ag_nlink_sched_offer(&s, NL_CLS_BULK, bulk_left, NL_QUANTUM);
    }

    /* Now the sound frame arrives. */
    ag_nlink_sched_offer(&s, NL_CLS_STREAM, 176u, 176u);
    const uint32_t bound = ag_nlink_sched_worst_wait(&s, NL_CLS_STREAM);

    /* It is the very next thing the wire does. */
    AG_CHECK_INT(ag_nlink_sched_pick(&s), NL_CLS_STREAM);

    /* And the promised bound is one quantum plus what was queued ahead of it -
     * not one byte of the megabyte behind. */
    AG_CHECK_INT(bound, NL_QUANTUM + 176u);
    AG_CHECK(bound < 1024u);
}

/*
 * The reservation is a floor under the stream, not a ceiling over it.  Spend it
 * all, and bulk gets the wire back - but with bulk empty the stream keeps
 * going, because idling a wire to enforce a cap serves nobody.
 */
static void test_reservation_is_a_floor_not_a_ceiling(void)
{
    ag_nlink_sched_t s;
    ag_nlink_sched_init(&s, 512u);

    ag_nlink_sched_offer(&s, NL_CLS_STREAM, 4096u, 256u);
    ag_nlink_sched_offer(&s, NL_CLS_BULK, 4096u, NL_QUANTUM);

    /* two frames of 256 spend the 512 */
    AG_CHECK_INT(ag_nlink_sched_pick(&s), NL_CLS_STREAM);
    ag_nlink_sched_sent(&s, NL_CLS_STREAM, 256u);
    ag_nlink_sched_offer(&s, NL_CLS_STREAM, 3840u, 256u);
    AG_CHECK_INT(ag_nlink_sched_pick(&s), NL_CLS_STREAM);
    ag_nlink_sched_sent(&s, NL_CLS_STREAM, 256u);
    ag_nlink_sched_offer(&s, NL_CLS_STREAM, 3584u, 256u);

    /* spent: bulk gets its turn */
    AG_CHECK_INT(ag_nlink_sched_pick(&s), NL_CLS_BULK);

    /* with no bulk waiting, the stream continues rather than the wire idling */
    ag_nlink_sched_offer(&s, NL_CLS_BULK, 0u, 0u);
    AG_CHECK_INT(ag_nlink_sched_pick(&s), NL_CLS_STREAM);

    /* a new period refills it */
    ag_nlink_sched_tick(&s);
    ag_nlink_sched_offer(&s, NL_CLS_BULK, 4096u, NL_QUANTUM);
    AG_CHECK_INT(ag_nlink_sched_pick(&s), NL_CLS_STREAM);
}

/*
 * A reservation too small for the stream it was sized for is the one fault this
 * scheduler can have that is nobody's bug, and it has to be visible as a number
 * rather than as a stutter somebody hears three weeks later.
 */
static void test_undersized_reservation_is_counted(void)
{
    ag_nlink_sched_t s;
    ag_nlink_sched_init(&s, 256u); /* one frame per period, and two arrive */

    ag_nlink_sched_offer(&s, NL_CLS_STREAM, 1024u, 256u);
    ag_nlink_sched_offer(&s, NL_CLS_BULK, 99999u, NL_QUANTUM);

    for (int period = 0; period < 4; period++) {
        AG_CHECK_INT(ag_nlink_sched_pick(&s), NL_CLS_STREAM);
        ag_nlink_sched_sent(&s, NL_CLS_STREAM, 256u);
        ag_nlink_sched_offer(&s, NL_CLS_STREAM, 1024u, 256u); /* still backed up */
        AG_CHECK_INT(ag_nlink_sched_pick(&s), NL_CLS_BULK);
        ag_nlink_sched_tick(&s);
    }
    AG_CHECK_INT(s.stream_starved, 4);
    AG_CHECK_INT(s.sent[NL_CLS_STREAM], 4);

    /* A reservation that fits does not count anything. */
    ag_nlink_sched_t ok;
    ag_nlink_sched_init(&ok, STREAM_RESERVE);
    ag_nlink_sched_offer(&ok, NL_CLS_STREAM, 176u, 176u);
    AG_CHECK_INT(ag_nlink_sched_pick(&ok), NL_CLS_STREAM);
    ag_nlink_sched_sent(&ok, NL_CLS_STREAM, 176u);
    ag_nlink_sched_offer(&ok, NL_CLS_STREAM, 0u, 0u);
    ag_nlink_sched_tick(&ok);
    AG_CHECK_INT(ok.stream_starved, 0);
}

/*
 * Bulk is unbounded on purpose, and saying so out loud is worth a check: it is
 * the class that pays for the other two being bounded, and a link where every
 * class had a bound would be a link with no room to absorb anything.
 */
static void test_the_bounds_are_what_was_promised(void)
{
    ag_nlink_sched_t s;
    ag_nlink_sched_init(&s, STREAM_RESERVE);

    AG_CHECK_INT(ag_nlink_sched_worst_wait(&s, NL_CLS_CONTROL), NL_QUANTUM);
    AG_CHECK_INT(ag_nlink_sched_worst_wait(&s, NL_CLS_STREAM), NL_QUANTUM);
    AG_CHECK(ag_nlink_sched_worst_wait(&s, NL_CLS_BULK) == UINT32_MAX);

    /* 256 bytes at the measured 400 KB/s is 0.64 ms, inside one 44.1 kHz
     * stereo frame period of 0.726 ms.  That is the whole argument for the
     * quantum being this number, so it is checked rather than asserted in a
     * comment. */
    const double quantum_ms = (double)NL_QUANTUM * 1000.0 / 400000.0;
    AG_CHECK(quantum_ms < 0.726);
}

void run_nlink_tests(void)
{
    test_hdr_roundtrip();
    test_exact_bytes();
    test_reject();
    test_header_survives_a_wrecked_payload();
    test_no_psum_is_not_a_failure();
    test_control_goes_first();
    test_empty_picks_nothing();
    test_bulk_cannot_starve_a_stream_frame();
    test_reservation_is_a_floor_not_a_ceiling();
    test_undersized_reservation_is_counted();
    test_the_bounds_are_what_was_promised();
}
