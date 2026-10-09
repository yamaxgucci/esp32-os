/*
 * ArgonOS - the cluster wire format, on the development machine.
 *
 * What is actually being tested here is the thing two boards cannot test
 * cheaply: what happens when several threads send at once.  A cluster job with
 * two workers on the far node has two conversations interleaving on one
 * channel, and the failure if that is wrong is not a crash - it is one
 * message's bytes appearing inside another's, silently, in a program that then
 * gives a wrong answer.  On a board that costs a ten-minute run to see; here it
 * costs a line number.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdlib.h>
#include <string.h>

#include <argon/abi.h>
#include <argon/cluframe.h>

#include "test.h"

static void *t_alloc(uint32_t bytes, void *ctx)
{
    int *count = (int *)ctx;
    if (count != NULL) {
        (*count)++;
    }
    return malloc(bytes);
}

static void t_free(void *p, void *ctx)
{
    int *count = (int *)ctx;
    if (count != NULL) {
        (*count)--;
    }
    free(p);
}

/* Cuts `len` bytes into fragments of `chunk` and feeds them to rx. */
static int feed(ag_cluf_rx_t *rx, int32_t from, int32_t to, const uint8_t *data,
                uint32_t len, uint32_t chunk, uint8_t **out, uint32_t *out_len)
{
    int last = 0;
    for (uint32_t off = 0; off < len; off += chunk) {
        uint32_t take = len - off;
        if (take > chunk) {
            take = chunk;
        }
        ag_cluf_hdr_t h;
        memset(&h, 0, sizeof(h));
        h.op = AG_CLUF_DATA;
        h.flags = (uint8_t)(((off == 0) ? AG_CLUF_F_FIRST : 0u) |
                            ((off + take >= len) ? AG_CLUF_F_LAST : 0u));
        h.len = (uint16_t)take;
        h.a = from;
        h.b = to;
        h.c = len;

        uint8_t frame[AG_CLUF_HDR + 512];
        const size_t n = ag_cluf_put(frame, &h, data + off);

        ag_cluf_hdr_t  got;
        const uint8_t *payload = NULL;
        if (!ag_cluf_get(frame, (uint16_t)n, &got, &payload)) {
            return -1000;
        }
        int32_t f = 0;
        int32_t t = 0;
        last = ag_cluf_rx_data(rx, &got, payload, out, out_len, &f, &t);
        if (last != 0 && last != 1) {
            return last;
        }
    }
    return last;
}

static void header_roundtrip(void)
{
    ag_cluf_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.op = AG_CLUF_SPAWN;
    h.flags = 3;
    h.len = 5;
    h.a = -7;
    h.b = 0x01020304;
    h.c = 4096;

    uint8_t frame[AG_CLUF_HDR + 8];
    const size_t n = ag_cluf_put(frame, &h, "hello");
    AG_CHECK_INT(n, AG_CLUF_HDR + 5);

    ag_cluf_hdr_t  back;
    const uint8_t *payload = NULL;
    AG_CHECK(ag_cluf_get(frame, (uint16_t)n, &back, &payload));
    AG_CHECK_INT(back.op, AG_CLUF_SPAWN);
    AG_CHECK_INT(back.flags, 3);
    AG_CHECK_INT(back.len, 5);
    /* Signed, and it has to survive being signed: a tid is an int32 and -7 is
     * what a failed spawn answers with. */
    AG_CHECK_INT(back.a, -7);
    AG_CHECK_INT(back.b, 0x01020304);
    AG_CHECK_INT((long)back.c, 4096);
    AG_CHECK(payload != NULL && memcmp(payload, "hello", 5) == 0);

    /* A frame that claims more payload than arrived is refused, not trusted. */
    AG_CHECK(!ag_cluf_get(frame, AG_CLUF_HDR + 2, &back, &payload));
    AG_CHECK(!ag_cluf_get(frame, AG_CLUF_HDR - 1, &back, &payload));
}

static void whole_message(void)
{
    int          live = 0;
    ag_cluf_rx_t rx;
    memset(&rx, 0, sizeof(rx));
    rx.alloc_fn = t_alloc;
    rx.free_fn = t_free;
    rx.ctx = &live;

    uint8_t src[1000];
    for (uint32_t i = 0; i < sizeof(src); i++) {
        src[i] = (uint8_t)(i * 7u + 1u);
    }

    uint8_t *out = NULL;
    uint32_t len = 0;
    AG_CHECK_INT(feed(&rx, 3, 0, src, sizeof(src), 232, &out, &len), 1);
    AG_CHECK_INT(len, sizeof(src));
    AG_CHECK(out != NULL && memcmp(out, src, sizeof(src)) == 0);
    t_free(out, &live);
    AG_CHECK_INT(live, 0);

    /* One fragment is still a message, and the common case for small work. */
    out = NULL;
    AG_CHECK_INT(feed(&rx, 1, 2, src, 12, 232, &out, &len), 1);
    AG_CHECK_INT(len, 12);
    t_free(out, &live);
    AG_CHECK_INT(live, 0);
}

/*
 * The one that matters: two senders, fragments alternating, on one channel.
 * A single accumulating buffer would splice these together and nothing would
 * report an error - both messages are only bytes.
 */
static void interleaved_senders(void)
{
    int          live = 0;
    ag_cluf_rx_t rx;
    memset(&rx, 0, sizeof(rx));
    rx.alloc_fn = t_alloc;
    rx.free_fn = t_free;
    rx.ctx = &live;

    uint8_t a[600];
    uint8_t b[600];
    memset(a, 0xaa, sizeof(a));
    memset(b, 0xbb, sizeof(b));

    const uint32_t chunk = 200;
    uint8_t       *done_a = NULL;
    uint8_t       *done_b = NULL;
    uint32_t       len_a = 0;
    uint32_t       len_b = 0;

    for (uint32_t off = 0; off < sizeof(a); off += chunk) {
        for (int who = 0; who < 2; who++) {
            const uint8_t *src = (who == 0) ? a : b;
            ag_cluf_hdr_t  h;
            memset(&h, 0, sizeof(h));
            h.op = AG_CLUF_DATA;
            h.flags = (uint8_t)(((off == 0) ? AG_CLUF_F_FIRST : 0u) |
                                ((off + chunk >= sizeof(a)) ? AG_CLUF_F_LAST
                                                            : 0u));
            h.len = (uint16_t)chunk;
            h.a = (who == 0) ? 1 : 2; /* two different senders */
            h.b = 0;                  /* both answering the master */
            h.c = sizeof(a);

            uint8_t frame[AG_CLUF_HDR + 256];
            const size_t n = ag_cluf_put(frame, &h, src + off);

            ag_cluf_hdr_t  got;
            const uint8_t *payload = NULL;
            AG_CHECK(ag_cluf_get(frame, (uint16_t)n, &got, &payload));

            uint8_t *out = NULL;
            uint32_t len = 0;
            int32_t  from = 0;
            int32_t  to = 0;
            const int r =
                ag_cluf_rx_data(&rx, &got, payload, &out, &len, &from, &to);
            if (r == 1) {
                if (from == 1) {
                    done_a = out;
                    len_a = len;
                } else {
                    done_b = out;
                    len_b = len;
                }
            } else {
                AG_CHECK_INT(r, 0);
            }
        }
    }

    AG_CHECK(done_a != NULL && done_b != NULL);
    AG_CHECK_INT(len_a, sizeof(a));
    AG_CHECK_INT(len_b, sizeof(b));
    if (done_a != NULL && done_b != NULL) {
        AG_CHECK(memcmp(done_a, a, sizeof(a)) == 0);
        AG_CHECK(memcmp(done_b, b, sizeof(b)) == 0);
        t_free(done_a, &live);
        t_free(done_b, &live);
    }
    AG_CHECK_INT(live, 0);
    AG_CHECK_INT((long)rx.dropped, 0);
}

static void bad_input(void)
{
    int          live = 0;
    ag_cluf_rx_t rx;
    memset(&rx, 0, sizeof(rx));
    rx.alloc_fn = t_alloc;
    rx.free_fn = t_free;
    rx.ctx = &live;

    /* A continuation with nothing to continue: the rest of a message whose
     * first fragment was dropped must not be taken for a new one. */
    ag_cluf_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.op = AG_CLUF_DATA;
    h.flags = AG_CLUF_F_LAST;
    h.len = 4;
    h.a = 1;
    h.b = 0;
    h.c = 8;
    uint8_t *out = NULL;
    uint32_t len = 0;
    int32_t  f = 0;
    int32_t  t = 0;
    AG_CHECK(ag_cluf_rx_data(&rx, &h, (const uint8_t *)"abcd", &out, &len, &f,
                             &t) < 0);

    /* More than the message said it would be. */
    h.flags = AG_CLUF_F_FIRST;
    h.len = 4;
    h.c = 6;
    AG_CHECK_INT(ag_cluf_rx_data(&rx, &h, (const uint8_t *)"abcd", &out, &len,
                                 &f, &t),
                 0);
    h.flags = AG_CLUF_F_LAST;
    h.len = 4; /* 4 + 4 > 6 */
    AG_CHECK(ag_cluf_rx_data(&rx, &h, (const uint8_t *)"efgh", &out, &len, &f,
                             &t) < 0);
    AG_CHECK_INT(live, 0); /* and the half-message was given back */

    /* Bigger than any message may be. */
    h.flags = AG_CLUF_F_FIRST;
    h.len = 4;
    h.c = AG_CLU_MSG_MAX + 1u;
    AG_CHECK(ag_cluf_rx_data(&rx, &h, (const uint8_t *)"abcd", &out, &len, &f,
                             &t) < 0);

    /* A sender that starts again mid-message loses the half it had, and the
     * new message still arrives whole. */
    memset(&rx, 0, sizeof(rx));
    rx.alloc_fn = t_alloc;
    rx.free_fn = t_free;
    rx.ctx = &live;
    h.flags = AG_CLUF_F_FIRST;
    h.len = 4;
    h.c = 100;
    AG_CHECK_INT(
        ag_cluf_rx_data(&rx, &h, (const uint8_t *)"abcd", &out, &len, &f, &t),
        0);
    h.flags = (uint8_t)(AG_CLUF_F_FIRST | AG_CLUF_F_LAST);
    h.len = 4;
    h.c = 4;
    AG_CHECK_INT(
        ag_cluf_rx_data(&rx, &h, (const uint8_t *)"wxyz", &out, &len, &f, &t),
        1);
    AG_CHECK_INT(len, 4);
    AG_CHECK_INT((long)rx.dropped, 1);
    t_free(out, &live);
    AG_CHECK_INT(live, 0);
}

/* Nothing half-received survives the end of a job. */
static void reset_frees(void)
{
    int          live = 0;
    ag_cluf_rx_t rx;
    memset(&rx, 0, sizeof(rx));
    rx.alloc_fn = t_alloc;
    rx.free_fn = t_free;
    rx.ctx = &live;

    ag_cluf_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.op = AG_CLUF_DATA;
    h.flags = AG_CLUF_F_FIRST;
    h.len = 4;
    h.a = 5;
    h.b = 0;
    h.c = 400;
    uint8_t *out = NULL;
    uint32_t len = 0;
    int32_t  f = 0;
    int32_t  t = 0;
    AG_CHECK_INT(
        ag_cluf_rx_data(&rx, &h, (const uint8_t *)"abcd", &out, &len, &f, &t),
        0);
    AG_CHECK_INT(live, 1);
    ag_cluf_rx_reset(&rx);
    AG_CHECK_INT(live, 0);
}

void run_cluframe_tests(void)
{
    header_roundtrip();
    whole_message();
    interleaved_senders();
    bad_input();
    reset_frees();
}
