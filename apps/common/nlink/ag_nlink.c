/*
 * ArgonOS - NLINK codec and scheduler.  See ag_nlink.h for the protocol and for
 * why the scheduler is the part that matters; this file is the byte layout and
 * the decision, and it is deliberately the only place that knows either.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ag_nlink.h"

#include <string.h>

/* Little-endian put/get, done by hand so the layout does not depend on the
 * host's struct packing or byte order. */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------------ */
/* CRC-32 (IEEE 802.3), reflected, table-driven                              */
/* ------------------------------------------------------------------------ */
/*
 * Built once on first use rather than stored: the table is a kilobyte, and a
 * kilobyte of constants in a driver's image is a kilobyte of a board's flash
 * and, on an .AXE, of its arena.  Building it costs 256 iterations, once.
 *
 * This is also where the cost of checking a payload lives - about five cycles a
 * byte, a tenth of a core at 5 MB/s.  ag_nlink.h says which frames pay it and
 * why the streaming ones do not.
 */
static uint32_t s_crc_table[256];
static bool     s_crc_ready;

static void crc_build(void)
{
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        s_crc_table[i] = c;
    }
    s_crc_ready = true;
}

uint32_t ag_nlink_crc32(uint32_t seed, const void *data, size_t len)
{
    if (!s_crc_ready) {
        crc_build();
    }
    const uint8_t *p = (const uint8_t *)data;
    uint32_t       c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c = s_crc_table[(c ^ p[i]) & 0xffu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------------ */
/* Codec                                                                     */
/* ------------------------------------------------------------------------ */

void ag_nlink_pack(uint8_t out[NL_HDR_SIZE], ag_nlink_hdr_t *h)
{
    h->magic = NL_MAGIC;
    h->ver = (uint8_t)NL_VERSION;

    put_u32(out + 0, NL_MAGIC);
    out[4] = h->ver;
    out[5] = h->cls;
    out[6] = h->flags;
    out[7] = h->chan;
    put_u16(out + 8, h->seq);
    put_u16(out + 10, h->ack);
    put_u16(out + 12, h->credit);
    put_u16(out + 14, h->len);

    h->crc = ag_nlink_crc32(0, out, 16u);
    put_u32(out + 16, h->crc);
}

bool ag_nlink_unpack(ag_nlink_hdr_t *h, const uint8_t in[NL_HDR_SIZE])
{
    if (get_u32(in + 0) != NL_MAGIC) {
        return false;
    }
    if (in[4] != (uint8_t)NL_VERSION) {
        return false;
    }
    if (in[5] >= (uint8_t)NL_CLASSES) {
        return false;
    }
    const uint16_t len = get_u16(in + 14);
    if (len > NL_QUANTUM) {
        return false;
    }
    /*
     * The header's own CRC, and it covers the header and nothing else - so it
     * can be checked here, before `len` is believed and used to read that many
     * bytes off a wire.  A single CRC over header and payload could not: it
     * would need the payload first, and how much payload there is is the very
     * field in question.
     */
    if (ag_nlink_crc32(0, in, 16u) != get_u32(in + 16)) {
        return false;
    }
    h->magic = NL_MAGIC;
    h->ver = in[4];
    h->cls = in[5];
    h->flags = in[6];
    h->chan = in[7];
    h->seq = get_u16(in + 8);
    h->ack = get_u16(in + 10);
    h->credit = get_u16(in + 12);
    h->len = len;
    h->crc = get_u32(in + 16);
    return true;
}

void ag_nlink_put_psum(uint8_t out[NL_PSUM_SIZE], const void *payload,
                       uint16_t len)
{
    const uint32_t crc =
        (len > 0u && payload != NULL) ? ag_nlink_crc32(0, payload, len) : 0u;
    put_u32(out, crc);
}

bool ag_nlink_check_psum(const ag_nlink_hdr_t *h, const void *payload,
                         const uint8_t in[NL_PSUM_SIZE])
{
    if ((h->flags & NL_F_PSUM) == 0u) {
        return true; /* nothing claimed, nothing to disagree with */
    }
    const uint32_t crc = (h->len > 0u && payload != NULL)
                             ? ag_nlink_crc32(0, payload, h->len)
                             : 0u;
    return crc == get_u32(in);
}

/* ------------------------------------------------------------------------ */
/* The scheduler                                                             */
/* ------------------------------------------------------------------------ */

void ag_nlink_sched_init(ag_nlink_sched_t *s, uint32_t reserve_bytes)
{
    memset(s, 0, sizeof(*s));
    s->reserve = reserve_bytes;
    s->stream_left = reserve_bytes;
}

void ag_nlink_sched_offer(ag_nlink_sched_t *s, int cls, uint32_t bytes,
                          uint32_t head_len)
{
    if (cls < 0 || cls >= (int)NL_CLASSES) {
        return;
    }
    s->waiting[cls] = bytes;
    s->next_len[cls] = head_len;
}

/* True when a stream frame may go out of this period's reservation. */
static bool stream_affordable(const ag_nlink_sched_t *s)
{
    if (s->waiting[NL_CLS_STREAM] == 0u) {
        return false;
    }
    if (s->reserve == 0u) {
        return true; /* uncapped: strictly ahead of bulk */
    }
    return s->stream_left >= s->next_len[NL_CLS_STREAM];
}

int ag_nlink_sched_pick(const ag_nlink_sched_t *s)
{
    /*
     * Control first, always.  It can do this safely only because it is bounded:
     * a control frame is a device call or a spawn, never a payload, so "always
     * first" is a promise about latency and not a licence to starve.
     */
    if (s->waiting[NL_CLS_CONTROL] > 0u) {
        return NL_CLS_CONTROL;
    }
    if (stream_affordable(s)) {
        return NL_CLS_STREAM;
    }
    if (s->waiting[NL_CLS_BULK] > 0u) {
        return NL_CLS_BULK;
    }
    /*
     * Stream has frames but has spent its reservation, and there is no bulk to
     * give the wire to.  Send it: a reservation is a floor under a class, not a
     * ceiling over it, and idling a wire to enforce a cap serves nobody.
     */
    if (s->waiting[NL_CLS_STREAM] > 0u) {
        return NL_CLS_STREAM;
    }
    return -1;
}

void ag_nlink_sched_sent(ag_nlink_sched_t *s, int cls, uint32_t len)
{
    if (cls < 0 || cls >= (int)NL_CLASSES) {
        return;
    }
    s->sent[cls]++;
    if (cls == NL_CLS_STREAM) {
        s->stream_left = (s->stream_left > len) ? (s->stream_left - len) : 0u;
    }
    s->waiting[cls] = (s->waiting[cls] > len) ? (s->waiting[cls] - len) : 0u;
    if (s->waiting[cls] == 0u) {
        s->next_len[cls] = 0u;
    }
}

void ag_nlink_sched_tick(ag_nlink_sched_t *s)
{
    /*
     * A period ending with stream still queued and its reservation spent is the
     * one fault this scheduler can have that is nobody's bug: the reservation
     * is simply too small for the stream it was sized for.  Counted rather than
     * logged, because it happens at audio rates and a log line at audio rates
     * is the next starvation.
     */
    if (s->reserve > 0u && s->stream_left == 0u &&
        s->waiting[NL_CLS_STREAM] > 0u) {
        s->stream_starved++;
    }
    s->stream_left = s->reserve;
}

/* ------------------------------------------------------------------------ */
/* Reliability: the window                                                   */
/* ------------------------------------------------------------------------ */

bool ag_nlink_seq_after(uint16_t a, uint16_t b)
{
    /*
     * Signed difference, which is how this is done and how it has been got
     * wrong in every generation of protocol that used a wrapping counter.
     * a > b is wrong: 0 is after 65535.  a - b > 0 is wrong for the same
     * reason in unsigned arithmetic.  The cast is the whole trick, and it is
     * correct for any two sequences less than 32768 apart - which a window of
     * a few dozen frames comfortably is.
     */
    return (int16_t)(a - b) > 0;
}

void ag_nlink_tx_init(ag_nlink_tx_t *tx, uint8_t cls, ag_nlink_slot_t *slots,
                      uint16_t cap, uint32_t rto_ms)
{
    memset(tx, 0, sizeof(*tx));
    tx->slot = slots;
    tx->cap = cap;
    tx->cls = cls;
    tx->rto_ms = rto_ms;
}

bool ag_nlink_tx_room(const ag_nlink_tx_t *tx)
{
    return tx->count < tx->cap;
}

bool ag_nlink_tx_queue(ag_nlink_tx_t *tx, uint8_t chan, const void *data,
                       uint16_t len, bool eom)
{
    if (len > NL_QUANTUM || tx->count >= tx->cap) {
        return false;
    }
    ag_nlink_slot_t *s = &tx->slot[tx->count];
    s->seq = tx->next_seq++;
    s->len = len;
    s->chan = chan;
    s->flags = (uint8_t)(NL_F_PSUM | (eom ? NL_F_EOM : 0u));
    s->sent = false;
    s->sent_ms = 0;
    /*
     * Every field, every time.  The slots are the caller's array and this is
     * the only place that owns one, so a field left over from the frame that
     * used this slot before - or from whatever was on the stack when the array
     * was declared - is read as if it belonged to this frame.  `tries` was
     * exactly that: uninitialised, it counted first transmissions as
     * retransmissions and the counter lied about a link that was working.
     */
    s->tries = 0;
    if (len > 0u && data != NULL) {
        memcpy(s->data, data, len);
    }
    tx->count++;
    return true;
}

static uint32_t clamp_rto(uint32_t ms)
{
    if (ms < NL_RTO_MIN_MS) {
        return NL_RTO_MIN_MS;
    }
    if (ms > NL_RTO_MAX_MS) {
        return NL_RTO_MAX_MS;
    }
    return ms;
}

const ag_nlink_slot_t *ag_nlink_tx_next(ag_nlink_tx_t *tx, uint32_t now_ms)
{
    /*
     * The oldest unacknowledged frame has timed out: go back to it, and mark
     * the whole window behind it unsent so it goes again in order.  That is
     * what the N in go-back-N is, and it is not pessimism.
     *
     * The alternative - re-send only the oldest and let the others time out on
     * their own - looks gentler on a busy wire and is far worse: a receiver
     * that discarded everything behind a gap has nothing to acknowledge, so
     * each frame behind the loss costs its own full timeout, and a window of
     * twenty frames takes twenty timeouts to recover from one lost frame.
     */
    if (tx->count > 0u && tx->slot[0].sent &&
        (uint32_t)(now_ms - tx->slot[0].sent_ms) >= tx->rto_ms) {
        for (uint16_t i = 0; i < tx->count; i++) {
            tx->slot[i].sent = false;
        }
        /*
         * Back off, and this half is not optional.  Karn's rule forbids taking
         * a sample from a retransmitted frame - so without a backoff a timeout
         * that is too short can never be corrected by anything, and the link
         * retransmits at that rate for as long as it is up.  Doubling on each
         * timeout is what makes the estimator able to climb as well as fall; a
         * good sample from a frame that goes out once puts it back.
         */
        tx->rto_ms = clamp_rto(tx->rto_ms * 2u);
    }
    for (uint16_t i = 0; i < tx->count; i++) {
        if (!tx->slot[i].sent) {
            tx->slot[i].sent = true;
            tx->slot[i].sent_ms = now_ms;
            tx->sent++;
            if (tx->slot[i].tries > 0u) {
                tx->retransmits++;
            }
            tx->slot[i].tries++;
            return &tx->slot[i];
        }
    }
    return NULL;
}

/*
 * One round-trip sample, folded in the way Jacobson and Karels settled on:
 * a smoothed mean plus four times a smoothed deviation.
 *
 * The deviation term is the part that matters and the part usually left out.
 * A timeout of "the average round trip" retransmits half of all frames by
 * construction; what is needed is a number the round trip rarely exceeds, and
 * that is the mean plus several times how much it moves about.
 *
 * Integer arithmetic throughout, in milliseconds: this runs on a board with no
 * hardware floating point, in the path of every acknowledgement.
 */
static void rtt_sample(ag_nlink_tx_t *tx, uint32_t r_ms)
{
    if (tx->srtt_ms == 0u) {
        tx->srtt_ms = r_ms;
        tx->rttvar_ms = r_ms / 2u;
    } else {
        const uint32_t diff = (r_ms > tx->srtt_ms) ? (r_ms - tx->srtt_ms)
                                                   : (tx->srtt_ms - r_ms);
        /* rttvar = 3/4 rttvar + 1/4 |srtt - r| */
        tx->rttvar_ms = (tx->rttvar_ms * 3u + diff) / 4u;
        /* srtt = 7/8 srtt + 1/8 r */
        tx->srtt_ms = (tx->srtt_ms * 7u + r_ms) / 8u;
    }
    tx->rto_ms = clamp_rto(tx->srtt_ms + 4u * tx->rttvar_ms);
}

void ag_nlink_tx_ack(ag_nlink_tx_t *tx, uint16_t ack, uint32_t now_ms)
{
    if (ack == NL_ACK_NONE) {
        return;
    }
    /* Cumulative: everything up to and including `ack` is gone from the window.
     * A frame is still ours only when its sequence is AFTER the ack. */
    uint16_t drop = 0;
    while (drop < tx->count && !ag_nlink_seq_after(tx->slot[drop].seq, ack)) {
        drop++;
    }
    if (drop == 0u) {
        return;
    }
    /*
     * Measure, before the slots go.  Karn's rule: only a frame that went out
     * ONCE can be measured, because the ack for a frame sent twice does not say
     * which copy it answers - and taking it as a sample drags the estimate
     * towards whichever guess was wrong, which is how an adaptive timer talks
     * itself into either a storm or a stall.
     *
     * The newest such frame, not the oldest: a cumulative ack for several
     * frames arrives when the LAST of them got there, so the oldest would be
     * measured as having taken the whole burst's time.
     */
    for (uint16_t i = drop; i-- > 0;) {
        if (tx->slot[i].tries == 1u) {
            rtt_sample(tx, (uint32_t)(now_ms - tx->slot[i].sent_ms));
            break;
        }
    }
    /*
     * Slide.  A ring would avoid the move; an array that is moved costs one
     * memmove of at most a window, and it keeps "oldest first" true by
     * construction rather than by two indices that have to be reasoned about
     * every time they are read.  At a few dozen slots that is the better trade.
     */
    tx->count = (uint16_t)(tx->count - drop);
    if (tx->count > 0u) {
        memmove(&tx->slot[0], &tx->slot[drop],
                (size_t)tx->count * sizeof(tx->slot[0]));
    }
}

uint16_t ag_nlink_tx_pending(const ag_nlink_tx_t *tx) { return tx->count; }

/* True when the oldest unacknowledged frame is overdue and the whole window
 * behind it is about to go again. */
static bool rto_expired(const ag_nlink_tx_t *tx, uint32_t now_ms)
{
    return tx->count > 0u && tx->slot[0].sent &&
           (uint32_t)(now_ms - tx->slot[0].sent_ms) >= tx->rto_ms;
}

uint32_t ag_nlink_tx_waiting_bytes(const ag_nlink_tx_t *tx, uint32_t now_ms)
{
    const bool overdue = rto_expired(tx, now_ms);
    uint32_t   n = 0;
    for (uint16_t i = 0; i < tx->count; i++) {
        if (overdue || !tx->slot[i].sent) {
            n += tx->slot[i].len + NL_HDR_SIZE + NL_PSUM_SIZE;
        }
    }
    return n;
}

uint32_t ag_nlink_tx_head_len(const ag_nlink_tx_t *tx, uint32_t now_ms)
{
    if (rto_expired(tx, now_ms)) {
        return (uint32_t)tx->slot[0].len + NL_HDR_SIZE + NL_PSUM_SIZE;
    }
    for (uint16_t i = 0; i < tx->count; i++) {
        if (!tx->slot[i].sent) {
            return (uint32_t)tx->slot[i].len + NL_HDR_SIZE + NL_PSUM_SIZE;
        }
    }
    return 0u;
}

void ag_nlink_rx_init(ag_nlink_rx_t *rx)
{
    memset(rx, 0, sizeof(*rx));
    rx->last = NL_ACK_NONE;
}

bool ag_nlink_rx_accept(ag_nlink_rx_t *rx, const ag_nlink_hdr_t *h)
{
    /*
     * The first frame of a stream is sequence zero, and nothing else will do.
     *
     * The first draft accepted whatever arrived first, reasoning that a link
     * which insisted on zero could never be resynchronised.  That was wrong in
     * the way that matters: lose frame zero, and the receiver silently adopts
     * frame one as the beginning, acknowledges it, and the sender slides past a
     * quantum of data that nobody ever received.  A transfer that is short by
     * 256 bytes and reports success.  The lossy-wire test below found it.
     *
     * Resynchronisation is a thing the link does on purpose, by resetting both
     * ends (NL_F_RST), and not something a receiver improvises out of the first
     * bytes it happens to see.
     */
    const uint16_t want = rx->any ? (uint16_t)(rx->last + 1u) : 0u;
    if (h->seq == want) {
        rx->any = true;
        rx->last = h->seq;
        rx->taken++;
        return true;
    }
    if (rx->any && !ag_nlink_seq_after(h->seq, rx->last)) {
        /* Already had it.  This means the ACKNOWLEDGEMENT was lost, not the
         * frame - the peer sent it again because nothing came back.  Counted
         * separately from a gap because the two say different things about
         * which direction of the wire is bad. */
        rx->dups++;
        return false;
    }
    /* Something ahead of it was lost.  Before anything has been accepted at
     * all, that is any frame but zero. */
    rx->gaps++;
    return false;
}

uint16_t ag_nlink_rx_ack(const ag_nlink_rx_t *rx)
{
    return rx->any ? rx->last : NL_ACK_NONE;
}

uint32_t ag_nlink_sched_worst_wait(const ag_nlink_sched_t *s, int cls)
{
    switch (cls) {
    case NL_CLS_CONTROL:
        /* One frame may already be going out and cannot be interrupted; then
         * whatever control is queued ahead. */
        return NL_QUANTUM + s->waiting[NL_CLS_CONTROL];
    case NL_CLS_STREAM:
        /* Same, plus the control queue, plus the stream queue.  Bulk does not
         * appear: only one bulk frame can be in flight and it is the quantum
         * already counted. */
        return NL_QUANTUM + s->waiting[NL_CLS_CONTROL] +
               s->waiting[NL_CLS_STREAM];
    case NL_CLS_BULK:
    default:
        /*
         * Unbounded, and that is the design rather than a gap in it.  Bulk is
         * the class that pays for the other two being bounded; a link where
         * every class had a bound would be a link with no room to absorb
         * anything.
         */
        return UINT32_MAX;
    }
}
