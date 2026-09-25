/*
 * ArgonOS - the framed link over a UART: how net/wlink's frames cross a
 * serial line.
 *
 * THE BYTES ARE THE WIRE'S, THE FRAMES ARE FOUND IN THEM.  A UART has no
 * transactions: it carries bytes, and a frame is wherever its header says
 * it is.  So the receiving task reads what the DMA has taken in, looks for
 * a header (0xA5 ? 0x5A), takes the frame's length from it, waits for the
 * rest, and checks the checksum over the whole frame.  Anything that is not
 * a frame - a byte lost to an overrun, a frame cut short, noise - costs a
 * one-byte step and a look for the next header; the stream's own offsets and
 * acknowledgements then have the lost frame sent again.
 *
 * A FRAME IS THE LENGTH IT IS.  The header and what it carries, no padding:
 * on SPI the transaction is the frame and its length is fixed, here an
 * acknowledgement is 24 bytes and not a frame's worth of zeros.  Both ends are
 * equal - there is no clock to hold - so each sends a frame when it has
 * something to say:
 *
 *   - data, and room for it at the other end;
 *   - a frame to send again, or a hole of its own to ask about;
 *   - an acknowledgement owed: UL_ACK_FRAMES frames' worth taken in since it
 *     last said, or UL_ACK_US since it took in any;
 *   - room made (its reader has taken as much since it last said, or has
 *     emptied the ring);
 *   - nothing for UL_KEEP_US - which is also how two ends that have not yet
 *     heard each other find out, at UL_HELLO_US.
 *
 * THE OTHER END'S ROOM IS EXACT HERE.  A frame carries both its sender's
 * acknowledgement and its room, reckoned at the same moment, so the other
 * end will take every byte up to ack + room and nothing past it: the
 * allowance is that edge less the next byte to send, with no guessing about
 * what was on its way.  (The SPI master has to guess, because the slave's
 * frames are built two transactions before they are clocked.)
 *
 * Two tasks, at the SPI link's priority: one receiving (it blocks in the
 * read), one sending (it blocks on the protocol's wake).
 *
 * BY DMA OR BY THE DRIVER, and the wire's configuration says which
 * (AG_UARTLINK_DMA).  By DMA (UHCI - the S3, the C3, the C6) two transmit
 * buffers go round: frames finish in the order they were given, so settling
 * the DMA to one out gives the older back; 16 Mbaud needs this.  By the
 * driver - every chip, the ESP32 that has no UHCI and the emulator that has
 * none either - a frame is copied into the driver's ring and read out of
 * its interrupt's, which at the cluster's 460 800 and 921 600 baud costs
 * nothing that matters.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/uartlink.h>

#include <string.h>

#include <argon/kernel.h>
#include <argon/log.h>

#include <argon/port/mem.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/time.h>
#include <argon/port/uart.h>

#include "core/sysconfig.h"
#include "net/wlink.h"

#define UL_PORTS     3
/* two: one on the wire while the next is built - queued behind it, so the
 * line does not go quiet between them */
#define UL_TXBUF     2u
/* what one DMA read takes at most; the frame finder holds a frame and this */
#define UL_READ      1024u
#define UL_READ_MS   20u
/* the sending task looks at the world at least this often */
#define UL_TICK_MS   10u
#define UL_HELLO_US  20000
#define UL_KEEP_US   100000
/*
 * An acknowledgement alone goes when UL_ACK_FRAMES frames' worth have come in
 * since the last one said, or UL_ACK_US after any came in, whichever is
 * first; room made goes at the same UL_ACK_FRAMES frames.  The receive ring
 * holds three, so the sender never runs dry waiting.
 *
 * NOT FOR EVERY FRAME, TWICE OVER.  One 24-byte frame back for each one
 * received was ~1 800 a second from the C3, each a piece of its own in the
 * Zero's receive queue, and the queue threw some away.  And at 16 Mbaud every
 * frame a board SENDS while it receives costs it bits: on the Zero-C3 pair
 * the C3's receive lost single 1s between 0s (0xB5 read as 0xB1, the frame
 * the right length and wrong) - 150 to 180 frames in five seconds with an
 * answer for each frame or half-frame of room, 45 with one for every two,
 * none at 8 or 10 Mbaud, none with the C3 silent.  92% of the wire became 97%
 * (25 Sep 2026).
 */
#define UL_ACK_FRAMES 2u
#define UL_ACK_US    2000
/*
 * Fewer reports make a lost one dearer: a sender its last report held back
 * sends nothing more, so nothing tells the receiver its answer went missing
 * - and the C3's receive turns bits in the Zero's acknowledgements too (27
 * to 72 in five seconds of C3->Z).  So a receiver the line has gone quiet on
 * says it again, UL_ACK_US apart, this many times.
 */
#define UL_RETELLS   3u
#define UL_PRIO      9u
#define UL_STACK     3072u
#define UL_STOP_MS   500u

typedef struct {
    wlink_t w;
    int     port;
    bool    dma; /* by the UART's DMA, or through its driver */
    /* two allocations: the transmit buffers, which the DMA reads and so must
     * be in memory it can reach, and the frame finder's buffer with the two
     * rings, which can be anywhere - on the Zero, PSRAM; as one 42 KB piece
     * of internal memory it did not fit beside the port's receive rings */
    uint8_t *dmem;
    uint8_t *mem;
    uint8_t *txf[UL_TXBUF];
    unsigned txk;
    uint8_t *pbuf;
    uint32_t pcap;
    uint32_t have;

    /* what the last frame out said, so the next is sent only when there is
     * something new to say */
    uint32_t ack_sent;
    uint32_t space_sent;
    int64_t  last_tx;
    /* when the other last sent anything new, and the reports since */
    uint32_t rx_seen;
    int64_t  rx_seen_at;
    uint32_t retells;

    volatile bool  stop;
    ag_port_task_t rx_task;
    ag_port_task_t tx_task;
    ag_port_sem_t  exit_sem;

    uint32_t frames_tx;
    uint32_t frames_rx;
    uint32_t skipped;

    /*
     * What a damaged frame WAS, told by where the next good one began: nearer
     * than its length said - bytes fell out of the line; exactly its length -
     * the bytes are all there and some are wrong.  `abs` counts every byte
     * the parse buffer has let go of, so a place in it is a place in the line.
     */
    uint32_t abs;
    uint32_t bad_at;
    uint32_t bad_len;
    bool     bad_open;
    uint32_t dmg_short;   /* frames bytes fell out of */
    uint32_t dmg_missing; /* ... and how many bytes, all told */
    uint32_t dmg_inplace; /* frames the right length and wrong */
    uint32_t errors;
    ag_err_t last_error;
} uartlink_t;

/*
 * On the heap, made by the first open of a port and kept (the lock and the
 * wakes in it have to outlive a close), and not three of them in the
 * kernel's statics: a few hundred bytes each, and the S3's emulator build
 * then overflowed its DRAM by 2 816 - for ports most boards never open.
 */
static uartlink_t *s_ul[UL_PORTS];

static void lock(uartlink_t *u) { (void)ag_port_mutex_take(u->w.lock, AG_PORT_FOREVER); }

static void unlock(uartlink_t *u) { (void)ag_port_mutex_give(u->w.lock); }

static ag_port_ticks_t ticks_at_least(uint32_t ms)
{
    const ag_port_ticks_t t = ag_port_ms_to_ticks(ms);
    return (t == 0u) ? 1u : t;
}

static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

static uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

static void note_error(uartlink_t *u, ag_err_t e)
{
    lock(u);
    if (u->errors == 0u) {
        u->last_error = e;
    }
    u->errors++;
    unlock(u);
}

static uartlink_t *ul_of(int port)
{
    return (port >= 0 && port < UL_PORTS) ? s_ul[port] : NULL;
}

static uartlink_t *ul_make(int port)
{
    if (port < 0 || port >= UL_PORTS) {
        return NULL;
    }
    if (s_ul[port] == NULL) {
        uartlink_t *u = ag_port_alloc(sizeof(*u), AG_MEM_FAST | AG_MEM_BYTE);
        if (u == NULL) {
            u = ag_port_alloc(sizeof(*u), AG_MEM_BYTE);
        }
        if (u != NULL) {
            memset(u, 0, sizeof(*u));
        }
        s_ul[port] = u;
    }
    return s_ul[port];
}

/* Out of range, or a port no link was ever opened on. */
static ag_err_t ul_absent(int port)
{
    return (port >= 0 && port < UL_PORTS) ? -AG_ENODEV : -AG_ERANGE;
}

/* ---------------------------------------------------------------------- */
/* Receiving: frames found in the bytes                                   */
/* ---------------------------------------------------------------------- */

static void find_frames(uartlink_t *u)
{
    uint32_t at = 0;
    bool     any = false;
    for (;;) {
        const uint32_t left = u->have - at;
        if (left < WL_HDR) {
            break;
        }
        const uint8_t *f = u->pbuf + at;
        if (f[0] != 0xA5u || f[2] != 0x5Au) {
            at++;
            u->skipped++;
            continue;
        }
        /* to the front: the checksum reads words, and so the frame has to
         * start on one */
        if (at != 0u) {
            memmove(u->pbuf, f, left);
            u->have = left;
            u->abs += at;
            at = 0;
            f = u->pbuf;
        }
        const uint32_t len = wl_frame_len(f, left);
        if (len == 0u) {
            break; /* not all of the header yet */
        }
        if (len == UINT32_MAX || len > u->w.block) {
            at = 1;
            u->skipped++;
            continue;
        }
        if (left < len) {
            break; /* the rest of the frame is still coming */
        }
        if (wl_sum(f, len - WL_HDR) != get32(f + 20)) {
            /* not a frame after all, or a damaged one: a step, and look
             * again - its length may be as wrong as the rest of it */
            if (!u->bad_open) {
                u->bad_open = true;
                u->bad_at = u->abs;
                u->bad_len = len;
            }
            at = 1;
            u->skipped++;
            continue;
        }
        if (u->bad_open) {
            const uint32_t dist = u->abs - u->bad_at;
            if (dist < u->bad_len) {
                u->dmg_short++;
                u->dmg_missing += u->bad_len - dist;
            } else if (dist == u->bad_len) {
                u->dmg_inplace++;
            }
            u->bad_open = false;
        }
        (void)wl_rx(&u->w, f, len);
        u->frames_rx++;
        any = true;
        at = len;
    }
    if (at > 0u) {
        memmove(u->pbuf, u->pbuf + at, u->have - at);
        u->have -= at;
        u->abs += at;
    }
    if (any) {
        /* an acknowledgement is owed now, and maybe a frame sent again */
        (void)ag_port_sem_give(u->w.tx_sem);
    }
}

/* What has arrived, up to UL_READ, waiting UL_READ_MS for the first byte. */
static int32_t take_in(uartlink_t *u)
{
    uint8_t *const at = u->pbuf + u->have;
    if (u->dma) {
        return ag_port_uart_dma_read(u->port, at, UL_READ, UL_READ_MS);
    }
    /* the driver's read waits for ALL it is asked for: so what is there,
     * or one byte with the wait */
    const int32_t there = ag_port_uart_pending(u->port);
    if (there > 0) {
        return ag_port_uart_read(u->port, at,
                                 ((uint32_t)there < UL_READ) ? (uint32_t)there
                                                             : UL_READ,
                                 0u);
    }
    return ag_port_uart_read(u->port, at, 1u, UL_READ_MS);
}

/* A frame out.  By DMA the buffer is the hardware's until it is settled
 * back; through the driver it is copied and the caller's again at once. */
static int32_t put_out(uartlink_t *u, const uint8_t *f, uint32_t len)
{
    if (u->dma) {
        return ag_port_uart_dma_submit(u->port, f, len);
    }
    uint32_t done = 0;
    while (done < len) {
        const int32_t n = ag_port_uart_write(u->port, f + done, len - done);
        if (n < 0) {
            return n;
        }
        if (n == 0) {
            ag_port_task_delay(1);
        }
        done += (uint32_t)n;
    }
    return (int32_t)len;
}

static void rx_task(void *arg)
{
    uartlink_t *u = (uartlink_t *)arg;
    while (!u->stop) {
        const int32_t n = take_in(u);
        if (n <= 0) {
            continue;
        }
        u->have += (uint32_t)n;
        find_frames(u);
    }
    (void)ag_port_sem_give(u->exit_sem);
    ag_port_task_delete(NULL);
}

/* ---------------------------------------------------------------------- */
/* Sending: a frame whenever there is something new to say                */
/* ---------------------------------------------------------------------- */

static void tx_task(void *arg)
{
    uartlink_t *u = (uartlink_t *)arg;
    wlink_t    *w = &u->w;
    uint32_t    wait_ms = UL_TICK_MS;
    while (!u->stop) {
        (void)ag_port_sem_take(w->tx_sem, ticks_at_least(wait_ms));
        while (!u->stop) {
            wl_rto_check(w);
            const int64_t now = ag_port_us();
            lock(u);
            uint32_t allow = 0;
            if (w->agreed && w->peer_known) {
                const uint32_t edge = w->peer_ack + w->peer_room;
                const uint32_t next = w->tx_base + w->sent_off;
                if ((int32_t)(edge - next) > 0) {
                    allow = edge - next;
                }
            }
            const bool data = w->tx.count > w->sent_off && allow > 0u &&
                              w->infl_n < WL_WIN;
            const bool again = w->resend || w->nak_pending;
            const uint32_t pay = w->frame - WL_HDR;
            const uint32_t unacked = w->rx_next - u->ack_sent;
            const bool     ack = unacked >= UL_ACK_FRAMES * pay ||
                             (unacked > 0u && now - u->last_tx >= UL_ACK_US);
            const uint32_t space = ring_space(&w->rx);
            /* room: what the sender may still send on the last report, and
             * what a report now would add to it - told when that is
             * UL_ACK_FRAMES frames, or half of one to a sender short of a
             * frame */
            const uint32_t left =
                (u->space_sent > unacked) ? u->space_sent - unacked : 0u;
            const uint32_t gain = (space > left) ? space - left : 0u;
            const bool     roomy = gain >= UL_ACK_FRAMES * pay ||
                               (left < pay && gain >= pay / 2u);
            if (w->rx_next != u->rx_seen) {
                u->rx_seen = w->rx_next;
                u->rx_seen_at = now;
                u->retells = 0;
            }
            /* the last report lost?  A sender it held back says nothing
             * more, so it is told again - a few times, then left to the
             * keep-alive */
            const bool retell = w->peer_known && u->retells < UL_RETELLS &&
                                space >= pay &&
                                now - u->rx_seen_at >= UL_ACK_US &&
                                now - u->last_tx >= UL_ACK_US;
            wait_ms = (unacked > 0u || (w->peer_known &&
                                        u->retells < UL_RETELLS))
                          ? (uint32_t)(UL_ACK_US / 1000)
                          : UL_TICK_MS;
            unlock(u);
            const bool quiet = (now - u->last_tx) >=
                               (w->peer_known ? UL_KEEP_US : UL_HELLO_US);
            if (!data && !again && !ack && !roomy && !retell && !quiet) {
                break;
            }
            /* the older transmit buffer back: the other may still be on
             * the wire */
            if (u->dma) {
                const ag_err_t e =
                    ag_port_uart_dma_settle(u->port, UL_TXBUF - 1u);
                if (e != AG_OK) {
                    note_error(u, e);
                    break;
                }
            }
            uint8_t *const f = u->txf[u->txk];
            bool           nak_was = false;
            uint32_t       wire = 0;
            const uint32_t n = wl_build(w, f, allow, 0u, &nak_was, &wire);
            const int32_t  rc = put_out(u, f, wire);
            if (rc < 0) {
                wl_unbuild(w, n, nak_was);
                note_error(u, (ag_err_t)rc);
                ag_port_task_delay(1);
                break;
            }
            u->ack_sent = get32(f + 8);
            u->space_sent = space;
            u->last_tx = now;
            if (retell) {
                u->retells++;
            }
            u->txk = u->dma ? (u->txk + 1u) % UL_TXBUF : 0u;
            u->frames_tx++;
        }
    }
    if (u->dma) {
        (void)ag_port_uart_dma_drain(u->port);
    }
    (void)ag_port_sem_give(u->exit_sem);
    ag_port_task_delete(NULL);
}

/* ---------------------------------------------------------------------- */
/* Open and close                                                          */
/* ---------------------------------------------------------------------- */

uint32_t ag_uartlink_frame(uint32_t frame)
{
    if (frame == 0u) {
        /* the board's own: how much memory a link may cost is its business */
        const int32_t v =
            ag_cfg_get_int(ag_sysconfig(), "wire.frame", (int32_t)4096);
        frame = (v > 0) ? (uint32_t)v : 4096u;
    }
    return (frame > WL_FRAME_MAX) ? WL_FRAME_MAX : frame;
}

ag_err_t ag_uartlink_open(int port, uint32_t frame, uint32_t flags)
{
    if (port < 0 || port >= UL_PORTS) {
        return -AG_ERANGE;
    }
    uartlink_t *const u = ul_make(port);
    if (u == NULL) {
        return -AG_ENOMEM;
    }
    wlink_t *const w = &u->w;
    if (w->lock == NULL) {
        w->lock = ag_port_mutex_new();
    }
    if (w->rx_sem == NULL) {
        w->rx_sem = ag_port_sem_new_counting(255u, 0u);
    }
    if (w->space_sem == NULL) {
        w->space_sem = ag_port_sem_new_binary();
    }
    if (w->tx_sem == NULL) {
        w->tx_sem = ag_port_sem_new_binary();
    }
    if (u->exit_sem == NULL) {
        u->exit_sem = ag_port_sem_new_counting(2u, 0u);
    }
    if (w->lock == NULL || w->rx_sem == NULL || w->space_sem == NULL ||
        w->tx_sem == NULL || u->exit_sem == NULL) {
        return -AG_ENOMEM;
    }
    frame = ag_uartlink_frame(frame);
    if (frame < WL_HELLO || frame % WL_UNIT != 0u) {
        return -AG_EINVAL;
    }

    /* the port's receive rings first, which it keeps: allocated after this
     * link's memory they sat in the middle of its hole when it was freed,
     * and the next open on a C3 found no 21 KB in one piece */
    const bool dma = (flags & AG_UARTLINK_DMA) != 0u;
    if (dma) {
        ag_err_t le = -AG_ENOTSUP;
        if (flags & AG_UARTLINK_RING) {
            /* what the other end may have in flight is our room, three
             * frames: four, and the reader's moment on top */
            le = ag_port_uart_dma_listen_ring(port, 4u * frame);
            if (le == -AG_ENOTSUP) {
                /* not on this chip (the S3): the DMA's receive as it is */
                ag_log(AG_LOG_WARN, "uartlink",
                       "uart%d: no never-stopped receive here; plain DMA",
                       port);
            }
        }
        if (le == -AG_ENOTSUP) {
            le = ag_port_uart_dma_listen(port);
        }
        if (le != AG_OK) {
            return le;
        }
    }

    lock(u);
    if (w->open) {
        unlock(u);
        return -AG_EBUSY;
    }
    const uint32_t pay = frame - WL_HDR;
    w->txcap = (4u * pay + 3u) & ~3u;
    w->rxcap = (3u * pay + 3u) & ~3u;
    u->pcap = frame + UL_READ;
    const size_t dbytes = (size_t)(dma ? UL_TXBUF : 1u) * frame;
    const size_t bytes = (size_t)u->pcap + w->txcap + w->rxcap;
    u->dmem = ag_port_alloc_aligned(4u, dbytes, AG_MEM_DMA);
    u->mem = (u->dmem != NULL) ? ag_port_alloc(bytes, AG_MEM_BYTE) : NULL;
    if (u->mem == NULL) {
        ag_port_free(u->dmem);
        u->dmem = NULL;
        unlock(u);
        ag_log(AG_LOG_WARN, "uartlink",
               "uart%d: no %u + %u bytes for the link", port,
               (unsigned)dbytes, (unsigned)bytes);
        return -AG_ENOMEM;
    }
    for (unsigned k = 0; k < UL_TXBUF; k++) {
        u->txf[k] = u->dmem + (dma ? (size_t)k * frame : 0u);
    }
    uint8_t *p = u->mem;
    u->pbuf = p;
    p += u->pcap;
    w->tx = (ring_t){.mem = p, .cap = w->txcap};
    p += w->txcap;
    w->rx = (ring_t){.mem = p, .cap = w->rxcap};

    w->framed = true;
    w->master = false;
    /* equal ends: no roles to check, the frame agreed on the first header
     * heard, and the sending task woken by writers and by readers alike */
    w->role_check = false;
    w->agree_by_len = false;
    w->kick_writes = true;
    w->kick_reads = true;
    wl_reset(w, frame, flags);
    u->port = port;
    u->dma = dma;
    u->txk = 0;
    u->have = 0;
    u->ack_sent = 0;
    u->space_sent = 0;
    u->last_tx = 0;
    u->stop = false;
    u->frames_tx = 0;
    u->frames_rx = 0;
    u->skipped = 0;
    u->rx_seen = 0;
    u->rx_seen_at = 0;
    u->retells = 0;
    u->abs = 0;
    u->bad_open = false;
    u->dmg_short = 0;
    u->dmg_missing = 0;
    u->dmg_inplace = 0;
    u->errors = 0;
    u->last_error = AG_OK;
    w->open = true;
    unlock(u);

    const int core = (int)ag_sysinfo()->app_core;
    const bool ok_rx = ag_port_task_create(rx_task, "uartlink", UL_STACK, u,
                                           UL_PRIO, core, 0, &u->rx_task);
    const bool ok_tx = ok_rx && ag_port_task_create(tx_task, "uartltx",
                                                    UL_STACK, u, UL_PRIO, core,
                                                    0, &u->tx_task);
    if (!ok_tx) {
        u->stop = true;
        if (ok_rx) {
            (void)ag_port_sem_take(u->exit_sem, ticks_at_least(UL_STOP_MS));
        }
        lock(u);
        w->open = false;
        ag_port_free(u->mem);
        ag_port_free(u->dmem);
        u->mem = NULL;
        u->dmem = NULL;
        unlock(u);
        return -AG_ENOMEM;
    }
    return AG_OK;
}

ag_err_t ag_uartlink_stats(int port, ag_spilink_stats_t *out)
{
    uartlink_t *const u = ul_of(port);
    if (u == NULL) {
        return ul_absent(port);
    }
    if (out == NULL) {
        return -AG_EINVAL;
    }
    if (u->w.lock == NULL) {
        return -AG_ENODEV;
    }
    memset(out, 0, sizeof(*out));
    lock(u);
    wl_stats(&u->w, out);
    out->blocks_clocked = u->frames_tx;
    out->blocks_data = u->frames_rx;
    out->timeouts = u->skipped;
    out->stalls = ag_port_uart_dma_overruns(u->port);
    out->short_frames = u->dmg_short;
    out->wire_us = u->dmg_missing;
    out->blocks_unarmed = u->dmg_inplace;
    out->errors = u->errors;
    out->last_error = u->last_error;
    unlock(u);
    return AG_OK;
}

ag_err_t ag_uartlink_close(int port, ag_spilink_stats_t *out)
{
    uartlink_t *const u = ul_of(port);
    if (u == NULL) {
        return ul_absent(port);
    }
    if (u->w.lock == NULL || !u->w.open) {
        return -AG_ENODEV;
    }
    if (out != NULL) {
        (void)ag_uartlink_stats(port, out);
    }
    u->stop = true;
    (void)ag_port_sem_give(u->w.tx_sem);
    const bool rx_gone =
        ag_port_sem_take(u->exit_sem, ticks_at_least(UL_STOP_MS));
    const bool tx_gone =
        ag_port_sem_take(u->exit_sem, ticks_at_least(UL_STOP_MS));
    lock(u);
    u->w.open = false;
    unlock(u);
    /* a reader or a writer waiting sees the link closed when it wakes */
    (void)ag_port_sem_give(u->w.rx_sem);
    (void)ag_port_sem_give(u->w.space_sem);
    if (!rx_gone || !tx_gone) {
        /* a task still inside the port: its buffers are not ours to free */
        ag_log(AG_LOG_WARN, "uartlink", "uart%d: a link task did not stop",
               port);
        return -AG_ETIMEDOUT;
    }
    lock(u);
    ag_port_free(u->mem);
    ag_port_free(u->dmem);
    u->mem = NULL;
    u->dmem = NULL;
    memset(&u->w.tx, 0, sizeof(u->w.tx));
    memset(&u->w.rx, 0, sizeof(u->w.rx));
    unlock(u);
    return AG_OK;
}

/* ---------------------------------------------------------------------- */
/* The reader's and the writer's calls: the protocol's                    */
/* ---------------------------------------------------------------------- */

static wlink_t *wl_of(int port)
{
    uartlink_t *const u = ul_of(port);
    return (u == NULL) ? NULL : &u->w;
}

int32_t ag_uartlink_write(int port, const void *buf, size_t len)
{
    wlink_t *const w = wl_of(port);
    return (w == NULL) ? -AG_EINVAL : wl_write(w, buf, len);
}

int32_t ag_uartlink_write_wait(int port, const void *buf, size_t len,
                               uint32_t timeout_ms)
{
    wlink_t *const w = wl_of(port);
    return (w == NULL) ? -AG_EINVAL : wl_write_wait(w, buf, len, timeout_ms);
}

int32_t ag_uartlink_send(int port, const void *buf, size_t len, uint32_t flags,
                         uint32_t timeout_ms)
{
    wlink_t *const w = wl_of(port);
    return (w == NULL) ? -AG_EINVAL : wl_send(w, buf, len, flags, timeout_ms);
}

int32_t ag_uartlink_recv(int port, void *buf, size_t cap,
                         ag_spilink_block_t *info, uint32_t timeout_ms)
{
    wlink_t *const w = wl_of(port);
    return (w == NULL) ? -AG_EINVAL : wl_recv(w, buf, cap, info, timeout_ms);
}

int32_t ag_uartlink_send2(int port, const void *head, size_t hlen,
                       const void *buf, size_t len, uint32_t flags,
                       uint32_t timeout_ms)
{
    wlink_t *const w = wl_of(port);
    return (w == NULL) ? -AG_EINVAL
                       : wl_send2(w, head, hlen, buf, len, flags, timeout_ms);
}

int32_t ag_uartlink_recv2(int port, void *head, size_t hcap, void *buf,
                       size_t cap, ag_spilink_block_t *info,
                       uint32_t timeout_ms)
{
    wlink_t *const w = wl_of(port);
    return (w == NULL) ? -AG_EINVAL
                       : wl_recv2(w, head, hcap, buf, cap, info, timeout_ms);
}

int32_t ag_uartlink_read(int port, void *buf, size_t len, uint32_t timeout_ms)
{
    wlink_t *const w = wl_of(port);
    return (w == NULL) ? -AG_EINVAL : wl_read(w, buf, len, timeout_ms);
}
