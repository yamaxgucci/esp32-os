/*
 * Host tests for the framed link's protocol (components/argon_kernel/src/
 * net/wlink.c): two ends and a wire between them that loses and damages
 * frames, the clock in the test's hand.  Nothing of a board is in it - the
 * port's locks, wakes, clock and randomness are the stubs in wlink_stub/ - so
 * what is tested is what the protocol promises:
 *
 *   - bytes: every byte arrives, once and in order, through 5% lost and 2%
 *     damaged frames;
 *   - blocks: every reliable block arrives whole; an unreliable one arrives
 *     whole or is named in a LOST record, and each loss is told exactly once;
 *   - sessions: when one end starts over, the other follows it into a new
 *     stream instead of waiting for offsets that will never come.
 *
 * Its own executable, because the stubs stand in for headers the rest of the
 * host tests reach through the `bare` port.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/wlink.h"

int64_t  wlink_stub_now_us = 0;
uint32_t wlink_stub_rng = 12345u;

static int g_checks, g_failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                      \
    } while (0)

#define FRAME 512u
#define PAY   (FRAME - WL_HDR)

static uint32_t s_wire_rng = 777u;

static uint32_t rnd(void)
{
    s_wire_rng = s_wire_rng * 1103515245u + 12345u;
    return s_wire_rng >> 8;
}

static void end_open(wlink_t *w, uint32_t flags)
{
    memset(w, 0, sizeof(*w));
    w->lock = ag_port_mutex_new();
    w->rx_sem = ag_port_sem_new_counting(255u, 0u);
    w->space_sem = ag_port_sem_new_binary();
    w->tx_sem = ag_port_sem_new_binary();
    w->framed = true;
    w->txcap = 4u * PAY;
    w->rxcap = 3u * PAY;
    w->tx = (ring_t){.mem = (uint8_t *)malloc(w->txcap), .cap = w->txcap};
    w->rx = (ring_t){.mem = (uint8_t *)malloc(w->rxcap), .cap = w->rxcap};
    wl_reset(w, FRAME, flags);
    w->open = true;
}

/* A board that starts over: its link opened again, a new session. */
static void end_restart(wlink_t *w, uint32_t flags)
{
    w->tx.head = w->tx.tail = w->tx.count = 0;
    w->rx.head = w->rx.tail = w->rx.count = 0;
    wl_reset(w, FRAME, flags);
}

static void end_close(wlink_t *w)
{
    free(w->tx.mem);
    free(w->rx.mem);
    free(w->lock);
    free(w->rx_sem);
    free(w->space_sem);
    free(w->tx_sem);
}

/* One frame from `a` to `b`, perhaps lost, perhaps damaged on the way. */
static void one_frame(wlink_t *a, wlink_t *b, uint32_t lose_pct,
                      uint32_t damage_pct)
{
    static uint32_t f32[FRAME / 4u];
    uint8_t *const  f = (uint8_t *)f32;
    uint32_t        allow = 0;
    if (a->agreed && a->peer_known) {
        const uint32_t edge = a->peer_ack + a->peer_room;
        const uint32_t next = a->tx_base + a->sent_off;
        if ((int32_t)(edge - next) > 0) {
            allow = edge - next;
        }
    }
    wl_rto_check(a);
    bool     nak_was = false;
    uint32_t wire = 0;
    (void)wl_build(a, f, allow, 0u, &nak_was, &wire);
    if (rnd() % 100u < lose_pct) {
        return;
    }
    if (rnd() % 100u < damage_pct) {
        f[rnd() % wire] ^= (uint8_t)(1u << (rnd() % 8u));
    }
    (void)wl_rx(b, f, wire);
}

static void tick(void) { wlink_stub_now_us += 250; }

static uint8_t pat(uint32_t i) { return (uint8_t)(i * 7u + (i >> 9)); }

/* ---------------------------------------------------------------------- */

static void bytes_through_a_bad_wire(void)
{
    static wlink_t a, b;
    end_open(&a, 0u);
    end_open(&b, 0u);
    const uint32_t total = 200u * 1024u;
    uint32_t       wrote = 0, got = 0, wrong = 0;
    uint8_t        buf[700];
    uint32_t       steps = 0;
    for (; steps < 400000u && got < total; steps++) {
        tick();
        if (wrote < total) {
            uint32_t n = total - wrote;
            if (n > sizeof(buf)) {
                n = sizeof(buf);
            }
            for (uint32_t i = 0; i < n; i++) {
                buf[i] = pat(wrote + i);
            }
            const int32_t w = wl_write(&a, buf, n);
            if (w > 0) {
                wrote += (uint32_t)w;
            }
        }
        one_frame(&a, &b, 5u, 2u);
        one_frame(&b, &a, 5u, 2u);
        for (;;) {
            const int32_t r = wl_read(&b, buf, sizeof(buf), 0u);
            if (r <= 0) {
                break;
            }
            for (int32_t i = 0; i < r; i++) {
                wrong += (buf[i] != pat(got + (uint32_t)i)) ? 1u : 0u;
            }
            got += (uint32_t)r;
        }
    }
    printf("  wlink bytes: %u KB through 5%% lost + 2%% damaged frames in %u "
           "steps, %u sent again, %u bad frames\n",
           (unsigned)(got / 1024u), (unsigned)steps, (unsigned)a.rewinds,
           (unsigned)b.frames_bad);
    CHECK(got == total);
    CHECK(wrong == 0u);
    CHECK(a.rewinds > 0u);    /* the wire did lose, and it was made good */
    CHECK(b.frames_bad > 0u); /* ... and damage was caught */
    end_close(&a);
    end_close(&b);
}

/* ---------------------------------------------------------------------- */

typedef struct {
    uint32_t good, bad, lost_told, lost_reliable, next_no;
} blk_tally_t;

static uint32_t blk_len(uint32_t no) { return 1u + (no * 397u) % 900u; }

static bool blk_reliable(uint32_t no) { return (no % 4u) != 0u; }

static void blk_fill(uint8_t *p, uint32_t no, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        p[i] = (uint8_t)(no * 13u + i);
    }
}

/* Every whole block that is in b's ring, taken and checked. */
static void blk_drain(wlink_t *b, blk_tally_t *t, uint8_t *buf, size_t cap)
{
    for (;;) {
        if (b->rx.count < WL_REC) {
            return;
        }
        uint8_t h[WL_REC];
        (void)ring_peek(&b->rx, h, WL_REC);
        const uint32_t len = (uint32_t)h[0] | ((uint32_t)h[1] << 8);
        if (b->rx.count < WL_REC + len) {
            return; /* the rest is still coming: recv would wait for it */
        }
        ag_spilink_block_t bi;
        memset(&bi, 0, sizeof(bi));
        const int32_t n = wl_recv(b, buf, cap, &bi, 0u);
        if (bi.flags & AG_SPILINK_LOST) {
            const uint32_t k = (uint32_t)(uint16_t)(bi.last - bi.no) + 1u;
            for (uint32_t j = 0; j < k; j++) {
                const uint32_t no = (uint16_t)(bi.no + j);
                t->lost_reliable += blk_reliable(no) ? 1u : 0u;
                CHECK(no == (uint16_t)t->next_no); /* in its place */
                t->next_no++;
            }
            t->lost_told += k;
            continue;
        }
        const uint32_t no = bi.no;
        uint8_t        want[900];
        blk_fill(want, no, blk_len(no));
        if (n == (int32_t)blk_len(no) && no == (uint16_t)t->next_no &&
            memcmp(buf, want, (size_t)n) == 0) {
            t->good++;
        } else {
            t->bad++;
        }
        t->next_no = no + 1u;
    }
}

/* Sends what fits whole, so send never waits for room (one thread). */
static void blk_feed(wlink_t *a, uint32_t *no, uint32_t upto)
{
    uint8_t p[900];
    while (*no < upto) {
        const uint32_t len = blk_len(*no);
        if (ring_space(&a->tx) < WL_REC + len) {
            return;
        }
        blk_fill(p, *no, len);
        const int32_t w = wl_send(a, p, len,
                                  blk_reliable(*no) ? AG_SPILINK_RELIABLE : 0u,
                                  0u);
        CHECK(w == (int32_t)len);
        (*no)++;
    }
}

static void blocks_through_a_bad_wire(void)
{
    static wlink_t a, b;
    end_open(&a, AG_SPILINK_BLOCKS);
    end_open(&b, AG_SPILINK_BLOCKS);
    const uint32_t count = 3000u;
    uint32_t       sent = 0;
    blk_tally_t    t;
    memset(&t, 0, sizeof(t));
    uint8_t  buf[900];
    uint32_t steps = 0;
    for (; steps < 400000u && t.good + t.lost_told < count; steps++) {
        tick();
        blk_feed(&a, &sent, count);
        one_frame(&a, &b, 5u, 2u);
        one_frame(&b, &a, 5u, 2u);
        blk_drain(&b, &t, buf, sizeof(buf));
    }
    printf("  wlink blocks: %u of %u whole, %u lost and told (%u of them "
           "reliable), %u bad, %u sent again\n",
           (unsigned)t.good, (unsigned)count, (unsigned)t.lost_told,
           (unsigned)t.lost_reliable, (unsigned)t.bad, (unsigned)a.rewinds);
    CHECK(t.good + t.lost_told == count);
    CHECK(t.bad == 0u);
    CHECK(t.lost_reliable == 0u); /* only what may be skipped is */
    CHECK(t.lost_told > 0u);      /* and some was: the wire lost frames */
    end_close(&a);
    end_close(&b);
}

/* ---------------------------------------------------------------------- */

static void a_restart_starts_the_stream_over(void)
{
    static wlink_t a, b;
    end_open(&a, AG_SPILINK_BLOCKS);
    end_open(&b, AG_SPILINK_BLOCKS);
    uint32_t    sent = 0;
    blk_tally_t t;
    memset(&t, 0, sizeof(t));
    uint8_t buf[900];
    for (uint32_t steps = 0; steps < 20000u && t.good < 200u; steps++) {
        tick();
        blk_feed(&a, &sent, 100000u);
        one_frame(&a, &b, 0u, 0u);
        one_frame(&b, &a, 0u, 0u);
        blk_drain(&b, &t, buf, sizeof(buf));
    }
    CHECK(t.good >= 200u);

    /* b starts over: a new session, and whatever it had of the old stream
     * is gone.  a has to notice and follow. */
    end_restart(&b, AG_SPILINK_BLOCKS);
    blk_tally_t after;
    memset(&after, 0, sizeof(after));
    uint32_t sent2 = 0;
    for (uint32_t steps = 0; steps < 20000u && after.good < 200u; steps++) {
        tick();
        if (a.peer_restarts > 0u) {
            blk_feed(&a, &sent2, 100000u); /* the new stream, block 0 on */
        }
        one_frame(&a, &b, 0u, 0u);
        one_frame(&b, &a, 0u, 0u);
        blk_drain(&b, &after, buf, sizeof(buf));
    }
    printf("  wlink restart: %u blocks before, the other end started over, "
           "%u after (%u restart seen)\n",
           (unsigned)t.good, (unsigned)after.good, (unsigned)a.peer_restarts);
    CHECK(a.peer_restarts == 1u);
    CHECK(after.good >= 200u);
    CHECK(after.bad == 0u);
    end_close(&a);
    end_close(&b);
}

/* ---------------------------------------------------------------------- */

/*
 * A block in two parts, as NODELINK and LINKSPI send one: a 6-byte head and
 * the caller's bytes, joined only on the wire, and taken apart again on the
 * way out - head first, the rest into the caller's buffer, cut where it
 * ends.  Blocks longer than a frame, over a wire that loses and damages.
 */
static void blocks_in_two_parts(void)
{
    static wlink_t a, b;
    end_open(&a, AG_SPILINK_BLOCKS);
    end_open(&b, AG_SPILINK_BLOCKS);
    const uint32_t count = 400u;
    uint32_t       sent = 0, good = 0, cut_right = 0, bad = 0;
    for (uint32_t steps = 0; steps < 400000u && good < count; steps++) {
        tick();
        /* in: head + body, whole blocks only (one thread: never wait) */
        while (sent < count) {
            /* longer than a frame, and no more: this reader takes a block
             * only once it is all in, so one near the receive ring's size
             * would wait for room the reader will not make (recv itself
             * takes a block as it comes) */
            const uint32_t len = 1u + (sent * 97u) % 800u;
            if (ring_space(&a.tx) < WL_REC + 6u + len) {
                break;
            }
            uint8_t head[6], body[1100];
            for (uint32_t i = 0; i < 6u; i++) {
                head[i] = (uint8_t)(0xA0u + sent + i);
            }
            for (uint32_t i = 0; i < len; i++) {
                body[i] = pat(sent * 1000u + i);
            }
            CHECK(wl_send2(&a, head, 6u, body, len, AG_SPILINK_RELIABLE, 0u) ==
                  (int32_t)(6u + len));
            sent++;
        }
        one_frame(&a, &b, 5u, 2u);
        one_frame(&b, &a, 5u, 2u);
        /* out: every whole block, into a head and a buffer that is short
         * for one block in three */
        for (;;) {
            if (b.rx.count < WL_REC) {
                break;
            }
            uint8_t h[WL_REC];
            (void)ring_peek(&b.rx, h, WL_REC);
            const uint32_t len = (uint32_t)h[0] | ((uint32_t)h[1] << 8);
            if (b.rx.count < WL_REC + len) {
                break;
            }
            const uint32_t no = good + bad;
            const uint32_t body_len = len - 6u;
            const uint32_t cap = (no % 3u == 0u) ? body_len / 2u : 1100u;
            uint8_t        head[6], buf[1100];
            ag_spilink_block_t bi;
            memset(&bi, 0, sizeof(bi));
            const int32_t n = wl_recv2(&b, head, 6u, buf, cap, &bi, 0u);
            const uint32_t want = 6u + ((body_len < cap) ? body_len : cap);
            bool ok = (n == (int32_t)want);
            for (uint32_t i = 0; ok && i < 6u; i++) {
                ok = (head[i] == (uint8_t)(0xA0u + no + i));
            }
            for (uint32_t i = 0; ok && i + 6u < want; i++) {
                ok = (buf[i] == pat(no * 1000u + i));
            }
            if (ok) {
                good++;
                cut_right += (want < len) ? 1u : 0u;
            } else {
                bad++;
            }
        }
    }
    printf("  wlink two parts: %u of %u blocks whole, %u of them cut at the "
           "buffer's end as asked, %u wrong\n",
           (unsigned)good, (unsigned)count, (unsigned)cut_right, (unsigned)bad);
    if (good != count) {
        uint8_t h[WL_REC] = {0};
        if (b.rx.count >= WL_REC) {
            (void)ring_peek(&b.rx, h, WL_REC);
        }
        printf("  stuck: sent %u, a.tx %u (sent_off %u), b.rx %u of %u, "
               "next block len %u, a allowance edge %u next %u\n",
               (unsigned)sent, (unsigned)a.tx.count, (unsigned)a.sent_off,
               (unsigned)b.rx.count, (unsigned)b.rx.cap,
               (unsigned)((uint32_t)h[0] | ((uint32_t)h[1] << 8)),
               (unsigned)(a.peer_ack + a.peer_room),
               (unsigned)(a.tx_base + a.sent_off));
    }
    CHECK(good == count);
    CHECK(bad == 0u);
    CHECK(cut_right > 0u);
    end_close(&a);
    end_close(&b);
}

/*
 * AG_SPILINK_IDLE: background goes into a wire only when nothing in its ring
 * is still waiting to go (27 Sep 2026), so what is written after it waits
 * for one block of it and not for a window.
 */
static void idle_waits_for_the_wire(void)
{
    static wlink_t a, b;
    end_open(&a, AG_SPILINK_BLOCKS);
    end_open(&b, AG_SPILINK_BLOCKS);
    const uint32_t bg = AG_SPILINK_RELIABLE | AG_SPILINK_WHOLE |
                        AG_SPILINK_YIELD | AG_SPILINK_IDLE;
    uint8_t body[600];
    for (uint32_t i = 0; i < sizeof(body); i++) {
        body[i] = pat(i);
    }
    /* the two ends know each other first: before that nothing goes */
    for (uint32_t k = 0; k < 20u; k++) {
        tick();
        one_frame(&a, &b, 0u, 0u);
        one_frame(&b, &a, 0u, 0u);
    }
    CHECK(wl_send2(&a, NULL, 0u, body, sizeof(body), bg, 0u) ==
          (int32_t)sizeof(body)); /* an idle wire takes it */
    CHECK(wl_send2(&a, NULL, 0u, body, sizeof(body), bg, 0u) == -AG_EAGAIN);
    /* anything else still goes in behind it */
    CHECK(wl_send2(&a, NULL, 0u, body, 100u,
                   AG_SPILINK_RELIABLE | AG_SPILINK_WHOLE, 0u) == 100);
    CHECK(wl_send2(&a, NULL, 0u, body, sizeof(body), bg, 0u) == -AG_EAGAIN);
    uint32_t steps = 0;
    while (a.tx.count > a.sent_off && steps++ < 1000u) {
        tick();
        one_frame(&a, &b, 0u, 0u);
        one_frame(&b, &a, 0u, 0u);
    }
    CHECK(a.tx.count == a.sent_off); /* all of it on the wire now */
    CHECK(wl_send2(&a, NULL, 0u, body, sizeof(body), bg, 0u) ==
          (int32_t)sizeof(body));
    printf("  wlink idle: refused while the ring had bytes waiting, taken once "
           "they had gone (%u steps)\n", (unsigned)steps);
    end_close(&a);
    end_close(&b);
}

/*
 * A block that may wait 10 ms goes ahead of blocks that may wait 50 queued
 * before it (2 Oct 2026): the sender's lanes feed the stream the soonest due
 * first, and the far end, which knows nothing of lanes, reads it in that
 * order, every block whole.
 */
static void budget_goes_first(void)
{
    static wlink_t a, b;
    end_open(&a, AG_SPILINK_BLOCKS);
    end_open(&b, AG_SPILINK_BLOCKS);
    static uint8_t lanes[2048u + 8192u + 2048u];
    const uint32_t caps[WL_LANES] = {2048u, 8192u, 2048u};
    wl_lanes(&a, lanes, caps);
    const uint32_t how = AG_SPILINK_RELIABLE | AG_SPILINK_WHOLE;
    /* two blocks and their heads to a frame exactly: none is cut, so the
     * reader below never waits inside one for the rest */
    uint8_t        body[PAY / 2u - WL_REC];
    for (uint32_t i = 0; i < 8u; i++) {
        memset(body, (int)('0' + i), sizeof(body));
        CHECK(wl_send2(&a, NULL, 0u, body, sizeof(body),
                       how | AG_SPILINK_BUDGET(50u), 0u) == (int32_t)sizeof(body));
    }
    memset(body, 'S', sizeof(body));
    CHECK(wl_send2(&a, NULL, 0u, body, sizeof(body),
                   how | AG_SPILINK_BUDGET(10u), 0u) == (int32_t)sizeof(body));
    char     order[16];
    uint32_t got = 0, whole = 0, steps = 0;
    uint8_t  buf[512];
    while (got < 9u && steps++ < 4000u) {
        tick();
        (void)wl_feed(&a);
        one_frame(&a, &b, 0u, 0u);
        one_frame(&b, &a, 0u, 0u);
        for (;;) {
            ag_spilink_block_t bi;
            memset(&bi, 0, sizeof(bi));
            const int32_t n = wl_recv2(&b, NULL, 0u, buf, sizeof(buf), &bi, 0u);
            if (n <= 0) {
                break;
            }
            bool same = n == (int32_t)sizeof(body);
            for (int32_t k = 1; k < n && same; k++) {
                same = buf[k] == buf[0];
            }
            whole += same ? 1u : 0u;
            if (got < sizeof(order)) {
                order[got] = (char)buf[0];
            }
            got++;
        }
    }
    CHECK(got == 9u);
    CHECK(whole == 9u);
    /* what fitted into the empty stream went at once (one block before the
     * frame size is agreed, two after); the 10 ms block is next, ahead of
     * every 50 ms block still waiting when it came */
    CHECK(got >= 3u && (order[1] == 'S' || order[2] == 'S'));
    order[(got < sizeof(order)) ? got : sizeof(order) - 1u] = '\0';
    printf("  wlink budget: read in the order %s (%u steps)\n", order,
           (unsigned)steps);
    end_close(&a);
    end_close(&b);
}

int main(void)
{
    budget_goes_first();
    bytes_through_a_bad_wire();
    blocks_through_a_bad_wire();
    a_restart_starts_the_stream_over();
    blocks_in_two_parts();
    idle_waits_for_the_wire();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
