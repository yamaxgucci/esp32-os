/*
 * ArgonOS - NODELINK.SYS: the wire to another node of the same system.
 *
 * Publishes an AG_DEV_LINK device (ABI 0.49) whose class vtable carries
 * MESSAGES - each with a channel and a service class - and hides NLINK
 * (apps/common/nlink) entirely.  Above it nothing knows about frames, windows,
 * sequence numbers or which class is being served; below it nothing knows what
 * the messages mean.
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include --include apps/common --include apps/common/nlink \
 *       -o build\apps\NODELINK.SYS apps/nodelink/nodelink.c apps/common/nlink/ag_nlink.c
 *   drv load a:\nodelink.sys      (registers /dev/node0)
 *
 * WHICH END LISTENS is not this driver's business and deliberately not in the
 * protocol: on a UART both ends simply talk, and on SPI later one end will hold
 * the clock.  Symmetry here means the same image runs on every board, which is
 * the property a cluster of identical boards needs most.
 *
 * A TASK OF ITS OWN (ABI 0.50), and the difference it makes is the difference
 * between a link and a node.
 *
 * api->task->create refuses a caller with no current process - a thread must
 * belong to something that can be made to end - so a .SYS used to advance only
 * on whichever task happened to call in.  That is enough while somebody is
 * asking and useless for a node, whose whole job is to hear what it did not ask
 * for.  api->task->svc_start gives the task to the MODULE instead, the kernel
 * stops it when the module is unloaded, and it runs below every application:
 * the wire's own task above them once took a game from 40 fps to unplayable.
 *
 * Every entry point still pumps, and that is not redundancy.  A caller that is
 * already awake should not wait for a task to be scheduled, and the service
 * task is what covers the times when there is no caller at all.  One mutex
 * makes the two safe together.
 *
 * ONE MUTEX over the wire, the assembler and the queues, for the same reason as
 * the radio driver: there is then a single reader, and the frame assembler
 * needs no locking of its own.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

#include "nlink/ag_nlink.h"

AG_DRV("NODELINK", "0.1", "argon");

/*
 * The two enumerations of the service classes have to agree, and nothing
 * enforces it but this.  The ABI cannot include nlink's header (nlink knows
 * nothing of ArgonOS, which is what lets its codec be tested on a development
 * machine), so the numbers are mirrored by hand - and a mirror nobody checks is
 * a mirror that drifts.  Here it costs one line and fails the build.
 */
_Static_assert((int)NL_CLS_CONTROL == (int)AG_LINK_CONTROL &&
                   (int)NL_CLS_STREAM == (int)AG_LINK_STREAM &&
                   (int)NL_CLS_BULK == (int)AG_LINK_BULK &&
                   (int)NL_CLASSES == (int)AG_LINK_CLASSES,
               "nlink and the ABI disagree about the service classes");

#define ND_UART_DEFAULT 1       /* QEMU emulates UART0 (console) and UART1 only */
#define ND_BAUD         921600u

/*
 * The windows, in frames, and they are arithmetic rather than taste: a window
 * has to hold a round trip's worth of data or the wire idles waiting for
 * acknowledgements (ag_nlink.h, "SIZING THE WINDOW").  CONTROL sends little and
 * rarely, so four; BULK streams, so twenty-four, which is 6 KB in flight and
 * covers a millisecond at 5 MB/s.
 */
#define ND_WIN_CONTROL 4u
#define ND_WIN_BULK    24u

/* The stream class is not windowed - it is not retransmitted - so its queue is
 * a plain ring of whole messages waiting for their slot. */
#define ND_STREAM_QUEUE 8u

/* Delivered messages waiting to be taken by recv(). */
#define ND_RX_QUEUE 8u

/* The reservation, in bytes per period: 44.1 kHz stereo is 176400 B/s and a
 * 10 ms period is 1764.  A number to be set from what is actually being sent,
 * not a constant of the universe - see ag_nlink_sched_init. */
#define ND_RESERVE_BYTES 1764u
#define ND_PERIOD_MS     10u

/*
 * How long to wait for an acknowledgement before sending the window again.
 *
 * A PLACEHOLDER, and marked as one.  It has to be longer than a round trip or
 * the link retransmits frames that were merely in flight - at 40 ms it did
 * that between two emulators sharing one PC, which is not even a slow wire.
 * The right number comes from measuring the round trip on the hardware this
 * runs on, and adapting to it the way every long-lived protocol ends up doing;
 * until that measurement exists, generous beats clever.
 */
#define ND_RTO_MS 150u

/* frame assembler states */
enum { SM_SYNC = 0, SM_HDR, SM_BODY, SM_PSUM };

typedef struct {
    uint8_t  chan;
    uint16_t len;
    uint8_t  data[NL_QUANTUM];
} msg_t;

typedef struct {
    int        uart;
    ag_mutex_t lock;
    bool       up;

    /* Send side: a window per reliable class, a ring for the stream class. */
    ag_nlink_slot_t slot_ctl[ND_WIN_CONTROL];
    ag_nlink_slot_t slot_bulk[ND_WIN_BULK];
    ag_nlink_tx_t   tx[NL_CLASSES];
    msg_t           stream[ND_STREAM_QUEUE];
    uint32_t        stream_r, stream_w, stream_fill;
    uint16_t        stream_seq;

    ag_nlink_rx_t    rx[NL_CLASSES];
    /* The acknowledgement last PUT ON THE WIRE for each class, as against the
     * one rx would give now.  When they differ, the far end is waiting for
     * something only we can say. */
    uint16_t         acked[NL_CLASSES];
    bool             acked_any[NL_CLASSES];
    ag_nlink_sched_t sched;
    uint32_t         period_at; /* ms, when this reservation period began */

    /* Receive side: whole messages, waiting to be taken. */
    msg_t    inbox[ND_RX_QUEUE];
    uint32_t in_r, in_w, in_fill;
    uint32_t in_dropped;

    /* frame assembler */
    int            sm;
    uint8_t        win[4];
    uint8_t        hdr[NL_HDR_SIZE];
    uint32_t       hlen;
    ag_nlink_hdr_t cur;
    uint8_t        body[NL_QUANTUM];
    uint32_t       blen;
    uint8_t        psum[NL_PSUM_SIZE];
    uint32_t       plen;

    uint32_t tx_bytes, rx_bytes, bad, acks;
} node_t;

static node_t s_nd;

static const ag_io_api_t   *IO;
static const ag_task_api_t *TASK;

static uint32_t now_ms(void)
{
    return (uint32_t)(g_ag_api->time->us() / 1000u);
}

/* ---------------------------------------------------------------------- */
/* The wire                                                                */
/* ---------------------------------------------------------------------- */

static void wire_write(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t         left = len;
    while (left > 0) {
        const int32_t n = IO->uart_write(s_nd.uart, p, left);
        if (n <= 0) {
            return; /* the link is gone; the window will time out */
        }
        p += (size_t)n;
        left -= (size_t)n;
        s_nd.tx_bytes += (uint32_t)n;
    }
}

/* ---------------------------------------------------------------------- */
/* Receiving                                                               */
/* ---------------------------------------------------------------------- */

static void inbox_put(uint8_t chan, const uint8_t *data, uint16_t len)
{
    if (s_nd.in_fill >= ND_RX_QUEUE) {
        /*
         * Dropped, and counted.  The alternative is to stop acknowledging and
         * let the far end's window close, which sounds tidier and is worse: the
         * sender then blocks on a link that is only slow because nobody here is
         * reading it, and a control message behind the jam waits on a picture
         * nobody wants.  A count says the same thing without the deadlock.
         */
        s_nd.in_dropped++;
        return;
    }
    msg_t *m = &s_nd.inbox[s_nd.in_w];
    m->chan = chan;
    m->len = len;
    if (len > 0) {
        memcpy(m->data, data, len);
    }
    s_nd.in_w = (s_nd.in_w + 1u) % ND_RX_QUEUE;
    s_nd.in_fill++;
}

static void frame_arrived(void)
{
    const ag_nlink_hdr_t *h = &s_nd.cur;
    const uint8_t         cls = h->cls;

    if (cls >= NL_CLASSES) {
        s_nd.bad++;
        return;
    }
    if (!ag_nlink_check_psum(h, s_nd.body, s_nd.psum)) {
        /*
         * A payload that does not check out is treated exactly as a payload
         * that never came: dropped, not acknowledged, and left to the far end's
         * timer.  There is no separate "corrupt" path because there is nothing
         * different to do - and pretending otherwise would mean acknowledging
         * bytes we know are wrong.
         */
        s_nd.bad++;
        return;
    }
    s_nd.up = true;

    /* The peer's acknowledgement rides on every frame it sends. */
    if (h->ack != NL_ACK_NONE) {
        ag_nlink_tx_ack(&s_nd.tx[cls], h->ack);
    }

    if ((h->flags & NL_F_ACKONLY) != 0u) {
        /* Nothing but the acknowledgement above.  No sequence, so nothing to
         * accept; no delivery, so nothing to queue; and not acknowledged
         * itself, so the two ends cannot answer each other for ever. */
        return;
    }

    if (cls == NL_CLS_STREAM) {
        /* Not sequenced and not acknowledged: whatever arrives is delivered,
         * and what did not arrive is not worth knowing about by the time it
         * could be asked for. */
        inbox_put(h->chan, s_nd.body, h->len);
        return;
    }
    if (ag_nlink_rx_accept(&s_nd.rx[cls], h)) {
        inbox_put(h->chan, s_nd.body, h->len);
    }
}

static void feed(uint8_t b)
{
    switch (s_nd.sm) {
    case SM_SYNC:
        s_nd.win[0] = s_nd.win[1];
        s_nd.win[1] = s_nd.win[2];
        s_nd.win[2] = s_nd.win[3];
        s_nd.win[3] = b;
        if (s_nd.win[0] == 'N' && s_nd.win[1] == 'L' && s_nd.win[2] == 'N' &&
            s_nd.win[3] == 'K') {
            memcpy(s_nd.hdr, s_nd.win, 4);
            s_nd.hlen = 4;
            s_nd.sm = SM_HDR;
        }
        break;

    case SM_HDR:
        s_nd.hdr[s_nd.hlen++] = b;
        if (s_nd.hlen < NL_HDR_SIZE) {
            break;
        }
        if (!ag_nlink_unpack(&s_nd.cur, s_nd.hdr)) {
            /* The header failed its own CRC, so `len` is not a number to
             * believe - resync from the next magic rather than skipping
             * however many bytes a corrupt field claims. */
            s_nd.bad++;
            s_nd.sm = SM_SYNC;
            break;
        }
        s_nd.blen = 0;
        s_nd.plen = 0;
        s_nd.sm = (s_nd.cur.len > 0) ? SM_BODY
                  : ((s_nd.cur.flags & NL_F_PSUM) ? SM_PSUM : SM_SYNC);
        if (s_nd.sm == SM_SYNC) {
            frame_arrived();
        }
        break;

    case SM_BODY:
        s_nd.body[s_nd.blen++] = b;
        if (s_nd.blen == s_nd.cur.len) {
            if ((s_nd.cur.flags & NL_F_PSUM) != 0) {
                s_nd.sm = SM_PSUM;
            } else {
                frame_arrived();
                s_nd.sm = SM_SYNC;
            }
        }
        break;

    case SM_PSUM:
        s_nd.psum[s_nd.plen++] = b;
        if (s_nd.plen == NL_PSUM_SIZE) {
            frame_arrived();
            s_nd.sm = SM_SYNC;
        }
        break;

    default:
        s_nd.sm = SM_SYNC;
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* Sending                                                                 */
/* ---------------------------------------------------------------------- */

/* The acknowledgement to put on the next frame of this class. */
static uint16_t ack_for(uint8_t cls)
{
    return (cls < NL_CLASSES) ? ag_nlink_rx_ack(&s_nd.rx[cls]) : NL_ACK_NONE;
}

static void emit(uint8_t cls, uint8_t chan, uint16_t seq, uint8_t flags,
                 const uint8_t *body, uint16_t len)
{
    ag_nlink_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.cls = cls;
    h.chan = chan;
    h.seq = seq;
    h.flags = flags;
    h.ack = ack_for(cls);
    /* The credit field is reserved: this driver's back-pressure is the window
     * itself, and a second mechanism that says the same thing differently is a
     * second mechanism to get wrong. */
    h.credit = 0;
    h.len = len;

    /* Whatever we are sending, it carries the current acknowledgement - so
     * record what the far end has now been told. */
    if (cls < NL_CLASSES) {
        s_nd.acked[cls] = h.ack;
        s_nd.acked_any[cls] = (h.ack != NL_ACK_NONE);
    }

    uint8_t hdr[NL_HDR_SIZE];
    ag_nlink_pack(hdr, &h);
    wire_write(hdr, NL_HDR_SIZE);
    if (len > 0) {
        wire_write(body, len);
    }
    if ((flags & NL_F_PSUM) != 0) {
        uint8_t ps[NL_PSUM_SIZE];
        ag_nlink_put_psum(ps, body, len);
        wire_write(ps, NL_PSUM_SIZE);
    }
}

/* Tell the scheduler what is queued, so its answer is about now. */
static void offer_all(uint32_t t)
{
    for (int c = 0; c < NL_CLASSES; c++) {
        if (c == NL_CLS_STREAM) {
            const uint32_t head =
                (s_nd.stream_fill > 0)
                    ? (uint32_t)s_nd.stream[s_nd.stream_r].len + NL_HDR_SIZE
                    : 0u;
            ag_nlink_sched_offer(&s_nd.sched, c,
                                 s_nd.stream_fill * (NL_QUANTUM + NL_HDR_SIZE),
                                 head);
        } else {
            ag_nlink_sched_offer(&s_nd.sched, c,
                                 ag_nlink_tx_waiting_bytes(&s_nd.tx[c], t),
                                 ag_nlink_tx_head_len(&s_nd.tx[c], t));
        }
    }
}

/*
 * One turn of the wire: read what has arrived, then send at most one frame of
 * whichever class the scheduler picks.
 *
 * At most one, deliberately.  The scheduler's promise is that a frame offered
 * now goes out within a quantum, and that promise is only kept if the decision
 * is remade between frames - a loop that drained a whole class would be exactly
 * the 13 ms of pixels that starved the sound on the real board.
 *
 * Caller holds the lock.
 */
static void turn(void)
{
    uint8_t buf[128];
    for (;;) {
        const int32_t n = IO->uart_read(s_nd.uart, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        s_nd.rx_bytes += (uint32_t)n;
        for (int32_t i = 0; i < n; i++) {
            feed(buf[i]);
        }
        if ((uint32_t)n < sizeof(buf)) {
            break;
        }
    }

    const uint32_t t = now_ms();
    if ((uint32_t)(t - s_nd.period_at) >= ND_PERIOD_MS) {
        ag_nlink_sched_tick(&s_nd.sched);
        s_nd.period_at = t;
    }

    offer_all(t);
    const int cls = ag_nlink_sched_pick(&s_nd.sched);
    if (cls < 0) {
        /*
         * Nothing to say - which is exactly when saying "I have your frames"
         * is free.  Without this the last answer of a conversation sits
         * unacknowledged until its timer fires and the whole window goes
         * again: one retransmission in twenty-five on a socket that loses
         * nothing.
         */
        for (int c = 0; c < NL_CLASSES; c++) {
            if (c == NL_CLS_STREAM) {
                continue; /* not sequenced, so nothing to acknowledge */
            }
            const uint16_t now_ack = ag_nlink_rx_ack(&s_nd.rx[c]);
            if (now_ack == NL_ACK_NONE) {
                continue;
            }
            if (s_nd.acked_any[c] && s_nd.acked[c] == now_ack) {
                continue; /* already told them */
            }
            emit((uint8_t)c, 0, 0, NL_F_ACKONLY, NULL, 0);
            s_nd.acks++;
            return; /* one frame per turn, here as everywhere */
        }
        return;
    }

    if (cls == NL_CLS_STREAM) {
        const msg_t *m = &s_nd.stream[s_nd.stream_r];
        emit(NL_CLS_STREAM, m->chan, s_nd.stream_seq++, NL_F_EOM, m->data,
             m->len);
        s_nd.stream_r = (s_nd.stream_r + 1u) % ND_STREAM_QUEUE;
        s_nd.stream_fill--;
        ag_nlink_sched_sent(&s_nd.sched, cls, (uint32_t)m->len + NL_HDR_SIZE);
        return;
    }

    const ag_nlink_slot_t *s = ag_nlink_tx_next(&s_nd.tx[cls], t);
    if (s == NULL) {
        return;
    }
    emit((uint8_t)cls, s->chan, s->seq, s->flags, s->data, s->len);
    ag_nlink_sched_sent(&s_nd.sched, cls,
                        (uint32_t)s->len + NL_HDR_SIZE + NL_PSUM_SIZE);
}

/* ---------------------------------------------------------------------- */
/* The class vtable                                                        */
/* ---------------------------------------------------------------------- */

static ag_err_t nd_start(ag_device_t *dev)
{
    (void)dev;
    TASK->mutex_lock(s_nd.lock, UINT32_MAX);
    turn();
    TASK->mutex_unlock(s_nd.lock);
    return AG_OK;
}

static bool nd_up(ag_device_t *dev)
{
    (void)dev;
    TASK->mutex_lock(s_nd.lock, UINT32_MAX);
    turn();
    const bool r = s_nd.up;
    TASK->mutex_unlock(s_nd.lock);
    return r;
}

static int32_t nd_send(ag_device_t *dev, uint8_t chan, uint8_t cls,
                       const void *buf, size_t len)
{
    (void)dev;
    if (buf == NULL || cls >= NL_CLASSES) {
        return -AG_EINVAL;
    }
    if (len > NL_QUANTUM) {
        /* Refused rather than split: only the caller knows where its message
         * boundaries are, and a driver that guessed would deliver two halves
         * that look like two messages. */
        return -AG_ERANGE;
    }

    TASK->mutex_lock(s_nd.lock, UINT32_MAX);
    int32_t r;
    if (cls == NL_CLS_STREAM) {
        if (s_nd.stream_fill >= ND_STREAM_QUEUE) {
            /*
             * The stream class drops rather than blocks, here as on the wire.
             * A caller told -AG_EAGAIN would retry, and a retried audio frame
             * is the thing this class exists to not do.
             */
            s_nd.stream_r = (s_nd.stream_r + 1u) % ND_STREAM_QUEUE;
            s_nd.stream_fill--;
        }
        msg_t *m = &s_nd.stream[s_nd.stream_w];
        m->chan = chan;
        m->len = (uint16_t)len;
        memcpy(m->data, buf, len);
        s_nd.stream_w = (s_nd.stream_w + 1u) % ND_STREAM_QUEUE;
        s_nd.stream_fill++;
        r = (int32_t)len;
    } else if (ag_nlink_tx_queue(&s_nd.tx[cls], chan, buf, (uint16_t)len,
                                 true)) {
        r = (int32_t)len;
    } else {
        r = -AG_EAGAIN; /* the window is full: back-pressure, not an error */
    }
    turn();
    TASK->mutex_unlock(s_nd.lock);
    return r;
}

static int32_t nd_recv(ag_device_t *dev, uint8_t *chan, void *buf, size_t max,
                       uint32_t timeout_ms)
{
    (void)dev;
    if (buf == NULL || chan == NULL) {
        return -AG_EINVAL;
    }
    /*
     * The deadline is read off the clock, not counted up by the sleep length.
     *
     * It was an accumulator - `waited += 2` beside a `sleep_ms(2)` - and that
     * is wrong by a factor nobody sees until it matters: the tick is 100 Hz, so
     * a two-millisecond sleep is a ten-millisecond sleep, and every timeout
     * here was five times what it said.  On one emulator that is slow; with two
     * of them sharing this PC it is a `link ping` that never appears to return,
     * which is exactly how it was found.
     */
    const uint32_t deadline = now_ms() + timeout_ms;
    for (;;) {
        TASK->mutex_lock(s_nd.lock, UINT32_MAX);
        turn();
        if (s_nd.in_fill > 0) {
            const msg_t   *m = &s_nd.inbox[s_nd.in_r];
            const uint16_t n = (m->len < max) ? m->len : (uint16_t)max;
            memcpy(buf, m->data, n);
            *chan = m->chan;
            s_nd.in_r = (s_nd.in_r + 1u) % ND_RX_QUEUE;
            s_nd.in_fill--;
            TASK->mutex_unlock(s_nd.lock);
            return (int32_t)n;
        }
        TASK->mutex_unlock(s_nd.lock);

        if ((int32_t)(now_ms() - deadline) >= 0) {
            return -AG_EAGAIN;
        }
        TASK->sleep_ms(1);
    }
}

static void nd_poll(ag_device_t *dev, uint32_t ms)
{
    (void)dev;
    const uint32_t deadline = now_ms() + ms;
    for (;;) {
        TASK->mutex_lock(s_nd.lock, UINT32_MAX);
        turn();
        TASK->mutex_unlock(s_nd.lock);
        if ((int32_t)(now_ms() - deadline) >= 0) {
            return;
        }
        TASK->sleep_ms(1);
    }
}

static ag_err_t nd_stats(ag_device_t *dev, ag_link_stats_t *out)
{
    (void)dev;
    if (out == NULL) {
        return -AG_EINVAL;
    }
    TASK->mutex_lock(s_nd.lock, UINT32_MAX);
    memset(out, 0, sizeof(*out));
    for (int c = 0; c < NL_CLASSES; c++) {
        out->sent += s_nd.tx[c].sent;
        out->retransmits += s_nd.tx[c].retransmits;
        out->received += s_nd.rx[c].taken;
        out->gaps += s_nd.rx[c].gaps;
        out->dups += s_nd.rx[c].dups;
    }
    out->sent += s_nd.sched.sent[NL_CLS_STREAM];
    out->bad = s_nd.bad;
    out->acks = s_nd.acks;
    out->tx_bytes = s_nd.tx_bytes;
    out->rx_bytes = s_nd.rx_bytes;
    TASK->mutex_unlock(s_nd.lock);
    return AG_OK;
}

/* ---------------------------------------------------------------------- */
/* The task that listens when nobody is asking                            */
/* ---------------------------------------------------------------------- */

static ag_svc_t s_svc;

static void pump_task(void *arg)
{
    (void)arg;
    /*
     * Short sleeps rather than a blocking read, because the read has to happen
     * under the same mutex everything else takes, and a task that slept inside
     * the lock would stop the shell from sending.  Ten milliseconds is a tick;
     * asking for less does not get less.
     */
    while (!ag_api()->task->svc_stopping(s_svc)) {
        TASK->mutex_lock(s_nd.lock, UINT32_MAX);
        turn();
        TASK->mutex_unlock(s_nd.lock);
        TASK->sleep_ms(1);
    }
}

static const ag_dev_ops_t k_dev_ops = {0};

static const ag_link_ops_t k_link_ops = {
    .size = sizeof(ag_link_ops_t),
    .start = nd_start,
    .up = nd_up,
    .send = nd_send,
    .recv = nd_recv,
    .poll = nd_poll,
    .stats = nd_stats,
};

/* ---------------------------------------------------------------------- */
/* Lifecycle                                                               */
/* ---------------------------------------------------------------------- */

ag_err_t ag_driver_init(void)
{
    IO = ag_api()->io;
    TASK = ag_api()->task;
    if (IO == NULL || TASK == NULL || !AG_HAS(ag_api()->dev, add) ||
        !AG_HAS(IO, uart_read) || !AG_HAS(TASK, mutex_create)) {
        return -AG_ENOSYS;
    }

    memset(&s_nd, 0, sizeof(s_nd));
    s_nd.sm = SM_SYNC;
    s_nd.uart = ND_UART_DEFAULT;
    uint32_t baud = ND_BAUD;
    if (AG_HAS(ag_api()->cfg, get_int)) {
        s_nd.uart = ag_api()->cfg->get_int("link.uart", ND_UART_DEFAULT);
        baud = (uint32_t)ag_api()->cfg->get_int("link.baud", (int32_t)ND_BAUD);
    }

    s_nd.lock = TASK->mutex_create();
    if (s_nd.lock == NULL) {
        return -AG_ENOMEM;
    }

    ag_nlink_tx_init(&s_nd.tx[NL_CLS_CONTROL], NL_CLS_CONTROL, s_nd.slot_ctl,
                     ND_WIN_CONTROL, ND_RTO_MS);
    ag_nlink_tx_init(&s_nd.tx[NL_CLS_BULK], NL_CLS_BULK, s_nd.slot_bulk,
                     ND_WIN_BULK, ND_RTO_MS);
    /* The stream class has no window and no slots; its tx is left zeroed so
     * that anything reading its counters gets zeros rather than rubbish. */
    for (int c = 0; c < NL_CLASSES; c++) {
        ag_nlink_rx_init(&s_nd.rx[c]);
    }
    ag_nlink_sched_init(&s_nd.sched, ND_RESERVE_BYTES);
    s_nd.period_at = now_ms();

    if (IO->uart_config != NULL) {
        const ag_err_t ue = IO->uart_config(s_nd.uart, baud, 8, 0, 1);
        if (ue != AG_OK) {
            ag_log(AG_LOG_WARN, "nodelink",
                   "uart%d did not come up: %d (set uart%d.tx/rx in BOARD.CFG)",
                   s_nd.uart, (int)ue, s_nd.uart);
        }
    }

    const ag_dev_add_t desc = {
        .name = "node0",
        .driver = "NODELINK",
        .cls = AG_DEV_LINK,
        .flags = 0,
        .ops = &k_dev_ops,        /* the registry requires one; empty = ENOTSUP */
        .class_ops = &k_link_ops, /* the vtable the kernel reads                */
        .priv = &s_nd,
    };
    const ag_err_t err = ag_dev_add(&desc);
    if (err != AG_OK) {
        return err;
    }
    /*
     * The listener.  Started after the device exists, so that a frame arriving
     * in the same millisecond has somewhere to go.
     *
     * Not fatal if it cannot start: the link still works for anything that
     * calls in, which is every `link` command - it simply cannot hear an
     * unsolicited request.  Said out loud rather than assumed, because "the
     * other node is silent" and "this node was not listening" look identical
     * from the far end.
     */
    if (AG_HAS(ag_api()->task, svc_start)) {
        s_svc = ag_api()->task->svc_start(pump_task, NULL, "nodelink", 4096, 0);
    }
    if (s_svc == NULL) {
        ag_log(AG_LOG_WARN, "nodelink",
               "no service task: this node answers only while asked");
    }

    ag_log(AG_LOG_INFO, "nodelink",
           "/dev/node0 on uart%d at %u baud%s - bind with `link use node0`",
           s_nd.uart, (unsigned)baud, (s_svc != NULL) ? ", listening" : "");
    return AG_OK;
}
