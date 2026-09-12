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

/* ---- the window --------------------------------------------------------- */

/*
 * Sequences wrap at 65536, and comparing them with < is the oldest bug in this
 * family of protocol: 0 is after 65535, and every naive comparison says the
 * opposite.  A link that got this wrong would work perfectly for eleven
 * minutes at 5 MB/s and then throw away a window of data, once, and go on
 * working - which is the worst way for a bug to behave.
 */
static void test_sequence_wrap(void)
{
    AG_CHECK(ag_nlink_seq_after(5, 4));
    AG_CHECK(!ag_nlink_seq_after(4, 5));
    AG_CHECK(!ag_nlink_seq_after(4, 4));
    AG_CHECK(ag_nlink_seq_after(0, 65535));      /* the whole point */
    AG_CHECK(!ag_nlink_seq_after(65535, 0));
    AG_CHECK(ag_nlink_seq_after(3, 65530));
    AG_CHECK(!ag_nlink_seq_after(65530, 3));
}

static void test_window_fills_and_drains(void)
{
    ag_nlink_slot_t slots[4];
    ag_nlink_tx_t   tx;
    ag_nlink_tx_init(&tx, NL_CLS_BULK, slots, 4, 50);

    const uint8_t body[16] = {0};
    for (int i = 0; i < 4; i++) {
        AG_CHECK(ag_nlink_tx_room(&tx));
        AG_CHECK(ag_nlink_tx_queue(&tx, 1, body, sizeof(body), i == 3));
    }
    /* Full is back-pressure, not an error. */
    AG_CHECK(!ag_nlink_tx_room(&tx));
    AG_CHECK(!ag_nlink_tx_queue(&tx, 1, body, sizeof(body), false));
    AG_CHECK_INT(ag_nlink_tx_pending(&tx), 4);

    for (int i = 0; i < 4; i++) {
        const ag_nlink_slot_t *s = ag_nlink_tx_next(&tx, 0);
        AG_CHECK(s != NULL);
        AG_CHECK_INT(s->seq, i);
        AG_CHECK_INT(s->flags & NL_F_EOM, (i == 3) ? NL_F_EOM : 0);
    }
    /* Everything sent, nothing due: the wire has nothing to do. */
    AG_CHECK(ag_nlink_tx_next(&tx, 0) == NULL);

    /* A cumulative ack for the second frame frees two slots. */
    ag_nlink_tx_ack(&tx, 1, 0);
    AG_CHECK_INT(ag_nlink_tx_pending(&tx), 2);
    AG_CHECK(ag_nlink_tx_room(&tx));
    ag_nlink_tx_ack(&tx, 3, 0);
    AG_CHECK_INT(ag_nlink_tx_pending(&tx), 0);
    AG_CHECK_INT(tx.retransmits, 0);
}

/*
 * One lost frame must cost one timeout, not one per frame behind it.  A sender
 * that re-sent only the oldest would look gentler and take a whole window of
 * timeouts to recover, because a receiver that discarded everything behind a
 * gap has nothing to acknowledge.
 */
static void test_one_loss_costs_one_timeout(void)
{
    ag_nlink_slot_t slots[8];
    ag_nlink_tx_t   tx;
    ag_nlink_rx_t   rx;
    ag_nlink_tx_init(&tx, NL_CLS_BULK, slots, 8, 50);
    ag_nlink_rx_init(&rx);

    const uint8_t body[8] = {0};
    for (int i = 0; i < 8; i++) {
        AG_CHECK(ag_nlink_tx_queue(&tx, 1, body, sizeof(body), false));
    }

    /* Send all eight; the first is lost on the way. */
    for (int i = 0; i < 8; i++) {
        const ag_nlink_slot_t *s = ag_nlink_tx_next(&tx, 0);
        AG_CHECK(s != NULL);
        if (i == 0) {
            continue; /* dropped by the wire */
        }
        ag_nlink_hdr_t h = {.cls = NL_CLS_BULK, .seq = s->seq, .len = s->len};
        AG_CHECK(!ag_nlink_rx_accept(&rx, &h)); /* all of them are gaps */
    }
    AG_CHECK_INT(rx.gaps, 7);
    AG_CHECK_INT(rx.taken, 0);
    AG_CHECK(ag_nlink_rx_ack(&rx) == NL_ACK_NONE);

    /* Nothing is due before the timeout. */
    AG_CHECK(ag_nlink_tx_next(&tx, 49) == NULL);

    /* One timeout, and the whole window goes again - in order, and all of it. */
    for (int i = 0; i < 8; i++) {
        const ag_nlink_slot_t *s = ag_nlink_tx_next(&tx, 50);
        AG_CHECK(s != NULL);
        AG_CHECK_INT(s->seq, i);
        ag_nlink_hdr_t h = {.cls = NL_CLS_BULK, .seq = s->seq, .len = s->len};
        AG_CHECK(ag_nlink_rx_accept(&rx, &h));
    }
    AG_CHECK_INT(rx.taken, 8);
    AG_CHECK_INT(ag_nlink_rx_ack(&rx), 7);
    AG_CHECK_INT(tx.retransmits, 8);

    ag_nlink_tx_ack(&tx, ag_nlink_rx_ack(&rx), 0);
    AG_CHECK_INT(ag_nlink_tx_pending(&tx), 0);
}

/* A duplicate means the ACKNOWLEDGEMENT was lost, not the frame - a different
 * fault, in the other direction of the wire, and worth counting apart. */
static void test_a_duplicate_is_not_a_gap(void)
{
    ag_nlink_rx_t rx;
    ag_nlink_rx_init(&rx);

    ag_nlink_hdr_t h0 = {.seq = 0};
    ag_nlink_hdr_t h1 = {.seq = 1};
    ag_nlink_hdr_t h3 = {.seq = 3};

    /* A stream starts at zero and nothing else will do: a receiver that adopted
     * whatever arrived first would silently skip the beginning of a transfer
     * whose first frame was lost, and report success. */
    AG_CHECK(!ag_nlink_rx_accept(&rx, &h1));
    AG_CHECK_INT(rx.gaps, 1);
    AG_CHECK(ag_nlink_rx_ack(&rx) == NL_ACK_NONE);

    AG_CHECK(ag_nlink_rx_accept(&rx, &h0));
    AG_CHECK(ag_nlink_rx_accept(&rx, &h1));
    AG_CHECK(!ag_nlink_rx_accept(&rx, &h1)); /* again */
    AG_CHECK(!ag_nlink_rx_accept(&rx, &h0)); /* older still */
    AG_CHECK(!ag_nlink_rx_accept(&rx, &h3)); /* 2 is missing */
    AG_CHECK_INT(rx.dups, 2);
    AG_CHECK_INT(rx.gaps, 2);
    AG_CHECK_INT(rx.taken, 2);
    AG_CHECK_INT(ag_nlink_rx_ack(&rx), 1); /* re-ack, so the peer moves on */
}

/*
 * A retransmission is WORK, and the scheduler has to be told about it.
 *
 * This is here because it was missed, and missed in the way that hurts: the
 * tests above drive ag_nlink_tx_next directly, so the window and the timer were
 * both proved correct - while the driver, which asks the SCHEDULER what to send
 * and only then reaches for a frame, never retransmitted anything at all.
 * waiting_bytes counted unsent frames only, so once every frame had gone out
 * once the class looked idle for ever.  Nothing failed; a transfer simply
 * stopped, on two emulators, after an afternoon.
 *
 * So the property is stated here in the terms the driver uses: offer, pick,
 * send, and ask again after the timer.
 */
static void test_the_scheduler_is_told_about_a_retransmission(void)
{
    ag_nlink_slot_t slots[4];
    ag_nlink_tx_t   tx;
    ag_nlink_sched_t s;
    ag_nlink_tx_init(&tx, NL_CLS_CONTROL, slots, 4, 50);
    ag_nlink_sched_init(&s, STREAM_RESERVE);

    const uint8_t body[8] = {0};
    AG_CHECK(ag_nlink_tx_queue(&tx, 1, body, sizeof(body), true));

    /* Offered, picked, sent - the ordinary path. */
    ag_nlink_sched_offer(&s, NL_CLS_CONTROL, ag_nlink_tx_waiting_bytes(&tx, 0),
                         ag_nlink_tx_head_len(&tx, 0));
    AG_CHECK_INT(ag_nlink_sched_pick(&s), NL_CLS_CONTROL);
    AG_CHECK(ag_nlink_tx_next(&tx, 0) != NULL);
    ag_nlink_sched_sent(&s, NL_CLS_CONTROL, sizeof(body) + NL_HDR_SIZE);

    /* Before the timer: nothing to do, and the wire must be free for others. */
    ag_nlink_sched_offer(&s, NL_CLS_CONTROL, ag_nlink_tx_waiting_bytes(&tx, 10),
                         ag_nlink_tx_head_len(&tx, 10));
    AG_CHECK_INT(ag_nlink_sched_pick(&s), -1);

    /* After it: the class has work again, and the frame really comes back. */
    ag_nlink_sched_offer(&s, NL_CLS_CONTROL, ag_nlink_tx_waiting_bytes(&tx, 50),
                         ag_nlink_tx_head_len(&tx, 50));
    AG_CHECK_INT(ag_nlink_sched_pick(&s), NL_CLS_CONTROL);
    const ag_nlink_slot_t *again = ag_nlink_tx_next(&tx, 50);
    AG_CHECK(again != NULL);
    AG_CHECK_INT(again->seq, 0);
    AG_CHECK_INT(tx.retransmits, 1);

    /* And an acknowledgement ends it: no work, no picking, no spinning. */
    ag_nlink_tx_ack(&tx, 0, 50);
    AG_CHECK_INT(ag_nlink_tx_pending(&tx), 0);
    ag_nlink_sched_offer(&s, NL_CLS_CONTROL, ag_nlink_tx_waiting_bytes(&tx, 500),
                         ag_nlink_tx_head_len(&tx, 500));
    AG_CHECK_INT(ag_nlink_sched_pick(&s), -1);
}

/* ---- a megabyte across a wire that is losing things ---------------------- */

/*
 * THE TEST THAT MUST NOT BE RUN FOR THE FIRST TIME ON HARDWARE.
 *
 * Above 5 Mbaud the measured line corrupts about 1.5% of blocks, and a fast SPI
 * link will corrupt more; at 5 MB/s errors are not a possibility but a rate.  A
 * file transfer that fails on one bad frame is useless, and finding that out on
 * a board means finding it out as "the transfer sometimes stops", weeks later,
 * with no way to tell a protocol bug from a bad solder joint.
 *
 * So: a megabyte through a channel that drops one frame in twenty-five and
 * corrupts one in fifty, byte-compared at the far end.  Deterministic, so a
 * failure can be reproduced rather than chased.
 */
static uint32_t rng(uint32_t *s)
{
    *s = (*s * 1664525u) + 1013904223u;
    return *s >> 16;
}

/*
 * The timeout is measured, and it has to move BOTH ways.
 *
 * Written in the terms the failure came in: on a link whose round trip was
 * longer than a fixed timeout, six frames in thirty-two went out twice with
 * zero frames lost, zero corrupt and zero duplicated - a link working hard and
 * a link working look identical from throughput alone, and the only number that
 * tells them apart is this one.
 */
static void test_the_timeout_follows_the_round_trip(void)
{
    ag_nlink_slot_t slots[8];
    ag_nlink_tx_t   tx;
    const uint8_t   body[8] = {0};

    /* Start it far too short on purpose: 30 ms against a 90 ms round trip. */
    ag_nlink_tx_init(&tx, NL_CLS_CONTROL, slots, 8, 30);

    /* Ten exchanges, each answered after 90 ms. */
    uint32_t now = 0;
    for (int i = 0; i < 10; i++) {
        AG_CHECK(ag_nlink_tx_queue(&tx, 1, body, sizeof(body), true));
        const ag_nlink_slot_t *s = ag_nlink_tx_next(&tx, now);
        AG_CHECK(s != NULL);
        const uint16_t seq = s->seq;
        now += 90;
        ag_nlink_tx_ack(&tx, seq, now);
        now += 10;
    }

    /*
     * It climbed past the round trip.  Not "to" it: a timeout equal to the mean
     * retransmits half of everything by construction, which is why the estimate
     * carries a deviation term at all.
     */
    AG_CHECK(tx.rto_ms > 90u);
    AG_CHECK(tx.rto_ms <= NL_RTO_MAX_MS);
    /* And nothing went twice while it was learning past the first exchange. */
    AG_CHECK(tx.retransmits <= 1u);

    /* Now the wire gets quick, and the estimate has to come back down. */
    for (int i = 0; i < 40; i++) {
        AG_CHECK(ag_nlink_tx_queue(&tx, 1, body, sizeof(body), true));
        const ag_nlink_slot_t *s = ag_nlink_tx_next(&tx, now);
        AG_CHECK(s != NULL);
        const uint16_t seq = s->seq;
        now += 2;
        ag_nlink_tx_ack(&tx, seq, now);
        now += 2;
    }
    AG_CHECK(tx.rto_ms < 90u);
    /* But never below the floor - that is the far end's scheduling, not the
     * wire, and a timeout under it fires on a tick rather than on a loss. */
    AG_CHECK(tx.rto_ms >= NL_RTO_MIN_MS);
}

/*
 * A timeout that is too short must be able to correct itself with no help.
 *
 * This is the half that is easy to leave out and impossible to notice: Karn's
 * rule forbids taking a sample from a frame that was sent twice, so on a link
 * where EVERY frame times out there is never a legal sample, and without a
 * backoff the estimate can never move at all.  The link then retransmits at
 * that rate for as long as it stays up.
 */
static void test_a_timeout_that_never_gets_a_sample_still_backs_off(void)
{
    ag_nlink_slot_t slots[4];
    ag_nlink_tx_t   tx;
    const uint8_t   body[8] = {0};

    ag_nlink_tx_init(&tx, NL_CLS_CONTROL, slots, 4, 40);
    AG_CHECK(ag_nlink_tx_queue(&tx, 1, body, sizeof(body), true));

    const uint32_t first = tx.rto_ms;
    uint32_t       now = 0;
    /* Nobody ever acknowledges.  Walk the clock past each deadline in turn. */
    for (int i = 0; i < 6; i++) {
        AG_CHECK(ag_nlink_tx_next(&tx, now) != NULL);
        now += tx.rto_ms;
    }
    AG_CHECK(tx.rto_ms > first);
    AG_CHECK(tx.rto_ms <= NL_RTO_MAX_MS);
}

static void test_a_megabyte_through_a_bad_wire(void)
{
    enum { TOTAL = 1024u * 1024u, WINDOW = 24 };
    static uint8_t         src[TOTAL];
    static uint8_t         dst[TOTAL];
    static ag_nlink_slot_t slots[WINDOW];

    for (uint32_t i = 0; i < TOTAL; i++) {
        src[i] = (uint8_t)(i * 31u + (i >> 8));
    }
    memset(dst, 0, sizeof(dst));

    ag_nlink_tx_t tx;
    ag_nlink_rx_t rx;
    ag_nlink_tx_init(&tx, NL_CLS_BULK, slots, WINDOW, 20);
    ag_nlink_rx_init(&rx);

    uint32_t seed = 12345u;
    uint32_t off = 0;      /* how much of src has been handed to the window */
    uint32_t got = 0;      /* how much has been delivered in order */
    uint32_t now = 0;
    uint32_t ack_from_peer = NL_ACK_NONE;
    uint32_t steps = 0;
    bool     done = false;

    /* Generous: at ~4100 frames and a 24-frame window this finishes in tens of
     * thousands of steps.  The bound is here so a protocol that livelocks fails
     * the test rather than hanging the suite. */
    const uint32_t max_steps = 4u * 1000u * 1000u;

    while (!done && steps < max_steps) {
        steps++;

        /* Fill the window from the file. */
        while (off < TOTAL && ag_nlink_tx_room(&tx)) {
            uint16_t n = (uint16_t)((TOTAL - off < NL_QUANTUM) ? (TOTAL - off)
                                                               : NL_QUANTUM);
            AG_CHECK(ag_nlink_tx_queue(&tx, 1, &src[off], n,
                                       (off + n) >= TOTAL));
            off += n;
        }

        const ag_nlink_slot_t *s = ag_nlink_tx_next(&tx, now);
        if (s == NULL) {
            now += 5u; /* nothing to do but let the timer run */
            continue;
        }

        /* --- the wire ------------------------------------------------- */
        uint8_t        hdr[NL_HDR_SIZE];
        uint8_t        psum[NL_PSUM_SIZE];
        uint8_t        body[NL_QUANTUM];
        ag_nlink_hdr_t h = {
            .cls = NL_CLS_BULK,
            .flags = s->flags,
            .chan = s->chan,
            .seq = s->seq,
            .ack = (uint16_t)ag_nlink_rx_ack(&rx),
            .len = s->len,
        };
        memcpy(body, s->data, s->len);
        ag_nlink_pack(hdr, &h);
        ag_nlink_put_psum(psum, body, h.len);

        const uint32_t roll = rng(&seed) % 100u;
        bool           lost = (roll < 4u);           /* 4%: never arrives   */
        const bool     bent = (!lost && roll < 6u);  /* 2%: arrives wrong   */
        if (bent) {
            /* Half the time wreck the header, half the time the payload -
             * the two are caught by different checks and both must be. */
            if ((rng(&seed) & 1u) != 0u) {
                hdr[rng(&seed) % 16u] ^= 0x20u;
            } else if (h.len > 0u) {
                body[rng(&seed) % h.len] ^= 0x08u;
            }
        }
        now += 1u;

        if (!lost) {
            ag_nlink_hdr_t g;
            if (!ag_nlink_unpack(&g, hdr)) {
                lost = true; /* a bad header is indistinguishable from silence */
            } else if (!ag_nlink_check_psum(&g, body, psum)) {
                lost = true; /* and so is a bad payload, deliberately */
            } else {
                if (ag_nlink_rx_accept(&rx, &g)) {
                    AG_CHECK(got + g.len <= TOTAL);
                    memcpy(&dst[got], body, g.len);
                    got += g.len;
                    if ((g.flags & NL_F_EOM) != 0u) {
                        done = true;
                    }
                }
                /*
                 * The acknowledgement rides back, and the return direction of
                 * a wire is a wire too.  Scattered losses are not enough here:
                 * a sender holding a slightly stale ack behaves identically to
                 * one holding a current one, so nothing is learned.  What
                 * exercises the duplicate path is a BLACKOUT longer than the
                 * timeout - a receiver too busy to answer for a while, which is
                 * exactly what a board painting a screen is.  Then the sender
                 * gives up waiting and re-sends frames that did arrive, and the
                 * receiver has to recognise them as already-had rather than as
                 * data.  Getting that wrong corrupts a file rather than slowing
                 * one down.
                 */
                const bool blackout = (steps % 500u) < 40u;
                if (!blackout && (rng(&seed) % 100u) >= 10u) {
                    ack_from_peer = ag_nlink_rx_ack(&rx);
                }
            }
        }
        if (ack_from_peer != NL_ACK_NONE) {
            ag_nlink_tx_ack(&tx, (uint16_t)ack_from_peer, now);
        }
    }

    AG_CHECK(done);
    AG_CHECK(steps < max_steps);
    AG_CHECK_INT(got, TOTAL);
    AG_CHECK_INT(memcmp(src, dst, TOTAL), 0);

    /* It cost something, and the number is the point: a link that says it is
     * fine while re-sending half of everything is a link about to fall over,
     * and throughput alone cannot tell the two apart. */
    AG_CHECK(tx.retransmits > 0u);
    AG_CHECK(rx.gaps > 0u);
    /* Both failure modes really happened, so both paths were really taken:
     * a gap is a frame lost on the way out, a duplicate is an acknowledgement
     * lost on the way back, and a test that only ever produced one of them
     * would be silent about half the code. */
    AG_CHECK(rx.dups > 0u);
    AG_CHECK(tx.sent > 4000u);

    /*
     * Printed, not just asserted.  The assertions say it worked; the numbers
     * say what it cost, and the ratio is the thing to watch when the real wire
     * replaces this one - a link doing twice the work for the same throughput
     * looks identical from throughput alone.
     */
    printf("  nlink: 1 MB over a 4%%-loss 2%%-corrupt wire - %u frames sent, "
           "%u of them again (%u%%), %u gaps, %u duplicates\n",
           (unsigned)tx.sent, (unsigned)tx.retransmits,
           (unsigned)(tx.retransmits * 100u / (tx.sent ? tx.sent : 1u)),
           (unsigned)rx.gaps, (unsigned)rx.dups);
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
    test_sequence_wrap();
    test_window_fills_and_drains();
    test_one_loss_costs_one_timeout();
    test_a_duplicate_is_not_a_gap();
    test_the_scheduler_is_told_about_a_retransmission();
    test_the_timeout_follows_the_round_trip();
    test_a_timeout_that_never_gets_a_sample_still_backs_off();
    test_a_megabyte_through_a_bad_wire();
}
