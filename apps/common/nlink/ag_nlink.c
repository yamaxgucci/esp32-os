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
