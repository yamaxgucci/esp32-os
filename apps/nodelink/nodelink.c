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
 * Where the timeout STARTS.  It is no longer the answer: NLINK measures the
 * round trip and follows it (see ag_nlink.h), because a fixed number is wrong
 * at every speed - too short and the link resends frames that were merely in
 * flight, which costs bandwidth and looks exactly like a lossy wire with
 * nothing actually lost; too long and a real loss stalls the window for as
 * long as the number says.  The same code runs on a 16 Mbaud pair of wires and
 * on two emulators sharing a busy PC, and the round trip differs between them
 * by two orders of magnitude.
 *
 * Generous as a starting point on purpose: the first frames go out before
 * there is anything to measure, and starting too short spends its first
 * exchanges retransmitting.
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
    uint32_t   index; /* which wire this is: 0 is `[link]`, 1 is `[link2]` */
    uint32_t   baud;  /* what info() computes the rate from                */
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

/*
 * ONE DRIVER, SEVERAL WIRES.
 *
 * A board used to have exactly one neighbour, so this was a single struct and
 * every function reached for it by name.  The shape a cluster is actually
 * wanted in is a star - one board in the middle, the others on spokes - and
 * the middle one needs a wire per spoke.  It is also the only way a third
 * board can join at all on this hardware: the CYD has three free pins in
 * total and two of them are already a wire, so the hub has to be a board with
 * pins to spare.
 *
 * From the heap, one per wire the configuration asks for, because the state is
 * thirteen kilobytes of windows and queues: a static array of four would take
 * fifty-two from every board that will only ever have one neighbour.
 *
 * Each device carries its own instance in `priv`, and every op reads it back
 * with ag_dev_priv() - so nothing in the frame assembler or the scheduler has
 * to know that more than one of them exists.
 */
#define ND_WIRES_MAX 4u

static node_t  *s_wire[ND_WIRES_MAX];
static uint32_t s_wires;

static const ag_io_api_t   *IO;
static const ag_task_api_t *TASK;

static uint32_t now_ms(void)
{
    return (uint32_t)(g_ag_api->time->us() / 1000u);
}

/* ---------------------------------------------------------------------- */
/* The wire                                                                */
/* ---------------------------------------------------------------------- */

static void wire_write(node_t *nd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t         left = len;
    while (left > 0) {
        const int32_t n = IO->uart_write(nd->uart, p, left);
        if (n <= 0) {
            return; /* the link is gone; the window will time out */
        }
        p += (size_t)n;
        left -= (size_t)n;
        nd->tx_bytes += (uint32_t)n;
    }
}

/* ---------------------------------------------------------------------- */
/* Receiving                                                               */
/* ---------------------------------------------------------------------- */

static void inbox_put(node_t *nd, uint8_t chan, const uint8_t *data,
                      uint16_t len)
{
    if (nd->in_fill >= ND_RX_QUEUE) {
        /*
         * Dropped, and counted.  The alternative is to stop acknowledging and
         * let the far end's window close, which sounds tidier and is worse: the
         * sender then blocks on a link that is only slow because nobody here is
         * reading it, and a control message behind the jam waits on a picture
         * nobody wants.  A count says the same thing without the deadlock.
         */
        nd->in_dropped++;
        return;
    }
    msg_t *m = &nd->inbox[nd->in_w];
    m->chan = chan;
    m->len = len;
    if (len > 0) {
        memcpy(m->data, data, len);
    }
    nd->in_w = (nd->in_w + 1u) % ND_RX_QUEUE;
    nd->in_fill++;
}

/* Defined below, beside the bind that sends it.  Declared here because the
 * receive path has to act on a reset before it judges a sequence. */
static void reset_channels(node_t *nd, bool announce);

static void frame_arrived(node_t *nd)
{
    const ag_nlink_hdr_t *h = &nd->cur;
    const uint8_t         cls = h->cls;

    if (cls >= NL_CLASSES) {
        nd->bad++;
        return;
    }
    if (!ag_nlink_check_psum(h, nd->body, nd->psum)) {
        /*
         * A payload that does not check out is treated exactly as a payload
         * that never came: dropped, not acknowledged, and left to the far end's
         * timer.  There is no separate "corrupt" path because there is nothing
         * different to do - and pretending otherwise would mean acknowledging
         * bytes we know are wrong.
         */
        nd->bad++;
        return;
    }
    nd->up = true;

    /* The peer's acknowledgement rides on every frame it sends. */
    if (h->ack != NL_ACK_NONE) {
        ag_nlink_tx_ack(&nd->tx[cls], h->ack, now_ms());
    }

    if ((h->flags & NL_F_RST) != 0u) {
        /*
         * The peer has started over - booted, or been bound again - and has
         * forgotten both what it sent us and what it had of ours.  Acted on
         * BEFORE the sequence check below, because a receiver waiting for
         * sequence zero rejects everything, this frame included, and a reset
         * that has to arrive in order is not a reset.
         *
         * Not announced back: both ends are now at the beginning, and a reply
         * would only give the two of them something to answer for ever.
         */
        reset_channels(nd, false);
        ag_log(AG_LOG_INFO, "nodelink", "the other end started over");
        return;
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
        inbox_put(nd, h->chan, nd->body, h->len);
        return;
    }
    if (ag_nlink_rx_accept(&nd->rx[cls], h)) {
        inbox_put(nd, h->chan, nd->body, h->len);
    }
}

static void feed(node_t *nd, uint8_t b)
{
    switch (nd->sm) {
    case SM_SYNC:
        nd->win[0] = nd->win[1];
        nd->win[1] = nd->win[2];
        nd->win[2] = nd->win[3];
        nd->win[3] = b;
        if (nd->win[0] == 'N' && nd->win[1] == 'L' && nd->win[2] == 'N' &&
            nd->win[3] == 'K') {
            memcpy(nd->hdr, nd->win, 4);
            nd->hlen = 4;
            nd->sm = SM_HDR;
        }
        break;

    case SM_HDR:
        nd->hdr[nd->hlen++] = b;
        if (nd->hlen < NL_HDR_SIZE) {
            break;
        }
        if (!ag_nlink_unpack(&nd->cur, nd->hdr)) {
            /* The header failed its own CRC, so `len` is not a number to
             * believe - resync from the next magic rather than skipping
             * however many bytes a corrupt field claims. */
            nd->bad++;
            nd->sm = SM_SYNC;
            break;
        }
        nd->blen = 0;
        nd->plen = 0;
        nd->sm = (nd->cur.len > 0) ? SM_BODY
                  : ((nd->cur.flags & NL_F_PSUM) ? SM_PSUM : SM_SYNC);
        if (nd->sm == SM_SYNC) {
            frame_arrived(nd);
        }
        break;

    case SM_BODY:
        nd->body[nd->blen++] = b;
        if (nd->blen == nd->cur.len) {
            if ((nd->cur.flags & NL_F_PSUM) != 0) {
                nd->sm = SM_PSUM;
            } else {
                frame_arrived(nd);
                nd->sm = SM_SYNC;
            }
        }
        break;

    case SM_PSUM:
        nd->psum[nd->plen++] = b;
        if (nd->plen == NL_PSUM_SIZE) {
            frame_arrived(nd);
            nd->sm = SM_SYNC;
        }
        break;

    default:
        nd->sm = SM_SYNC;
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* Sending                                                                 */
/* ---------------------------------------------------------------------- */

/* The acknowledgement to put on the next frame of this class. */
static uint16_t ack_for(node_t *nd, uint8_t cls)
{
    return (cls < NL_CLASSES) ? ag_nlink_rx_ack(&nd->rx[cls]) : NL_ACK_NONE;
}

static void emit(node_t *nd, uint8_t cls, uint8_t chan, uint16_t seq,
                 uint8_t flags,
                 const uint8_t *body, uint16_t len)
{
    ag_nlink_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.cls = cls;
    h.chan = chan;
    h.seq = seq;
    h.flags = flags;
    h.ack = ack_for(nd, cls);
    /* The credit field is reserved: this driver's back-pressure is the window
     * itself, and a second mechanism that says the same thing differently is a
     * second mechanism to get wrong. */
    h.credit = 0;
    h.len = len;

    /* Whatever we are sending, it carries the current acknowledgement - so
     * record what the far end has now been told. */
    if (cls < NL_CLASSES) {
        nd->acked[cls] = h.ack;
        nd->acked_any[cls] = (h.ack != NL_ACK_NONE);
    }

    uint8_t hdr[NL_HDR_SIZE];
    ag_nlink_pack(hdr, &h);
    wire_write(nd, hdr, NL_HDR_SIZE);
    if (len > 0) {
        wire_write(nd, body, len);
    }
    if ((flags & NL_F_PSUM) != 0) {
        uint8_t ps[NL_PSUM_SIZE];
        ag_nlink_put_psum(ps, body, len);
        wire_write(nd, ps, NL_PSUM_SIZE);
    }
}

/* Tell the scheduler what is queued, so its answer is about now. */
static void offer_all(node_t *nd, uint32_t t)
{
    for (int c = 0; c < NL_CLASSES; c++) {
        if (c == NL_CLS_STREAM) {
            const uint32_t head =
                (nd->stream_fill > 0)
                    ? (uint32_t)nd->stream[nd->stream_r].len + NL_HDR_SIZE
                    : 0u;
            ag_nlink_sched_offer(&nd->sched, c,
                                 nd->stream_fill * (NL_QUANTUM + NL_HDR_SIZE),
                                 head);
        } else {
            ag_nlink_sched_offer(&nd->sched, c,
                                 ag_nlink_tx_waiting_bytes(&nd->tx[c], t),
                                 ag_nlink_tx_head_len(&nd->tx[c], t));
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
static void turn(node_t *nd)
{
    uint8_t buf[128];
    for (;;) {
        const int32_t n = IO->uart_read(nd->uart, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        nd->rx_bytes += (uint32_t)n;
        for (int32_t i = 0; i < n; i++) {
            feed(nd, buf[i]);
        }
        if ((uint32_t)n < sizeof(buf)) {
            break;
        }
    }

    const uint32_t t = now_ms();
    if ((uint32_t)(t - nd->period_at) >= ND_PERIOD_MS) {
        ag_nlink_sched_tick(&nd->sched);
        nd->period_at = t;
    }

    offer_all(nd, t);
    const int cls = ag_nlink_sched_pick(&nd->sched);
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
            const uint16_t now_ack = ag_nlink_rx_ack(&nd->rx[c]);
            if (now_ack == NL_ACK_NONE) {
                continue;
            }
            if (nd->acked_any[c] && nd->acked[c] == now_ack) {
                continue; /* already told them */
            }
            emit(nd, (uint8_t)c, 0, 0, NL_F_ACKONLY, NULL, 0);
            nd->acks++;
            return; /* one frame per turn, here as everywhere */
        }
        return;
    }

    if (cls == NL_CLS_STREAM) {
        const msg_t *m = &nd->stream[nd->stream_r];
        emit(nd, NL_CLS_STREAM, m->chan, nd->stream_seq++, NL_F_EOM, m->data,
             m->len);
        nd->stream_r = (nd->stream_r + 1u) % ND_STREAM_QUEUE;
        nd->stream_fill--;
        ag_nlink_sched_sent(&nd->sched, cls, (uint32_t)m->len + NL_HDR_SIZE);
        return;
    }

    const ag_nlink_slot_t *s = ag_nlink_tx_next(&nd->tx[cls], t);
    if (s == NULL) {
        return;
    }
    emit(nd, (uint8_t)cls, s->chan, s->seq, s->flags, s->data, s->len);
    ag_nlink_sched_sent(&nd->sched, cls,
                        (uint32_t)s->len + NL_HDR_SIZE + NL_PSUM_SIZE);
}

/* ---------------------------------------------------------------------- */
/* The class vtable                                                        */
/* ---------------------------------------------------------------------- */

/*
 * Both halves of every sequenced class, back to the beginning.
 *
 * `announce` tells the far end to do the same.  Reset is symmetric on purpose:
 * "forget what you have of this channel" is worthless one-sided, because the
 * peer's next frame would still carry a sequence our receiver has never seen
 * the start of.
 */
static void reset_channels(node_t *nd, bool announce)
{
    ag_nlink_tx_init(&nd->tx[NL_CLS_CONTROL], NL_CLS_CONTROL, nd->slot_ctl,
                     ND_WIN_CONTROL, ND_RTO_MS);
    ag_nlink_tx_init(&nd->tx[NL_CLS_BULK], NL_CLS_BULK, nd->slot_bulk,
                     ND_WIN_BULK, ND_RTO_MS);
    for (int c = 0; c < NL_CLASSES; c++) {
        ag_nlink_rx_init(&nd->rx[c]);
    }
    if (announce) {
        /* Sequence zero on the frame itself, and it does not matter: the far
         * end acts on the flag BEFORE it looks at the sequence, which is the
         * only way a reset can reach a receiver that is refusing everything. */
        emit(nd, NL_CLS_CONTROL, 0, 0, NL_F_RST, NULL, 0);
        emit(nd, NL_CLS_BULK, 0, 0, NL_F_RST, NULL, 0);
    }
}

/*
 * Binding the wire.
 *
 * THE RESET IS THE POINT, and it is what a year of testing in an emulator
 * could not have asked for.  A receiver accepts sequence zero first and
 * nothing else - deliberately, so that a lost first frame cannot be silently
 * skipped - and two boards on a bench do not start together: one has been
 * running for a minute, has sent frames 0..N into a wire nobody was listening
 * to, and never sends zero again.  The other boots, expects zero, and counts
 * every frame that arrives as a gap.  For ever.
 *
 * What that looks like from the outside is exactly what it looked like here:
 * bytes flowing both ways, every checksum good, ZERO frames received, zero
 * bad, and a peer that "is not saying yet".  It reads as a bad wire, and two
 * hours went on the wire before the counters were believed.
 *
 * In the emulator both guests are started by one script and come up together,
 * so both begin at zero and this never happens.  The flag for it - NL_F_RST -
 * has been in the header the whole time, named in a comment as the way a link
 * is resynchronised, and nothing sent it or acted on it.  A line in a header
 * is not a mechanism.
 *
 * Whoever binds LAST fixes the pair, which is why this needs no retry: a board
 * that reboots binds again and its reset reaches a peer that never left.
 */
static ag_err_t nd_start(ag_device_t *dev)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return -AG_ENODEV;
    }
    TASK->mutex_lock(nd->lock, UINT32_MAX);
    reset_channels(nd, true);
    turn(nd);
    TASK->mutex_unlock(nd->lock);
    return AG_OK;
}

static bool nd_up(ag_device_t *dev)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return false;
    }
    TASK->mutex_lock(nd->lock, UINT32_MAX);
    turn(nd);
    const bool r = nd->up;
    TASK->mutex_unlock(nd->lock);
    return r;
}

static int32_t nd_send(ag_device_t *dev, uint8_t chan, uint8_t cls,
                       const void *buf, size_t len)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return -AG_ENODEV;
    }
    if (buf == NULL || cls >= NL_CLASSES) {
        return -AG_EINVAL;
    }
    if (len > NL_QUANTUM) {
        /* Refused rather than split: only the caller knows where its message
         * boundaries are, and a driver that guessed would deliver two halves
         * that look like two messages. */
        return -AG_ERANGE;
    }

    TASK->mutex_lock(nd->lock, UINT32_MAX);
    int32_t r;
    if (cls == NL_CLS_STREAM) {
        if (nd->stream_fill >= ND_STREAM_QUEUE) {
            /*
             * The stream class drops rather than blocks, here as on the wire.
             * A caller told -AG_EAGAIN would retry, and a retried audio frame
             * is the thing this class exists to not do.
             */
            nd->stream_r = (nd->stream_r + 1u) % ND_STREAM_QUEUE;
            nd->stream_fill--;
        }
        msg_t *m = &nd->stream[nd->stream_w];
        m->chan = chan;
        m->len = (uint16_t)len;
        memcpy(m->data, buf, len);
        nd->stream_w = (nd->stream_w + 1u) % ND_STREAM_QUEUE;
        nd->stream_fill++;
        r = (int32_t)len;
    } else if (ag_nlink_tx_queue(&nd->tx[cls], chan, buf, (uint16_t)len,
                                 true)) {
        r = (int32_t)len;
    } else {
        r = -AG_EAGAIN; /* the window is full: back-pressure, not an error */
    }
    turn(nd);
    TASK->mutex_unlock(nd->lock);
    return r;
}

static int32_t nd_recv(ag_device_t *dev, uint8_t *chan, void *buf, size_t max,
                       uint32_t timeout_ms)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return -AG_ENODEV;
    }
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
        TASK->mutex_lock(nd->lock, UINT32_MAX);
        turn(nd);
        if (nd->in_fill > 0) {
            const msg_t   *m = &nd->inbox[nd->in_r];
            const uint16_t n = (m->len < max) ? m->len : (uint16_t)max;
            memcpy(buf, m->data, n);
            *chan = m->chan;
            nd->in_r = (nd->in_r + 1u) % ND_RX_QUEUE;
            nd->in_fill--;
            TASK->mutex_unlock(nd->lock);
            return (int32_t)n;
        }
        TASK->mutex_unlock(nd->lock);

        if ((int32_t)(now_ms() - deadline) >= 0) {
            return -AG_EAGAIN;
        }
        TASK->sleep_ms(1);
    }
}

static void nd_poll(ag_device_t *dev, uint32_t ms)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return;
    }
    const uint32_t deadline = now_ms() + ms;
    for (;;) {
        TASK->mutex_lock(nd->lock, UINT32_MAX);
        turn(nd);
        TASK->mutex_unlock(nd->lock);
        if ((int32_t)(now_ms() - deadline) >= 0) {
            return;
        }
        TASK->sleep_ms(1);
    }
}

static ag_err_t nd_stats(ag_device_t *dev, ag_link_stats_t *out)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return -AG_ENODEV;
    }
    if (out == NULL) {
        return -AG_EINVAL;
    }
    TASK->mutex_lock(nd->lock, UINT32_MAX);
    memset(out, 0, sizeof(*out));
    for (int c = 0; c < NL_CLASSES; c++) {
        out->sent += nd->tx[c].sent;
        out->retransmits += nd->tx[c].retransmits;
        out->received += nd->rx[c].taken;
        out->gaps += nd->rx[c].gaps;
        out->dups += nd->rx[c].dups;
    }
    out->sent += nd->sched.sent[NL_CLS_STREAM];
    out->bad = nd->bad;
    out->acks = nd->acks;
    out->tx_bytes = nd->tx_bytes;
    out->rx_bytes = nd->rx_bytes;
    /*
     * CONTROL's, not a sum or an average: the three classes have separate
     * windows and separate timers, and an average of three timeouts is a
     * number no timer ever used.  Control is the one with a latency budget,
     * so it is the one worth watching.
     */
    out->rto_ms = nd->tx[NL_CLS_CONTROL].rto_ms;
    out->srtt_ms = nd->tx[NL_CLS_CONTROL].srtt_ms;
    TASK->mutex_unlock(nd->lock);
    return AG_OK;
}

/* ---------------------------------------------------------------------- */
/* The task that listens when nobody is asking                            */
/* ---------------------------------------------------------------------- */

/* One listener per wire: a task that pumped two of them would make each wait
 * for the other's lock, and the wire that waits is the one that drops. */
static ag_svc_t s_svc[ND_WIRES_MAX];

static void pump_task(void *arg)
{
    node_t *nd = (node_t *)arg;
    if (nd == NULL) {
        return;
    }
    /*
     * WAIT FOR OUR OWN HANDLE BEFORE ASKING WHETHER WE SHOULD STOP.
     *
     * svc_start returns the handle that svc_stopping is asked about, and this
     * task is running before it has returned - so the first question was put
     * about a handle that was still NULL, and a task that answers "yes, stop"
     * to that never listens at all.
     *
     * Which is exactly the shape of the fault it caused: with no listener,
     * nothing reads the wire between commands, and the class that suffers is
     * the UNRELIABLE one.  Control is retransmitted and survives a missed
     * read; stream is not acknowledged and not repeated, so a frame nobody
     * collected is gone.  "Control 24 of 24, stream silent" reads like a
     * broken class and is a missing reader.
     */
    while (s_svc[nd->index] == NULL) {
        TASK->sleep_ms(1);
    }
    /*
     * Short sleeps rather than a blocking read, because the read has to happen
     * under the same mutex everything else takes, and a task that slept inside
     * the lock would stop the shell from sending.  Ten milliseconds is a tick;
     * asking for less does not get less.
     */
    while (!ag_api()->task->svc_stopping(s_svc[nd->index])) {
        TASK->mutex_lock(nd->lock, UINT32_MAX);
        turn(nd);
        TASK->mutex_unlock(nd->lock);
        TASK->sleep_ms(1);
    }
    /* Said out loud.  A listener that ends while the board is still running
     * leaves a wire that works for whoever asks and drops everything nobody
     * asked for, which is a fault three layers from where it is felt. */
    ag_log(AG_LOG_WARN, "nodelink", "uart%d: the listener has stopped",
           nd->uart);
}

/*
 * What this wire is, for the layers that have to size themselves to it.
 *
 * The rate is computed from the baud actually configured rather than named as a
 * constant, because the baud is a config line and a constant would be a lie the
 * moment anyone changed it.  Ten bits a byte on the line (a start and a stop
 * bit), then the two corrections that stop it being a brochure number:
 *
 *   - framing.  A frame carries NL_QUANTUM of payload under NL_HDR_SIZE of
 *     header, so the useful fraction is that ratio and not one.
 *   - the line.  Measured between two boards: clean to 5 Mbaud, and at 16
 *     Mbaud 24.7% of frames arrive corrupt and are sent again, which leaves
 *     1 132 730 B/s of payload out of 1 599 966 on the line.  So above 5
 *     Mbaud three quarters of it is what a caller can actually plan against.
 *
 * rtt_us is left at zero: the kernel has a measured round trip in the stats
 * above, and a driver's guess would only be used before any traffic has flowed.
 */


static ag_err_t nd_info(ag_device_t *dev, ag_link_info_t *out)
{
    node_t *nd = (node_t *)ag_dev_priv(dev);
    if (nd == NULL) {
        return -AG_ENODEV;
    }
    if (out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->mtu = NL_QUANTUM - 8u; /* room for a caller's own small header */

    uint32_t bytes = nd->baud / 10u;
    bytes = (uint32_t)((uint64_t)bytes * NL_QUANTUM /
                       (NL_QUANTUM + NL_HDR_SIZE));
    if (nd->baud > 5000000u) {
        bytes = (uint32_t)((uint64_t)bytes * 753u / 1000u);
    }
    out->bytes_per_sec = bytes;
    out->rtt_us = 0;
    ag_strlcpy(out->wire, "uart", sizeof(out->wire));
    return AG_OK;
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
    .info = nd_info,
};

/* ---------------------------------------------------------------------- */
/* Lifecycle                                                               */
/* ---------------------------------------------------------------------- */

/*
 * One wire, from one section of the configuration.
 *
 *   [link]  uart = 1   baud = 921600      -> /dev/node0
 *   [link2] uart = 2   baud = 921600      -> /dev/node1
 *
 * `[link]` with no number is the first, because every board that has one wire
 * already spells it that way and a rename would be a flag day for nothing.
 * The numbered ones exist only on a board in the middle of a star.
 */
/* Spelled out rather than formatted: a .SYS has no stdio, the set is four, and
 * a table cannot get the order wrong the way a loop building strings can. */
static const char *const k_dev_name[ND_WIRES_MAX] = {"node0", "node1", "node2",
                                                     "node3"};
static const char *const k_uart_key[ND_WIRES_MAX] = {
    "link.uart", "link2.uart", "link3.uart", "link4.uart"};
static const char *const k_baud_key[ND_WIRES_MAX] = {
    "link.baud", "link2.baud", "link3.baud", "link4.baud"};

static ag_err_t wire_up(uint32_t i)
{
    const int dflt = (i == 0) ? ND_UART_DEFAULT : -1;
    const int uart = AG_HAS(ag_api()->cfg, get_int)
                         ? ag_api()->cfg->get_int(k_uart_key[i], dflt)
                         : dflt;
    if (uart < 0) {
        return -AG_ENOENT; /* nobody asked for this one */
    }

    /*
     * Outside internal SRAM when there is anywhere else: thirteen kilobytes of
     * windows and queues, read and written by one task, is exactly what the
     * slow bus is for - and on the CYD the internal side is measured in
     * thousands of bytes.
     */
    node_t *nd = (node_t *)ag_malloc_caps(sizeof(node_t), AG_MEM_DEFAULT);
    if (nd == NULL) {
        nd = (node_t *)ag_malloc_caps(sizeof(node_t), AG_MEM_FAST);
    }
    if (nd == NULL) {
        return -AG_ENOMEM;
    }

    memset(nd, 0, sizeof(*nd));
    nd->index = i;
    nd->sm = SM_SYNC;
    nd->uart = uart;
    uint32_t baud = ND_BAUD;
    if (AG_HAS(ag_api()->cfg, get_int)) {
        baud = (uint32_t)ag_api()->cfg->get_int(k_baud_key[i],
                                                (int32_t)ND_BAUD);
    }
    nd->baud = baud; /* what info() computes the rate from */

    nd->lock = TASK->mutex_create();
    if (nd->lock == NULL) {
        return -AG_ENOMEM;
    }

    ag_nlink_tx_init(&nd->tx[NL_CLS_CONTROL], NL_CLS_CONTROL, nd->slot_ctl,
                     ND_WIN_CONTROL, ND_RTO_MS);
    ag_nlink_tx_init(&nd->tx[NL_CLS_BULK], NL_CLS_BULK, nd->slot_bulk,
                     ND_WIN_BULK, ND_RTO_MS);
    /* The stream class has no window and no slots; its tx is left zeroed so
     * that anything reading its counters gets zeros rather than rubbish. */
    for (int c = 0; c < NL_CLASSES; c++) {
        ag_nlink_rx_init(&nd->rx[c]);
    }
    ag_nlink_sched_init(&nd->sched, ND_RESERVE_BYTES);
    nd->period_at = now_ms();

    if (IO->uart_config != NULL) {
        const ag_err_t ue = IO->uart_config(nd->uart, baud, 8, 0, 1);
        if (ue != AG_OK) {
            ag_log(AG_LOG_WARN, "nodelink",
                   "uart%d did not come up: %d (set uart%d.tx/rx in BOARD.CFG)",
                   nd->uart, (int)ue, nd->uart);
        }
    }

    const char *const name = k_dev_name[i];
    const ag_dev_add_t desc = {
        .name = name,
        .driver = "NODELINK",
        .cls = AG_DEV_LINK,
        .flags = 0,
        .ops = &k_dev_ops,        /* the registry requires one; empty = ENOTSUP */
        .class_ops = &k_link_ops, /* the vtable the kernel reads                */
        .priv = nd,
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
        s_svc[i] = ag_api()->task->svc_start(pump_task, nd, "nodelink", 4096, 0);
    }
    if (s_svc[i] == NULL) {
        ag_log(AG_LOG_WARN, "nodelink",
               "no service task: this node answers only while asked");
    }

    s_wire[i] = nd;
    s_wires++;
    ag_log(AG_LOG_INFO, "nodelink",
           "/dev/%s on uart%d at %u baud%s - bind with `link use %s`", name,
           nd->uart, (unsigned)baud, (s_svc[i] != NULL) ? ", listening" : "",
           name);
    return AG_OK;
}

ag_err_t ag_driver_init(void)
{
    IO = ag_api()->io;
    TASK = ag_api()->task;
    if (IO == NULL || TASK == NULL || !AG_HAS(ag_api()->dev, add) ||
        !AG_HAS(IO, uart_read) || !AG_HAS(TASK, mutex_create)) {
        return -AG_ENOSYS;
    }

    /*
     * `[link]`, then `[link2]`, `[link3]`, `[link4]` for as long as they are
     * there.  Stopping at the first absent one rather than checking all four:
     * a board with a gap in that numbering has a typo, and carrying on past it
     * would bind the wires it found and say nothing about the one it did not.
     */
    for (uint32_t i = 0; i < ND_WIRES_MAX; i++) {
        const ag_err_t e = wire_up(i);
        if (e == -AG_ENOENT) {
            break;
        }
        if (e != AG_OK) {
            ag_log(AG_LOG_ERROR, "nodelink", "%s: %d", k_uart_key[i], (int)e);
            break;
        }
    }
    if (s_wires == 0) {
        return -AG_ENODEV;
    }
    if (s_wires > 1) {
        ag_log(AG_LOG_INFO, "nodelink",
               "%u wires: this board is the middle of the cluster",
               (unsigned)s_wires);
    }
    return AG_OK;
}
