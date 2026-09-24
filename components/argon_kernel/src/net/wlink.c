/*
 * ArgonOS - the framed link's protocol, whatever the wire.  See wlink.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "net/wlink.h"

#include <argon/port/random.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

static void lock(wlink_t *s) { (void)ag_port_mutex_take(s->lock, AG_PORT_FOREVER); }

static void unlock(wlink_t *s) { (void)ag_port_mutex_give(s->lock); }

static void count(wlink_t *s, uint32_t *field, uint32_t by)
{
    lock(s);
    *field += by;
    unlock(s);
}

/* Ticks for a wait, never zero: a wait that rounds to no ticks is a poll. */
static ag_port_ticks_t ticks_at_least(uint32_t ms)
{
    const ag_port_ticks_t ticks = ag_port_ms_to_ticks(ms);
    return (ticks == 0u) ? 1u : ticks;
}

/* A reader made room: a sending side that waits to be told is told. */
static void room_made(wlink_t *s)
{
    if (s->kick_reads) {
        (void)ag_port_sem_give(s->tx_sem);
    }
}

void wl_reset(wlink_t *s, uint32_t block, uint32_t flags)
{
    s->block = block;
    s->frame = s->framed ? WL_HELLO : block;
    s->agreed = !s->framed;
    s->peer_known = false;
    s->peer_room = 0;
    s->peer_ack = 0;
    s->tx_base = 0;
    s->sent_off = 0;
    s->infl_n = 0;
    s->resend = false;
    s->resend_whole = false;
    s->resend_off = 0;
    s->ack_at = ag_port_us();
    memset(&s->undo, 0, sizeof(s->undo));
    s->blocks = (flags & AG_SPILINK_BLOCKS) != 0u;
    s->blk_next = 0;
    s->next_block = 0;
    s->rx_next = 0;
    s->ooo_n = 0;
    s->refused = false;
    s->refused_off = 0;
    s->nak_pending = false;
    s->nak_sent = false;
    s->nak_expect = 0;
    s->nak_at = 0;
    s->drop_every = (flags >> 8) & 0xFFu;
    s->drop_count = 0;
    s->last_rx_len = 0;
    ag_port_random(&s->sess, sizeof(s->sess));
    s->sess |= 1u; /* never 0 */
    s->peer_sess = 0;
    s->peer_sess_known = false;
    s->epoch = 0;
    s->rewinds = 0;
    s->naks = 0;
    s->peer_restarts = 0;
    s->starve_data = 0;
    s->starve_credit = 0;
    s->frames_bad = 0;
    s->seq_gaps = 0;
    s->bytes_dropped = 0;
}

uint32_t wl_sum(const uint8_t *f, uint32_t len)
{
    /* two running sums over words: cheap enough for 4 KB a frame on a C3,
     * and a flipped bit, a dropped byte or a swapped word all change it */
    uint32_t a = 0x6A09E667u;
    uint32_t b = 0;
    const uint32_t *w = (const uint32_t *)f;
    for (unsigned k = 0; k < 5u; k++) {
        a += w[k];
        b += a;
    }
    const uint8_t  *p = f + WL_HDR;
    const uint32_t *pw = (const uint32_t *)p;
    const uint32_t  words = len / 4u;
    for (uint32_t k = 0; k < words; k++) {
        a += pw[k];
        b += a;
    }
    for (uint32_t k = words * 4u; k < len; k++) {
        a += p[k];
        b += a;
    }
    return a ^ ((b << 13) | (b >> 19));
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static uint32_t get16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return get16(p) | (get16(p + 2) << 16);
}

/* Bytes the peer may be sent now: its room, less what is already on the way. */
uint32_t wl_allowance(const wlink_t *s, uint32_t in_flight)
{
    if (!s->agreed || !s->peer_known || s->peer_room <= in_flight) {
        return 0u;
    }
    return s->peer_room - in_flight;
}

/* Copy out without removing, starting `off` bytes past the head. */
static uint32_t ring_peek_at(const ring_t *r, uint32_t off, uint8_t *dst,
                             uint32_t want)
{
    if (off >= r->count) {
        return 0u;
    }
    const uint32_t avail = r->count - off;
    const uint32_t n = (want < avail) ? want : avail;
    const uint32_t start = (r->head + off) % r->cap;
    const uint32_t first = r->cap - start;
    const uint32_t a0 = (n < first) ? n : first;
    memcpy(dst, r->mem + start, a0);
    if (n > a0) {
        memcpy(dst + a0, r->mem, n - a0);
    }
    return n;
}

/* Write `n` bytes `delta` past the tail, not yet counted as in the ring;
 * src NULL writes zeros.  The caller has checked delta + n fits. */
static void ring_write_at(ring_t *r, uint32_t delta, const uint8_t *src,
                          uint32_t n)
{
    const uint32_t start = (r->tail + delta) % r->cap;
    const uint32_t first = r->cap - start;
    const uint32_t a0 = (n < first) ? n : first;
    if (src != NULL) {
        memcpy(r->mem + start, src, a0);
        if (n > a0) {
            memcpy(r->mem, src + a0, n - a0);
        }
    } else {
        memset(r->mem + start, 0, a0);
        if (n > a0) {
            memset(r->mem, 0, n - a0);
        }
    }
}

static void ring_advance(ring_t *r, uint32_t n)
{
    r->tail = (r->tail + n) % r->cap;
    r->count += n;
}

/* Receive ring bytes beyond the tail already taken by frames after a hole. */
static uint32_t ooo_reserved(const wlink_t *s)
{
    if (s->ooo_n == 0u) {
        return 0u;
    }
    return s->ooo[s->ooo_n - 1u].off + s->ooo[s->ooo_n - 1u].n - s->rx_next;
}

/*
 * A lost frame of blocks, as segments: stream bytes [off, off + n), whose
 * first record header is at `blk`, into `p`.  Returns the payload length,
 * or 0 if no block in it may be skipped or the segments would not fit
 * `pay` - the frame then goes again as it was.
 */
static uint32_t seg_build(const wlink_t *s, uint32_t off, uint32_t n,
                          uint32_t blk, uint8_t *p, uint32_t pay,
                          uint32_t *data)
{
    const uint32_t end = off + n;
    uint32_t       used = 2;
    uint32_t       open = 0; /* the data segment being added to, or 0 */
    uint32_t       pos = off;
    uint32_t       b = blk;
    bool           skipped = false;
    *data = 0;
    while ((int32_t)(pos - end) < 0) {
        uint32_t upto;
        if ((int32_t)(pos - b) < 0) {
            upto = ((int32_t)(b - end) < 0) ? b : end;
        } else {
            uint8_t h[WL_REC];
            (void)ring_peek_at(&s->tx, b - s->tx_base, h, WL_REC);
            uint32_t bend = b + WL_REC + get16(h);
            if (!(h[4] & AG_SPILINK_RELIABLE) && (int32_t)(bend - end) <= 0) {
                /* a run of whole unreliable blocks: one notice */
                const uint32_t first = get16(h + 2);
                uint32_t       last = first;
                while ((int32_t)(bend - end) < 0) {
                    (void)ring_peek_at(&s->tx, bend - s->tx_base, h,
                                       WL_REC);
                    const uint32_t nb = bend + WL_REC + get16(h);
                    if ((h[4] & AG_SPILINK_RELIABLE) ||
                        (int32_t)(nb - end) > 0) {
                        break;
                    }
                    last = get16(h + 2);
                    bend = nb;
                }
                if (used + 6u > pay) {
                    return 0u;
                }
                put16(p + used, WL_SEG_SKIP | (bend - pos));
                put16(p + used + 2, first);
                put16(p + used + 4, last);
                used += 6u;
                open = 0;
                skipped = true;
                pos = b = bend;
                continue;
            }
            upto = ((int32_t)(bend - end) < 0) ? bend : end;
            b = bend;
        }
        /* bytes [pos, upto) as they are */
        const uint32_t len = upto - pos;
        if (open == 0u) {
            if (used + 2u + len > pay) {
                return 0u;
            }
            open = used;
            put16(p + used, 0);
            used += 2u;
        } else if (used + len > pay) {
            return 0u;
        }
        (void)ring_peek_at(&s->tx, pos - s->tx_base, p + used, len);
        put16(p + open, get16(p + open) + len);
        used += len;
        *data += len;
        pos = upto;
    }
    if (!skipped) {
        return 0u;
    }
    put16(p, used - 2u);
    return used;
}

/*
 * Fill a frame.  A frame to send again goes first - the one frame the other
 * side has a hole at, in segments when it held blocks not worth sending
 * again - and otherwise as much NOT YET SENT data as `allow` lets.  Nothing
 * is freed here; the acknowledgement does that.  Returns the data bytes in
 * it.
 */
uint32_t wl_build(wlink_t *s, uint8_t *f, uint32_t allow,
                  uint32_t room_less, bool *nak_was, uint32_t *wire_len)
{
    const uint32_t pay = s->frame - WL_HDR;
    uint32_t       flags = 0;
    uint32_t       off = 0;
    uint32_t       n = 0;     /* data bytes */
    uint32_t       span = 0;  /* stream bytes this frame stands for */
    uint32_t       body = 0;  /* payload bytes after the header */
    s->undo.kind = 0;

    lock(s);
    if (s->resend) {
        s->resend = false;
        for (uint32_t k = 0; k < s->infl_n; k++) {
            if (s->infl[k].off != s->resend_off) {
                continue;
            }
            off = s->infl[k].off;
            span = s->infl[k].n;
            /* blocks are skipped only for a frame the wire lost, never for
             * one the receiver threw away for want of room: that is not a
             * loss the application was willing to take */
            body = (s->blocks && !s->resend_whole)
                       ? seg_build(s, off, span, s->infl[k].blk,
                                   f + WL_HDR, pay, &n)
                       : 0u;
            if (body > 0u) {
                flags = WL_F_SEG;
            } else {
                flags = WL_F_DATA;
                n = ring_peek_at(&s->tx, off - s->tx_base, f + WL_HDR,
                                 span);
                body = n;
            }
            s->undo.kind = 2;
            s->undo.resend_off = off;
            break;
        }
    }
    if (flags == 0u && s->infl_n < WL_WIN) {
        const uint32_t start = s->tx_base + s->sent_off;
        const uint32_t avail =
            (s->tx.count > s->sent_off) ? s->tx.count - s->sent_off : 0u;
        uint32_t limit = (avail < allow) ? avail : allow;
        if (limit > pay) {
            limit = pay;
        }
        const uint32_t blk = s->blk_next;
        if (s->blocks && limit > 0u) {
            /*
             * Blocks of both kinds share a frame, and any block may be cut
             * by its edges: cutting frames at every change of kind cost a
             * quarter of the wire with one block in eight unreliable, and so
             * did refusing to cut an unreliable block (74%, measured).  What
             * is not sent again is sorted out only if the frame is lost.
             * Here the record headers are only walked past, so the next
             * frame knows where its first one is.
             */
            const uint32_t end = start + limit;
            uint32_t       b = s->blk_next;
            while ((int32_t)(b - end) < 0) {
                uint8_t h[WL_REC];
                (void)ring_peek_at(&s->tx, b - s->tx_base, h, WL_REC);
                b += WL_REC + get16(h);
            }
            s->undo.blk_next = s->blk_next;
            s->blk_next = b;
        }
        if (limit > 0u) {
            n = ring_peek_at(&s->tx, s->sent_off, f + WL_HDR, limit);
            if (s->infl_n == 0u) {
                /* the retransmit clock starts with the first frame out, not
                 * at the last ack: after a quiet spell that was long ago, and
                 * every answer of an exchange went twice (127 of 794) */
                s->ack_at = ag_port_us();
            }
            s->infl[s->infl_n].off = start;
            s->infl[s->infl_n].blk = blk;
            s->infl[s->infl_n].n = (uint16_t)n;
            s->infl_n++;
            s->sent_off += n;
            flags = WL_F_DATA;
            off = start;
            span = n;
            body = n;
            s->undo.kind = 1;
            s->undo.n = n;
        }
    }
    /*
     * One frame's payload counted as room beyond what is free: the reader
     * takes what arrived before the room is looked at again - and in block
     * mode the start of a block cut across two frames cannot be taken until
     * the second arrives, which strict counting charged to every frame (74%
     * of the wire, measured).  A frame that does not fit is not
     * acknowledged and comes again, so this can cost a resend, never data.
     *
     * Not while a hole is open: the reader then takes nothing, and the
     * piece of a block before the hole stays in the ring until it is
     * filled, so the frame promised beyond the room did not fit and was
     * thrown away - a new hole behind every hole (block mode under 5%
     * loss: 762 KB thrown away in 5 s, 24% of the wire, measured).
     */
    const bool     hole = s->ooo_n > 0u || s->nak_sent || s->refused;
    uint32_t       space = ring_space(&s->rx) + (hole ? 0u : pay);
    const uint32_t held = ooo_reserved(s);
    space = (space > held) ? space - held : 0u;
    if (space > s->rxcap) {
        space = s->rxcap;
    }
    unlock(s);

    if (flags == 0u) {
        s->starve_data += (s->tx.count > s->sent_off) ? 0u : 1u;
    }
    *nak_was = s->nak_pending;
    if (s->nak_pending) {
        flags |= WL_F_NAK;
        if (s->refused && s->refused_off == s->rx_next) {
            flags |= WL_F_REFUSED;
        }
        s->nak_pending = false;
        s->naks++;
    }
    uint32_t room = (space > room_less) ? space - room_less : 0u;
    if (room > 0xFFFFu) {
        room = 0xFFFFu;
    }
    f[0] = 0xA5u;
    f[1] = (uint8_t)(flags | (s->master ? WL_F_MASTER : 0u));
    f[2] = 0x5Au;
    f[3] = (uint8_t)(s->block / WL_UNIT);
    put32(f + 4, off);
    put32(f + 8, s->rx_next);
    put16(f + 12, span);
    put16(f + 14, room);
    put32(f + 16, s->sess);
    put32(f + 20, wl_sum(f, body));
    if (wire_len != NULL) {
        *wire_len = WL_HDR + body;
    }
    return n;
}

/* Take back what frame_build did, for a frame that never went out. */
void wl_unbuild(wlink_t *s, uint32_t n, bool nak_was)
{
    (void)n;
    if (s->undo.kind == 1u && s->infl_n > 0u) {
        s->infl_n--;
        s->sent_off -= s->undo.n;
        if (s->blocks) {
            s->blk_next = s->undo.blk_next;
        }
    } else if (s->undo.kind == 2u) {
        s->resend = true;
        s->resend_off = s->undo.resend_off;
    }
    s->undo.kind = 0;
    if (nak_was) {
        s->nak_pending = true;
        s->naks--;
    }
}

static void ask_again(wlink_t *s, int64_t now)
{
    if (!s->nak_sent || s->nak_expect != s->rx_next ||
        now - s->nak_at > WL_NAK_US) {
        s->nak_pending = true;
        s->nak_sent = true;
        s->nak_expect = s->rx_next;
        s->nak_at = now;
        s->seq_gaps++;
    }
}

/*
 * The other end has started over: both streams begin again at zero, and what
 * this end still held of the old ones is gone - the other end has none of
 * it.  The frame size is agreed again, as the other end is saying hello.
 */
static void peer_restarted(wlink_t *s, uint32_t sess)
{
    lock(s);
    s->peer_sess = sess;
    s->epoch++;
    s->peer_restarts++;
    s->tx.head = s->tx.tail = s->tx.count = 0;
    s->tx_base = 0;
    s->sent_off = 0;
    s->infl_n = 0;
    s->resend = false;
    s->resend_whole = false;
    s->blk_next = 0;
    s->next_block = 0;
    s->rx.head = s->rx.tail = s->rx.count = 0;
    s->rx_next = 0;
    s->ooo_n = 0;
    s->refused = false;
    s->nak_pending = false;
    s->nak_sent = false;
    s->agreed = false;
    s->peer_known = false;
    unlock(s);
    /* a writer waiting for room and a reader waiting for bytes both look at
     * the epoch when they wake */
    (void)ag_port_sem_give(s->space_sem);
    (void)ag_port_sem_give(s->rx_sem);
}

/* A segmented payload that stands for exactly `span` stream bytes. */
static bool seg_check(const uint8_t *p, uint32_t body, uint32_t span)
{
    uint32_t at = 2u;
    uint32_t total = 0;
    while (at < body) {
        if (at + 2u > body) {
            return false;
        }
        const uint32_t head = get16(p + at);
        const uint32_t len = head & ~WL_SEG_SKIP;
        const uint32_t size = (head & WL_SEG_SKIP) ? 4u : len;
        if (((head & WL_SEG_SKIP) && len <= WL_REC) || len == 0u ||
            at + 2u + size > body) {
            return false;
        }
        total += len;
        at += 2u + size;
    }
    return at == body && total == span;
}

/*
 * One received frame: its ack applied to what this end sent, a hole report
 * honoured, and its bytes put where they belong.  Returns the bytes that
 * reached the reader.
 */
uint32_t wl_rx(wlink_t *s, const uint8_t *f, uint32_t got)
{
    const uint32_t pay = s->block - WL_HDR;
    if (got < WL_HDR || f[0] != 0xA5u || f[2] != 0x5Au) {
        count(s, &s->frames_bad, 1u);
        return 0u;
    }
    const uint32_t flags = f[1];
    /*
     * A frame from this end's own side is an ECHO: a slave not yet armed
     * sends back what it last took in, and the master's 64-byte hello is
     * exactly its hardware buffer.  Taken for the slave's, the master
     * agreed on its own frame size (4096 against the C3's 2048, and half
     * the wire, measured) - and an echo of data would be data.
     */
    if (s->role_check && ((flags & WL_F_MASTER) != 0u) == s->master) {
        count(s, &s->frames_bad, 1u);
        return 0u;
    }
    /* this end's own session: its own frame, come back (a wire with no
     * roles has only this to tell an echo by) */
    if (get32(f + 16) == s->sess) {
        count(s, &s->frames_bad, 1u);
        return 0u;
    }
    const uint32_t off = get32(f + 4);
    const uint32_t ack = get32(f + 8);
    const uint32_t span = get16(f + 12);
    const uint32_t room = get16(f + 14);
    const uint32_t body =
        (flags & WL_F_SEG)
            ? ((got >= WL_HDR + 2u) ? 2u + get16(f + WL_HDR) : ~0u)
        : (flags & WL_F_DATA) ? span
                                   : 0u;
    const int64_t  now = ag_port_us();
    if (body > pay || body > got - WL_HDR || span > pay ||
        wl_sum(f, body) != get32(f + 20)) {
        count(s, &s->frames_bad, 1u);
        ask_again(s, now);
        return 0u;
    }
    if ((flags & WL_F_SEG) && !seg_check(f + WL_HDR, body, span)) {
        count(s, &s->frames_bad, 1u);
        ask_again(s, now);
        return 0u;
    }
    if (!s->peer_sess_known) {
        s->peer_sess = get32(f + 16);
        s->peer_sess_known = true;
    } else if (get32(f + 16) != s->peer_sess) {
        peer_restarted(s, get32(f + 16));
    }
    if (!s->agreed) {
        /* a peer that says nothing (0) is taken to be this end's size */
        const uint32_t peer = (f[3] != 0u) ? f[3] * WL_UNIT : s->block;
        s->frame = (peer < s->block) ? peer : s->block;
        s->agreed = !s->agree_by_len || got >= s->frame;
    }
    const bool carries = (flags & (WL_F_DATA | WL_F_SEG)) != 0u;
    if (s->drop_every != 0u && carries &&
        (++s->drop_count % s->drop_every) == 0u) {
        return 0u; /* test: this frame never arrived */
    }

    /* the ack: free what it passes */
    lock(s);
    s->peer_room = room;
    s->peer_ack = ack;
    s->peer_known = true;
    bool freed = false;
    const uint32_t d = ack - s->tx_base;
    if ((int32_t)d > 0 && d <= s->sent_off) {
        ring_commit(&s->tx, d);
        s->sent_off -= d;
        s->tx_base = ack;
        while (s->infl_n > 0u &&
               (int32_t)(s->infl[0].off + s->infl[0].n - ack) <= 0) {
            for (uint32_t k = 1; k < s->infl_n; k++) {
                s->infl[k - 1] = s->infl[k];
            }
            s->infl_n--;
        }
        freed = true;
    }
    unlock(s);
    if (freed) {
        s->ack_at = now;
        (void)ag_port_sem_give(s->space_sem);
    }
    if ((flags & WL_F_NAK) && s->infl_n > 0u &&
        (int32_t)(ack - s->tx_base) == 0) {
        s->resend = true;
        s->resend_whole = (flags & WL_F_REFUSED) != 0u;
        s->resend_off = s->tx_base;
        s->rewinds++;
    }
    if (!carries || span == 0u) {
        return 0u;
    }

    /* the bytes: a repeat, the next in order, or after a hole */
    if ((int32_t)(off + span - s->rx_next) <= 0 ||
        (int32_t)(off - s->rx_next) < 0) {
        return 0u;
    }
    const uint32_t delta = off - s->rx_next;
    for (uint32_t k = 0; k < s->ooo_n; k++) {
        if (s->ooo[k].off == off) {
            return 0u; /* already have it */
        }
    }
    lock(s);
    const bool fits = delta + span <= ring_space(&s->rx) &&
                      (delta == 0u || s->ooo_n < WL_WIN);
    unlock(s);
    if (!fits) {
        /* not taken, so not acknowledged: it comes again */
        s->bytes_dropped += span;
        if (!s->refused || (int32_t)(off - s->refused_off) < 0) {
            s->refused = true;
            s->refused_off = off;
        }
        ask_again(s, now);
        return 0u;
    }
    lock(s);
    if (flags & WL_F_DATA) {
        ring_write_at(&s->rx, delta, f + WL_HDR, span);
    } else {
        /* segments: bytes where they go, and for each notice a LOST record
         * of the same size; the reader passes over the rest unread */
        const uint8_t *p = f + WL_HDR + 2u;
        const uint8_t *const pe = f + WL_HDR + body;
        uint32_t       at = delta;
        while (p < pe) {
            const uint32_t head = get16(p);
            const uint32_t len = head & ~WL_SEG_SKIP;
            if (head & WL_SEG_SKIP) {
                uint8_t rec[WL_REC];
                put16(rec, len - WL_REC);
                put16(rec + 2, get16(p + 2));
                rec[4] = WL_REC_LOST;
                rec[5] = 0;
                put16(rec + 6, get16(p + 4));
                ring_write_at(&s->rx, at, rec, WL_REC);
                p += 6;
            } else {
                ring_write_at(&s->rx, at, p + 2, len);
                p += 2u + len;
            }
            at += len;
        }
    }
    uint32_t delivered = 0;
    if (delta == 0u) {
        ring_advance(&s->rx, span);
        s->rx_next += span;
        delivered = span;
        s->last_rx_len = span;
        while (s->ooo_n > 0u && s->ooo[0].off == s->rx_next) {
            ring_advance(&s->rx, s->ooo[0].n);
            s->rx_next += s->ooo[0].n;
            delivered += s->ooo[0].n;
            for (uint32_t k = 1; k < s->ooo_n; k++) {
                s->ooo[k - 1] = s->ooo[k];
            }
            s->ooo_n--;
        }
    } else {
        uint32_t at = s->ooo_n;
        while (at > 0u && (int32_t)(s->ooo[at - 1u].off - off) > 0) {
            s->ooo[at] = s->ooo[at - 1u];
            at--;
        }
        s->ooo[at].off = off;
        s->ooo[at].n = span;
        s->ooo_n++;
    }
    if (s->refused && (int32_t)(s->rx_next - s->refused_off) > 0) {
        s->refused = false; /* the frame thrown away has come again */
    }
    unlock(s);
    if (delivered > 0u) {
        s->nak_sent = false;
        (void)ag_port_sem_give(s->rx_sem);
        /*
         * A further hole behind the one just filled: one between frames
         * held, or the frame this end threw away itself.  Forgetting the
         * second kind promised room again, the frame after it did not fit
         * either, and every window of frames made a new hole (24% of the
         * wire with block mode under 5% loss, measured).
         */
        if (s->ooo_n > 0u || s->refused) {
            ask_again(s, now);
        }
    } else {
        ask_again(s, now);
    }
    return delivered;
}

/* No acknowledgement for too long with frames out: send the first again. */
void wl_rto_check(wlink_t *s)
{
    const int64_t now = ag_port_us();
    if (s->infl_n > 0u && now - s->ack_at > WL_RTO_US) {
        s->resend = true;
        s->resend_whole = false;
        s->resend_off = s->tx_base;
        s->ack_at = now;
        s->rewinds++;
    }
}


uint32_t wl_frame_len(const uint8_t *f, uint32_t have)
{
    if (have < WL_HDR) {
        return 0u;
    }
    if (f[0] != 0xA5u || f[2] != 0x5Au) {
        return UINT32_MAX;
    }
    const uint32_t flags = f[1];
    const uint32_t span = get16(f + 12);
    uint32_t       body = 0;
    if (flags & WL_F_SEG) {
        if (have < WL_HDR + 2u) {
            return 0u;
        }
        body = 2u + get16(f + WL_HDR);
    } else if (flags & WL_F_DATA) {
        body = span;
    }
    if (body > WL_FRAME_MAX - WL_HDR || span > WL_FRAME_MAX - WL_HDR) {
        return UINT32_MAX;
    }
    return WL_HDR + body;
}

void wl_stats(const wlink_t *s, ag_spilink_stats_t *out)
{
    out->bytes_dropped = s->bytes_dropped;
    out->open = s->open;
    out->frames_bad = s->frames_bad;
    out->seq_gaps = s->seq_gaps;
    out->rewinds = s->rewinds;
    out->naks = s->naks;
    out->peer_restarts = s->peer_restarts;
    out->starve_data = s->starve_data;
    out->starve_credit = s->starve_credit;
    out->frame = s->agreed ? s->frame : 0u;
}

int32_t wl_write(wlink_t *s, const void *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }
    if (s->lock == NULL) {
        return -AG_ENODEV;
    }

    /* A ring this size cannot hold more, so the rest is the caller's to bring
     * back - and saying so is what "-AG_EAGAIN rather than dropping" means. */
    lock(s);
    if (!s->open) {
        unlock(s);
        return -AG_ENODEV;
    }
    const uint32_t want = (len > s->txcap) ? s->txcap : (uint32_t)len;
    const uint32_t n = ring_put(&s->tx, buf, want);
    const bool     wake = s->kick_writes && n > 0u;
    unlock(s);

    /* An idle master is waiting out its poll; this is something to clock. */
    if (wake) {
        (void)ag_port_sem_give(s->tx_sem);
    }

    /* Nothing was dropped: what did not fit is still the caller's. */
    return (n > 0u) ? (int32_t)n : -AG_EAGAIN;
}

/*
 * Write all of `buf`, waiting for room as the service makes it, or until
 * `timeout_ms` has passed with nothing more taken.  Returns what was taken.
 *
 * Why it exists: a caller of the plain write that found the ring full could
 * only sleep and try again, and the shortest sleep there is is a tick - a
 * millisecond, in which a 40 MHz link empties 4 KB and then clocks frames
 * with nothing in them.  Measured: 72% of the wire with 8 KB writes, the
 * same with the receiver checking nothing, so it was never the receiver.
 */
int32_t wl_write_wait(wlink_t *s, const void *buf, size_t len,
                      uint32_t timeout_ms)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }
    if (s->lock == NULL) {
        return -AG_ENODEV;
    }
    const uint8_t *p = (const uint8_t *)buf;
    size_t         done = 0;
    while (done < len) {
        lock(s);
        if (!s->open) {
            unlock(s);
            break;
        }
        const uint32_t n = ring_put(&s->tx, p + done, (uint32_t)(len - done));
        const bool     wake = s->kick_writes && n > 0u;
        unlock(s);
        if (wake) {
            (void)ag_port_sem_give(s->tx_sem);
        }
        done += n;
        if (done < len &&
            !ag_port_sem_take(s->space_sem, ticks_at_least(timeout_ms))) {
            break;
        }
    }
    return (done > 0u) ? (int32_t)done : -AG_EAGAIN;
}

/*
 * BLOCK MODE.  One block in: its record header and its bytes go into the
 * transmit ring together, and the block is remembered until it is
 * acknowledged, with its number and whether it is worth sending again.
 */
/* Of two parts taken as one, from byte `done` on: what the ring took. */
static uint32_t put2(ring_t *r, const uint8_t *a, uint32_t alen,
                     const uint8_t *b, uint32_t blen, uint32_t done)
{
    uint32_t put = 0;
    if (done < alen) {
        put = ring_put(r, a + done, alen - done);
        if (done + put < alen) {
            return put;
        }
    }
    const uint32_t bd = done + put - alen;
    return put + ((bd < blen) ? ring_put(r, b + bd, blen - bd) : 0u);
}

int32_t wl_send(wlink_t *s, const void *buf, size_t len, uint32_t flags,
                uint32_t timeout_ms)
{
    return wl_send2(s, NULL, 0u, buf, len, flags, timeout_ms);
}

/*
 * The block in two parts - a driver's own head, and the caller's bytes - so
 * the driver needs no buffer of its own to join them: NODELINK and LINKSPI
 * each kept 4 KB a wire for nothing else (25 Sep 2026).
 */
int32_t wl_send2(wlink_t *s, const void *head, size_t hlen, const void *body,
                 size_t blen, uint32_t flags, uint32_t timeout_ms)
{
    const size_t len = hlen + blen;
    if ((head == NULL && hlen != 0u) || (body == NULL && blen != 0u) ||
        len == 0 || len > 0xFFFFu) {
        return -AG_EINVAL;
    }
    if (s->lock == NULL) {
        return -AG_ENODEV;
    }
    const bool     reliable = (flags & AG_SPILINK_RELIABLE) != 0u;
    const uint32_t need = WL_REC + (uint32_t)len;
    if (!s->blocks || need > s->txcap) {
        return -AG_EINVAL;
    }
    for (;;) {
        lock(s);
        if (!s->open) {
            unlock(s);
            return -AG_ENODEV;
        }
        if (ring_space(&s->tx) >= WL_REC) {
            /*
             * The header now and the bytes as room is made: waiting for room
             * for the WHOLE block left a 2 KB block waiting while a frame
             * went out half empty (81% of the wire, measured).  The frame
             * builder takes only what is in the ring, and a block cut across
             * frames is simply sent again whole if lost.  One writer a link.
             */
            uint8_t hdr[WL_REC];
            const uint16_t no = s->next_block++;
            put16(hdr, (uint32_t)len);
            put16(hdr + 2, no);
            hdr[4] = reliable ? AG_SPILINK_RELIABLE : 0u;
            hdr[5] = 0;
            put16(hdr + 6, no);
            (void)ring_put(&s->tx, hdr, WL_REC);
            const uint8_t *const a = (const uint8_t *)head;
            const uint8_t *const b = (const uint8_t *)body;
            size_t done = put2(&s->tx, a, (uint32_t)hlen, b, (uint32_t)blen, 0u);
            const uint32_t epoch = s->epoch;
            unlock(s);
            for (;;) {
                if (s->kick_writes) {
                    (void)ag_port_sem_give(s->tx_sem);
                }
                if (done >= len) {
                    return (int32_t)len;
                }
                /* the header is in: the rest of the block has to follow,
                 * however long room takes, or the stream would be cut */
                (void)ag_port_sem_take(s->space_sem,
                                       ticks_at_least(timeout_ms));
                lock(s);
                if (!s->open) {
                    unlock(s);
                    return -AG_ENODEV;
                }
                if (s->epoch != epoch) {
                    unlock(s);
                    return -AG_EIO; /* the other end started over */
                }
                done += put2(&s->tx, a, (uint32_t)hlen, b, (uint32_t)blen,
                             (uint32_t)done);
                unlock(s);
            }
        }
        unlock(s);
        if (!ag_port_sem_take(s->space_sem, ticks_at_least(timeout_ms))) {
            return -AG_EAGAIN;
        }
    }
}

/*
 * One block out, in order: its bytes into `buf` (cut at `cap`), and in
 * `*info` its number and flags - or, for blocks that were lost and not sent
 * again, AG_SPILINK_LOST with the first and last of them, and no bytes.
 */
int32_t wl_recv(wlink_t *s, void *buf, size_t cap, ag_spilink_block_t *info,
                uint32_t timeout_ms)
{
    return wl_recv2(s, NULL, 0u, buf, cap, info, timeout_ms);
}

/* Of `have` bytes at block offset `done`: into head, then buf, as far as
 * each reaches; the rest is dropped.  What was placed. */
static uint32_t take2(ring_t *r, uint8_t *head, uint32_t hcap, uint8_t *buf,
                      uint32_t cap, uint32_t done, uint32_t have)
{
    uint32_t placed = 0, gone = 0;
    if (done < hcap && head != NULL) {
        const uint32_t k = (hcap - done < have) ? hcap - done : have;
        (void)ring_take(r, head + done, k);
        placed += k;
        gone += k;
    }
    const uint32_t at = done + gone; /* block offset now */
    if (gone < have && at >= hcap && buf != NULL && at - hcap < cap) {
        const uint32_t room = cap - (at - hcap);
        const uint32_t k = (room < have - gone) ? room : have - gone;
        (void)ring_take(r, buf + (at - hcap), k);
        placed += k;
        gone += k;
    }
    ring_commit(r, have - gone);
    return placed;
}

int32_t wl_recv2(wlink_t *s, void *head, size_t hcap, void *buf, size_t cap,
                 ag_spilink_block_t *info, uint32_t timeout_ms)
{
    if (info == NULL) {
        return -AG_EINVAL;
    }
    if (head == NULL) {
        hcap = 0u;
    }
    if (s->lock == NULL || !s->blocks) {
        return (s->lock == NULL) ? -AG_ENODEV : -AG_EINVAL;
    }
    const ag_port_ticks_t start = ag_port_ticks();
    const ag_port_ticks_t span = ticks_at_least(timeout_ms);
    for (;;) {
        lock(s);
        if (!s->open) {
            unlock(s);
            return -AG_ENODEV;
        }
        uint8_t hdr[WL_REC];
        if (s->rx.count >= WL_REC) {
            (void)ring_peek(&s->rx, hdr, WL_REC);
            ring_commit(&s->rx, WL_REC);
            info->no = (uint16_t)get16(hdr + 2);
            info->last = (uint16_t)get16(hdr + 6);
            info->flags = hdr[4];
            const uint32_t len = get16(hdr);
            if (hdr[4] & WL_REC_LOST) {
                /* a LOST record arrives whole, in one frame */
                ring_commit(&s->rx, len);
                unlock(s);
                room_made(s);
                return 0;
            }
            /*
             * The block is taken as it arrives, and the call waits for the
             * rest, however long - as a writer does for room once its header
             * is in.  Leaving a block cut across two frames in the ring until
             * the second arrived took its first piece off the room of every
             * frame (Z->C3 81% of the wire against 92% for bytes, measured).
             */
            uint32_t       done = 0, n = 0;
            const uint32_t epoch = s->epoch;
            for (;;) {
                uint32_t have = s->rx.count;
                if (have > len - done) {
                    have = len - done;
                }
                n += take2(&s->rx, (uint8_t *)head, (uint32_t)hcap,
                           (uint8_t *)buf, (uint32_t)cap, done, have);
                done += have;
                unlock(s);
                room_made(s);
                if (done >= len) {
                    return (int32_t)n;
                }
                (void)ag_port_sem_take(s->rx_sem, ticks_at_least(100));
                lock(s);
                if (!s->open) {
                    unlock(s);
                    return -AG_ENODEV;
                }
                if (s->epoch != epoch) {
                    unlock(s);
                    return -AG_EIO; /* the other end started over */
                }
            }
        }
        unlock(s);
        const ag_port_ticks_t used = (ag_port_ticks_t)(ag_port_ticks() - start);
        if (used >= span) {
            return -AG_EAGAIN;
        }
        (void)ag_port_sem_take(s->rx_sem, span - used);
    }
}

int32_t wl_read(wlink_t *s, void *buf, size_t len, uint32_t timeout_ms)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }
    if (s->lock == NULL) {
        return -AG_ENODEV;
    }

    /*
     * A minute is a link reader's idea of forever, and treating it that way
     * keeps ag_port_ms_to_ticks away from a value that would overflow it.
     */
    const bool      forever = (timeout_ms >= 60000u);
    ag_port_ticks_t span = forever ? 0u : ticks_at_least(timeout_ms);
    const ag_port_ticks_t slice = ticks_at_least(WL_WAIT_SLICE_MS);
    const ag_port_ticks_t start = ag_port_ticks();
    const uint32_t        want =
        (len > s->rxcap && s->rxcap != 0u) ? s->rxcap : (uint32_t)len;

    for (;;) {
        lock(s);
        if (!s->open) {
            unlock(s);
            return -AG_ENODEV;
        }
        const uint32_t n = ring_take(&s->rx, buf, want);
        unlock(s);

        if (n > 0u) {
            room_made(s);
            return (int32_t)n;
        }
        if (timeout_ms == 0u) {
            return -AG_EAGAIN;
        }

        ag_port_ticks_t wait = slice;
        if (!forever) {
            const ag_port_ticks_t used =
                (ag_port_ticks_t)(ag_port_ticks() - start);
            if (used >= span) {
                return -AG_EAGAIN;
            }
            const ag_port_ticks_t left = span - used;
            if (left < wait) {
                wait = left;
            }
            if (wait == 0u) {
                wait = 1u; /* a wait of no ticks is a poll, and this is a wait */
            }
        }

        /*
         * Outside the lock: a read with a timeout is a wait, and holding the
         * ring while waiting would stop the service task from delivering into
         * it.  The wake is a count, so a block that arrived between the empty
         * check and this take is not lost.
         */
        (void)ag_port_sem_take(s->rx_sem, wait);
    }
}
