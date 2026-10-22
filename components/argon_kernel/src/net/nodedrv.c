/*
 * ArgonOS - the other node's card, as a drive letter.
 *
 * Channel 5, separate from `link get` on channel 4, because this is no longer
 * one conversation but many at once: every open file is its own.
 *
 * Three things here are easy to get wrong and are done deliberately:
 *
 *   1. A read is synchronous by nature and the link is not, so every request
 *      carries a TAG and a reply is only taken if the tag matches.  Without it
 *      a reply to a request that has already timed out would be handed to
 *      whoever asked next, and two readers would get each other's bytes.
 *
 *   2. Handles are numbers the FAR node made up.  We never index a table of
 *      our own with them.
 *
 *   3. A request may not be made from the link's reader task: the reply would
 *      arrive on the very task that is blocked waiting for it.
 *
 * AND FILE BYTES DO NOT TRAVEL AS REQUESTS ANY MORE.  They did: a read was
 * ND_READ, the far node answered with ND_MAX_PAYLOAD (232) bytes, and the next
 * 232 bytes cost another round trip.  One exchange in flight at a time is a
 * ceiling of chunk / round trip whatever the wire can do, and it was measured
 * as exactly that: 232 bytes per 15 ms on the SPI pair (15 467 B/s) and 232 per
 * 5 ms on the UART (46 400 B/s), against 1 755 781 B/s of wire.  The link has a
 * 24-frame BULK window that shape never used.
 *
 * So a read now opens a STREAM.  ND_FETCH asks once for as much as the wire
 * says it moves in an eighth of a second (nd_stream_max, never less than one
 * `copy` buffer), the serving side reads its file and pushes chunks back to
 * back on BULK, and the asking side writes each chunk at its running offset.  There is no
 * reassembly and no tag table: BULK is one sequence space with go-back-N, so
 * the chunks are in order or the transfer fails loudly - and the window plus
 * the acknowledgements inside it ARE the flow control.  The byte count still
 * travels with the verdict (ND_END), because a stream that ends early must be
 * a failure and not a short file (see net/nodefs.c, which paid for that one).
 *
 * Everything genuinely request-shaped - stat, readdir, seek, write, unlink -
 * keeps the tag-matched request/answer path unchanged.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/nodedrv.h>

#include <string.h>

#include <argon/libc.h>
#include <argon/linkprov.h>
#include <argon/log.h>
#include <argon/vfs.h>

#include <argon/port/task.h>
#include <argon/port/time.h>
#include <argon/port/place.h>
#include <argon/place.h>

#include "core/place_bss.h"

#define ND_CHAN 5u
#define ND_HDR 12u
#define ND_REPLY 0x80u

/*
 * THE MOST ONE FRAME HERE MAY CARRY, and why a number is still allowed to
 * exist at all after `ND_MAX_PAYLOAD 232` was removed.
 *
 * What a frame carries is now asked of the wire (nd_payload_max below).  What
 * is left here is a CEILING on this file's own buffers, which are static or on
 * the link reader's stack - the task that has already been overflowed once on
 * this bench (see linkprov.c, eight kilobytes and why).  255 payload plus the
 * 12-byte header is 267, inside the 288-byte buffer that task receives into,
 * and it is also the widest value the one spare header byte can name.  A wire
 * that reports more than this carries smaller frames than it could: slower
 * than it could be, never unsafe.  Raising it is one number on the day there
 * is a wire worth raising it for.
 */
#define ND_CHUNK_MAX 255u

/*
 * HOW MUCH OF A FILE ONE REQUEST MAY ASK FOR, and it is not a constant either.
 *
 * The frame size comes from `mtu` (see nd_payload_max); this is how many FRAMES
 * one request may pull, and it has to not be a constant for the same reason:
 * the point of the change is that the round trip stops being the unit of work,
 * and one fixed request size makes it the unit again on any wire faster than a
 * few hundred kilobytes a second.  So this is what the wire says it moves in an
 * eighth of a second, held between two numbers that are about this file rather
 * than about a wire:
 *
 *   the floor is the buffer `copy` reads with (shell/cmd_fs.c, AG_COPY_CHUNK),
 *   because below it a file transfer is back to one request per buffer read;
 *
 *   the ceiling bounds how long the serving board stays inside its link reader
 *   task for one request - while it streams, that wire hears nothing else -
 *   and 16 KB is a second and a half even on the slowest direction this bench
 *   has measured (380 B/s, SPI).
 *
 * A wire that overstates its rate only asks for more in one request, which the
 * window and the asking side's stall timer bound anyway; the SPI pair declares
 * 2 253 521 B/s and was measured at 380, so that is not hypothetical.
 */
#define ND_STREAM_MIN 4096u
#define ND_STREAM_MAX 16384u

static uint32_t nd_stream_max(void)
{
    ag_link_info_t info;
    uint32_t       n = ND_STREAM_MIN;

    if (ag_linkprov_info(&info) == AG_OK && info.bytes_per_sec > 0) {
        n = info.bytes_per_sec / 8u;
    }
    if (n < ND_STREAM_MIN) {
        n = ND_STREAM_MIN;
    }
    if (n > ND_STREAM_MAX) {
        n = ND_STREAM_MAX;
    }
    return n;
}

/*
 * How long the asking side waits with NOTHING arriving before it calls the far
 * side dead.
 *
 * A STALL and not a total: every chunk that lands refreshes it, so a wire that
 * is slow but working is waited out and a wire that has stopped is not.  That
 * distinction is the only honest one available here, because the rate the wire
 * reports about itself is not usable as a deadline - info().bytes_per_sec said
 * 2 253 521 for a pair that was delivering 380 B/s, so a deadline computed from
 * it would fail transfers that were going as well as the wire allowed.
 */
#define ND_STALL_MS 5000u

/*
 * How long to wait for the one exchange slot.
 *
 * A holder is a stream now, not a 232-byte question, so the wait is that
 * stream's worst honest case: ND_STREAM_MAX at the slowest rate this cluster
 * has measured (380 B/s on SPI - four milliseconds a byte), plus two seconds
 * for whatever exchange follows it.  Derived from the same numbers as the
 * stream itself, so it does not need revisiting when the wire does, and still
 * bounded, so a board that has gone away cannot hold every other file here for
 * good.
 */
static uint32_t nd_lock_ms(void)
{
    return 2000u + nd_stream_max() * 4u;
}

enum {
    ND_OPEN = 1,
    ND_CLOSE,
    ND_READ,
    ND_WRITE,
    ND_SEEK,
    ND_STAT,
    ND_UNLINK,
    ND_MKDIR,
    ND_RMDIR,
    ND_OPENDIR,
    ND_READDIR,
    ND_CLOSEDIR,
    /*
     * The streamed read and the two frames of its answer.  ND_READ stays, and
     * stays served: a board that has not been flashed with this still reads
     * files with it (see nd_read's fallback).
     */
    ND_FETCH,
    ND_DATA,
    ND_END,
};

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static bool s_serving;

/*
 * One exchange outstanding at a time, request or stream.  Deliberate: it is
 * the shape the rest of this codebase already uses for file work, and it keeps
 * the tag the only thing standing between a late reply and the wrong caller -
 * a rule small enough to be sure of.
 */
static volatile bool     s_busy;
static volatile uint16_t s_tag;
static volatile bool     s_have;
static volatile int32_t  s_status;
static uint8_t           s_rbuf[ND_CHUNK_MAX] AG_KERNEL_STATIC;
static volatile uint16_t s_rlen;

/*
 * The streamed read in progress, asking side.  `s_fbuf` is the CALLER's buffer
 * and the link's reader task is the only writer of it while s_fetch is true: it
 * writes each chunk straight at s_foff, which is why nothing is reassembled and
 * why there is no per-chunk allocation anywhere in this file.
 *
 * `s_fcount` is the sender's own byte count, from the END, and it is compared
 * against s_foff before anything is reported as success.
 */
static volatile bool     s_fetch;
static volatile uint16_t s_ftag;
static uint8_t          *s_fbuf;
static volatile uint32_t s_fwant;
static volatile uint32_t s_foff;
static volatile uint32_t s_fcount;
static volatile bool     s_fend;
static volatile bool     s_frefused;
static volatile int32_t  s_fstatus;

/*
 * What one frame on the bound wire can hold, header aside.
 *
 * 232 was the UART's figure written down in four files; this asks instead, and
 * the plan's own list named this constant as the first thing a fast wire breaks
 * against.  A wire that will not say (a driver older than info()) is answered
 * with the UART's measured numbers by ag_linkprov_info itself, so there is no
 * "unknown" case here.
 */
static uint32_t nd_payload_max(void)
{
    ag_link_info_t info;
    uint32_t       n = ND_CHUNK_MAX;

    if (ag_linkprov_info(&info) == AG_OK && info.mtu > ND_HDR) {
        const uint32_t m = info.mtu - ND_HDR;
        if (m < n) {
            n = m;
        }
    }
    return n;
}

/*
 * One tag counter for both shapes of exchange, so a number is never reused
 * while anything carrying it could still be on the wire: a late reply to a
 * timed-out request and a late chunk of an abandoned stream are both caught by
 * the number, which is the only thing that can catch them.
 */
static uint16_t next_tag(void)
{
    static uint16_t t = 1;

    if (++t == 0) {
        t = 1;
    }
    return t;
}

/* Take the one exchange slot, or say why not.  Bounded, and see nd_lock_ms. */
static int32_t nd_lock(void)
{
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    if (ag_linkprov_in_reader()) {
        return -AG_EBUSY;
    }

    const int64_t give_up = ag_port_us() + (int64_t)nd_lock_ms() * 1000;
    while (s_busy) {
        if (ag_port_us() >= give_up) {
            return -AG_ETIMEDOUT;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(2));
    }
    s_busy = true;
    return AG_OK;
}

static void nd_unlock(void)
{
    /* Clear the tag before the flags: a frame arriving now belongs to nobody,
     * and the reader task must not start writing into a buffer that is about
     * to be returned to its owner. */
    s_tag = 0;
    s_ftag = 0;
    s_fetch = false;
    s_busy = false;
}

/*
 * One frame out.
 *
 * `dst` is the node it is FOR.  Zero means a NEW conversation, which the link
 * addresses to the node `link use` last named; anything else is an answer and
 * is addressed to the node that asked - which is not the same board at all
 * when a third one sits in between (net/nodefs.c had a file arrive somewhere
 * else this way, and the same latent mistake was here).
 *
 * `note` is the one header byte this protocol never used: the asking side's
 * own per-frame payload limit, so a serving side does not put a frame on the
 * wire that the asking side's link cannot hold.  Zero is "did not say", which
 * is what everything but ND_FETCH says.
 */
/* background: a frame may wait this long for its window (see fetch_task) */
#define ND_BG_WAIT_MS   60000u
#define ND_FETCH_STACK  6144u /* as nodefs.c's server, measured there */
#define ND_FETCH_PRIO   2u

static ag_err_t send_frame_on(int32_t wire, uint16_t dst, uint8_t op,
                              uint8_t note, uint16_t tag, uint32_t h,
                              uint32_t arg, uint8_t cls, const void *pay,
                              uint16_t plen)
{
    uint8_t buf[ND_HDR + ND_CHUNK_MAX];
    if (plen > ND_CHUNK_MAX) {
        plen = ND_CHUNK_MAX;
    }
    buf[0] = op;
    buf[1] = note;
    put16(buf + 2, tag);
    put32(buf + 4, h);
    put32(buf + 8, arg);
    if (plen > 0 && pay != NULL) {
        memcpy(buf + ND_HDR, pay, plen);
    }
    /* A full window is back-pressure, not a failure; bounded so a departed
     * node cannot block us for ever.  Pumping while we wait is also what lets
     * the acknowledgements that open the window come in - on a wire with one
     * clock master, nothing moves unless somebody is inside the driver. */
    const bool    bg = (cls == AG_LINK_BACKGROUND);
    const int64_t until =
        ag_port_us() + (int64_t)(bg ? ND_BG_WAIT_MS : 4000u) * 1000;
    for (;;) {
        const size_t  n = (size_t)(ND_HDR + plen);
        const int32_t r =
            (wire >= 0) ? ag_linkprov_send_to_on(wire, dst, ND_CHAN, cls, buf, n)
            : (dst == 0) ? ag_linkprov_send(ND_CHAN, cls, buf, n)
                         : ag_linkprov_send_to(dst, ND_CHAN, cls, buf, n);
        if (r >= 0) {
            return AG_OK;
        }
        if (r != -AG_EAGAIN) {
            return (ag_err_t)r;
        }
        if (ag_port_us() >= until) {
            return -AG_ETIMEDOUT;
        }
        if (bg) {
            ag_port_task_delay(1); /* its own task: the reader reads on */
        } else {
            ag_linkprov_poll(2);
        }
    }
}

static ag_err_t send_frame(uint16_t dst, uint8_t op, uint8_t note, uint16_t tag,
                           uint32_t h, uint32_t arg, uint8_t cls,
                           const void *pay, uint16_t plen)
{
    return send_frame_on(-1, dst, op, note, tag, h, arg, cls, pay, plen);
}

static int32_t ask(uint8_t op, uint32_t h, uint32_t arg, uint8_t cls,
                   const void *pay, uint16_t plen, void *out, uint16_t *outlen)
{
    const int32_t lock = nd_lock();
    if (lock != AG_OK) {
        return lock;
    }

    s_tag = next_tag();
    s_have = false;
    s_rlen = 0;

    int32_t rc = (int32_t)send_frame(0, op, 0, s_tag, h, arg, cls, pay, plen);
    if (rc == AG_OK) {
        const int64_t deadline = ag_port_us() + 15000000;
        rc = -AG_ETIMEDOUT;
        while (ag_port_us() < deadline) {
            if (s_have) {
                rc = s_status;
                break;
            }
            ag_port_task_delay(ag_port_ms_to_ticks(2));
        }
    }
    if (rc >= 0 && out != NULL && outlen != NULL) {
        uint16_t n = s_rlen;
        if (n > *outlen) {
            n = *outlen;
        }
        memcpy(out, s_rbuf, n);
        *outlen = n;
    }
    /* Clear the tag before the flag: a reply arriving now belongs to nobody. */
    nd_unlock();
    return rc;
}

/* ---- answering the other node ----------------------------------------- */

static void reply(uint8_t op, uint16_t tag, int32_t status, uint8_t cls,
                  const void *pay, uint16_t plen)
{
    /*
     * ADDRESSED TO WHOEVER ASKED, not to the board `link use` last named here.
     * The target is where a NEW conversation goes; an answer has exactly one
     * right destination, and on a board in the middle of three that is a node
     * this one may never have selected.  A reply that says "for whoever is
     * reading" is taken by the board in between.
     */
    (void)send_frame(ag_linkprov_from(), (uint8_t)(op | ND_REPLY), 0, tag,
                     (uint32_t)status, 0, cls, pay, plen);
}

/*
 * A STREAMED READ, served: the file goes back as a run of frames without
 * being asked again, and the count with the verdict (see ND_FETCH in
 * serve()).  `cls` is BACKGROUND from the fetch task and BULK on the
 * reader; `wire` the one the request came in on, or -1 on the reader, where
 * the wire is the one being read.
 */
static void fetch_serve(int32_t wire, uint16_t peer, uint16_t tag, uint32_t h,
                        uint32_t want, uint32_t chunk, uint8_t cls,
                        uint8_t *obuf)
{
    uint32_t sent = 0;
    int32_t  err = 0;
    while (sent < want) {
        uint32_t take = chunk;
        if (take > want - sent) {
            take = want - sent;
        }
        const int32_t r = ag_vfs_read((ag_handle_t)h, obuf, take);
        if (r < 0) {
            err = r;
            break;
        }
        if (r == 0) {
            break; /* end of file: a short answer, not a failure */
        }
        const ag_err_t e = send_frame_on(wire, peer, ND_DATA, 0, tag, 0, 0,
                                         cls, obuf, (uint16_t)r);
        if (e != AG_OK) {
            err = (int32_t)e;
            break;
        }
        sent += (uint32_t)r;
    }
    (void)send_frame_on(wire, peer, ND_END, 0, tag, (uint32_t)err, sent, cls,
                        NULL, 0);
}

/*
 * THE REMOTE DRIVE'S FILE ON WHAT IS LEFT OVER (27 Sep 2026), as `link get`
 * (net/nodefs.c): a fetch is handed to a task of its own and streamed on
 * BACKGROUND, behind the picture and the sound on every board.  It cannot be
 * on the reader - the window's answers come in on the reader.  One fetch at
 * a time; another, or no memory for the task, is served on the reader, on
 * BULK, as before.
 */

static struct {
    volatile bool busy;
    int32_t       wire;
    uint16_t      peer, tag;
    uint32_t      h, want, chunk;
} s_fsrv;

static void fetch_task(void *arg)
{
    (void)arg;
    uint8_t obuf[ND_CHUNK_MAX]; /* on this task's stack, not in static RAM */
    fetch_serve(s_fsrv.wire, s_fsrv.peer, s_fsrv.tag, s_fsrv.h,
                s_fsrv.want, s_fsrv.chunk, AG_LINK_BACKGROUND, obuf);
    s_fsrv.busy = false;
    ag_log(AG_LOG_INFO, "nodedrv", "a read served; %u of %u stack bytes unused",
           (unsigned)ag_port_task_stack_unused(NULL), (unsigned)ND_FETCH_STACK);
    ag_port_task_delete(NULL);
}

static uint16_t enc_stat(uint8_t *p, const ag_stat_t *st)
{
    put64(p, st->size);
    put64(p + 8, st->mtime);
    put32(p + 16, st->attr);
    return 20;
}
static void dec_stat(const uint8_t *p, ag_stat_t *st)
{
    st->size = get64(p);
    st->mtime = get64(p + 8);
    st->attr = get32(p + 16);
}

static void serve(const uint8_t *b, uint16_t n)
{
    const uint8_t  op = b[0];
    const uint16_t tag = get16(b + 2);
    const uint32_t h = get32(b + 4);
    const uint32_t arg = get32(b + 8);
    const uint8_t *pay = b + ND_HDR;
    const uint16_t plen = (uint16_t)(n - ND_HDR);
    char           path[AG_PATH_MAX];
    /* A local, and therefore a private one per reader task: two neighbours of a
     * three-board cluster can be inside serve() at the same moment, each on its
     * own wire's reader. */
    uint8_t        obuf[ND_CHUNK_MAX];
    /* What the wire will carry, asked once for this frame's answer.  Every
     * reply below is bounded by it, so nothing here can hand the driver a
     * message it is going to refuse. */
    const uint32_t cap = nd_payload_max();

    uint16_t pc = plen;
    if (pc >= sizeof(path)) {
        pc = (uint16_t)(sizeof(path) - 1);
    }
    memcpy(path, pay, pc);
    path[pc] = '\0';

    switch (op) {
    case ND_OPEN:
        reply(op, tag, (int32_t)ag_vfs_open(path, "/", arg), AG_LINK_CONTROL,
              NULL, 0);
        break;
    case ND_OPENDIR:
        reply(op, tag, (int32_t)ag_vfs_opendir(path, "/"), AG_LINK_CONTROL,
              NULL, 0);
        break;
    case ND_CLOSE:
        reply(op, tag, (int32_t)ag_vfs_close((ag_handle_t)h), AG_LINK_CONTROL,
              NULL, 0);
        break;
    case ND_CLOSEDIR:
        reply(op, tag, (int32_t)ag_vfs_closedir((ag_handle_t)h),
              AG_LINK_CONTROL, NULL, 0);
        break;
    case ND_UNLINK:
        reply(op, tag, (int32_t)ag_vfs_unlink(path, "/"), AG_LINK_CONTROL, NULL,
              0);
        break;
    case ND_MKDIR:
        reply(op, tag, (int32_t)ag_vfs_mkdir(path, "/"), AG_LINK_CONTROL, NULL,
              0);
        break;
    case ND_RMDIR:
        reply(op, tag, (int32_t)ag_vfs_rmdir(path, "/"), AG_LINK_CONTROL, NULL,
              0);
        break;
    case ND_READ: {
        /* The request-shaped read, kept for a peer that does not know
         * ND_FETCH.  Sized by the wire, not by a number of ours. */
        uint32_t want = arg;
        if (want > cap) {
            want = cap;
        }
        const int32_t r = ag_vfs_read((ag_handle_t)h, obuf, want);
        /* BULK: file bytes may wait behind anything with a deadline. */
        reply(op, tag, r, AG_LINK_BULK, obuf, (uint16_t)((r > 0) ? r : 0));
        break;
    }
    case ND_FETCH: {
        /*
         * A STREAMED READ: one request, and the file goes back as a run of
         * BULK frames without being asked again.
         *
         * The window and the acknowledgements in it are the flow control -
         * send_frame waits when the window is full and pumps the wire while it
         * waits, which is also what lets the acknowledgements that open it
         * arrive.  BULK is one sequence space with go-back-N, so the chunks
         * reach the asking side in order and the END below cannot overtake the
         * data it ends (net/nodefs.c measured what END on CONTROL did: a
         * 19 024-byte file reported as 15 600 bytes, with no error said
         * anywhere).
         *
         * WHO ASKED is taken once, here, because this callback is a long one:
         * ag_linkprov_from() answers only while it runs, and further frames on
         * this wire are handled by this same task before it returns.  Every
         * frame of the answer is addressed to that node - a stream addressed
         * to "whoever is reading" is taken by the board in between and the
         * asking node waits for a file that arrived somewhere else.
         */
        const uint16_t peer = ag_linkprov_from();
        const uint32_t most = nd_stream_max();
        uint32_t       want = arg;
        if (want > most) {
            want = most;
        }
        /* The asking side may say that its own link holds less than ours; it
         * must never be sent a frame its end cannot receive. */
        uint32_t chunk = cap;
        if (b[1] != 0 && (uint32_t)b[1] < chunk) {
            chunk = b[1];
        }

        if (!s_fsrv.busy) {
            s_fsrv.busy = true;
            s_fsrv.wire = ag_linkprov_current();
            s_fsrv.peer = peer;
            s_fsrv.tag = tag;
            s_fsrv.h = h;
            s_fsrv.want = want;
            s_fsrv.chunk = chunk;
            ag_port_task_t t = NULL;
            if (ag_place_task("cluster.tasks", fetch_task, "nodedrv",
                              ND_FETCH_STACK, NULL, ND_FETCH_PRIO,
                              AG_PORT_ANY_CORE, (void **)&t, NULL)) {
                break; /* the task answers it all, END included */
            }
            s_fsrv.busy = false;
        }
        fetch_serve(-1, peer, tag, h, want, chunk, AG_LINK_BULK, obuf);

        /*
         * THE COUNT GOES WITH THE VERDICT, on BULK, in the same sequence space
         * as the bytes it counts.
         *
         * `sent` is what was handed to the reliable class and not what was
         * read, because a chunk the link refused never left.  The asking side
         * compares it against the bytes that reached its buffer, so a stream
         * that ended early is a failure and not a short file - the property
         * that makes silent truncation impossible, and the one this floor has
         * already paid for once (net/nodefs.c).
         */
        break;
    }
    case ND_WRITE:
        reply(op, tag, ag_vfs_write((ag_handle_t)h, pay, plen), AG_LINK_CONTROL,
              NULL, 0);
        break;
    case ND_SEEK: {
        const int64_t off = (plen >= 8) ? (int64_t)get64(pay) : 0;
        const int64_t r = ag_vfs_seek((ag_handle_t)h, off, (int)arg);
        if (r < 0) {
            reply(op, tag, (int32_t)r, AG_LINK_CONTROL, NULL, 0);
        } else {
            put64(obuf, (uint64_t)r);
            reply(op, tag, 0, AG_LINK_CONTROL, obuf, 8);
        }
        break;
    }
    case ND_STAT: {
        ag_stat_t      st;
        const ag_err_t e = ag_vfs_stat(path, "/", &st);
        if (e != AG_OK) {
            reply(op, tag, (int32_t)e, AG_LINK_CONTROL, NULL, 0);
        } else {
            reply(op, tag, 0, AG_LINK_CONTROL, obuf, enc_stat(obuf, &st));
        }
        break;
    }
    case ND_READDIR: {
        ag_dirent_t    de;
        const ag_err_t e = ag_vfs_readdir((ag_handle_t)h, &de);
        if (e != AG_OK) {
            reply(op, tag, (int32_t)e, AG_LINK_CONTROL, NULL, 0);
        } else {
            const uint16_t k = enc_stat(obuf, &de.st);
            size_t         nl = strlen(de.name);
            /*
             * The name is cut to what the WIRE leaves beside the stat, and
             * the subtraction is guarded: `cap` comes from a driver now and a
             * too-small one would otherwise wrap it into an enormous length
             * and write the name straight off the end of obuf.
             */
            const size_t room = (cap > (uint32_t)k + 1u)
                                    ? (size_t)(cap - (uint32_t)k - 1u)
                                    : 0u;
            if (nl > room) {
                nl = room;
            }
            memcpy(obuf + k, de.name, nl);
            obuf[k + nl] = '\0';
            reply(op, tag, 0, AG_LINK_CONTROL, obuf, (uint16_t)(k + nl + 1));
        }
        break;
    }
    default:
        reply(op, tag, -AG_ENOTSUP, AG_LINK_CONTROL, NULL, 0);
        break;
    }
}

/*
 * A frame of a streamed read, on the board that asked for it.
 *
 * Every chunk is written straight at the running offset and nothing is
 * reassembled: BULK is one sequence space with go-back-N, so what arrives
 * arrives in order or the transfer ends in a failure.  That is the whole reason
 * a stream needs no tag table, no receive window and no credit of its own - the
 * link's window IS the flow control, and the acknowledgements in it are how the
 * serving side is slowed down.
 */
static void on_stream(const uint8_t *b, uint16_t n)
{
    const uint8_t  op = b[0];
    const uint16_t tag = get16(b + 2);

    if (op == (uint8_t)(ND_FETCH | ND_REPLY)) {
        /*
         * NOT a stream: a peer that does not know ND_FETCH answers the request
         * itself, and serve()'s default case answers -ENOTSUP.  Its verdict is
         * the request's answer, and nd_read falls back to the one-exchange
         * read that peer does know.
         */
        if (!s_fetch || tag != s_ftag) {
            return;
        }
        s_fstatus = (int32_t)get32(b + 4);
        s_frefused = true;
        s_fend = true;
        return;
    }

    /* A chunk or an end that nobody is waiting for - abandoned, timed out, or
     * a tail arriving after the fetch gave up - is dropped by its number. */
    if (!s_fetch || tag != s_ftag) {
        return;
    }

    if (op == ND_DATA) {
        const uint16_t plen = (uint16_t)(n - ND_HDR);
        if (plen == 0) {
            return;
        }
        if (plen > s_fwant - s_foff) {
            /*
             * MORE THAN WAS ASKED FOR, and it is a failure rather than extra
             * credit, for the same reason a short write is: the bytes after
             * this one would land past the end of the caller's buffer or, if
             * silently dropped, would shift everything behind them.
             */
            s_fstatus = -AG_EIO;
            s_fend = true;
            return;
        }
        memcpy(s_fbuf + s_foff, b + ND_HDR, plen);
        s_foff += plen;
        return;
    }

    /*
     * ND_END.  The sender's own count travels with the verdict and is compared
     * by the caller against what reached the buffer: the last frame arriving
     * says nothing about the length of the file, which is exactly how a
     * truncated copy was once reported as a success (net/nodefs.c).
     */
    s_fstatus = (int32_t)get32(b + 4);
    s_fcount = get32(b + 8);
    s_fend = true;
}

static void on_msg(uint8_t chan, const void *data, uint16_t n, void *ctx)
{
    (void)chan;
    (void)ctx;
    if (n < ND_HDR) {
        return;
    }
    const uint8_t *b = (const uint8_t *)data;

    /*
     * THE STREAMED READ's frames first.  They are neither requests nor replies
     * in the tag-matched sense: they are one answer's chunks, recognized by
     * their opcode and their number, and only the side that asked ever sees
     * them - so they are decided before the request/reply split below, which
     * would send them to serve() as if they were questions.
     */
    if (b[0] == ND_DATA || b[0] == ND_END ||
        b[0] == (uint8_t)(ND_FETCH | ND_REPLY)) {
        on_stream(b, n);
        return;
    }

    if ((b[0] & ND_REPLY) == 0) {
        serve(b, n);
        return;
    }

    /* A reply.  Only the one we are waiting for, matched by tag. */
    const uint16_t tag = get16(b + 2);
    if (!s_busy || s_have || tag == 0 || tag != s_tag) {
        return;
    }
    s_status = (int32_t)get32(b + 4);
    uint16_t plen = (uint16_t)(n - ND_HDR);
    if (plen > sizeof(s_rbuf)) {
        plen = (uint16_t)sizeof(s_rbuf);
    }
    memcpy(s_rbuf, b + ND_HDR, plen);
    s_rlen = plen;
    s_have = true;
}

ag_err_t ag_nodedrv_serve(void)
{
    if (s_serving) {
        return AG_OK;
    }
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    const ag_err_t e = ag_linkprov_listen(ND_CHAN, on_msg, NULL);
    if (e != AG_OK) {
        return e;
    }
    s_serving = true;
    return AG_OK;
}

/* ---- the backend ------------------------------------------------------- */

static uint32_t fh(void *file)
{
    return (uint32_t)(uintptr_t)file - 1u;
}

/*
 * The streamed read, asking side: one request, and the bytes arrive on BULK.
 *
 * This is the whole of the change that makes a file move at the speed of the
 * wire instead of the speed of a round trip.  It was one ND_READ per 232 bytes,
 * each waiting for its own reply, which is a ceiling of chunk / round trip
 * whatever the link can do - measured at 232 bytes per 15 ms on the SPI pair.
 * Here the serving side is told once how much is wanted and answers with a run
 * of chunks, and its own window fills the wire while the asking side writes
 * each chunk at the running offset.
 *
 * `len` is the caller's buffer size, so one read of a 4096-byte buffer is one
 * request and about seventeen frames, and a caller with a bigger buffer gets
 * fewer requests rather than more round trips - up to the wire-derived bound,
 * past which one request would hold the serving board's link reader for longer
 * than the rest of that wire can wait.
 */
static int32_t fetch(uint32_t h, void *out, size_t len)
{
    if (out == NULL || len == 0) {
        return 0;
    }

    uint32_t want = (uint32_t)len;
    const uint32_t most = nd_stream_max();
    if (want > most) {
        want = most;
    }

    const int32_t lock = nd_lock();
    if (lock != AG_OK) {
        return lock;
    }

    s_ftag = next_tag();
    s_fbuf = (uint8_t *)out;
    s_fwant = want;
    s_foff = 0;
    s_fcount = 0;
    s_fend = false;
    s_frefused = false;
    s_fstatus = 0;
    /* Published LAST: the reader task is allowed to write into `out` only once
     * every field it needs means something. */
    s_fetch = true;

    int32_t rc = (int32_t)send_frame(0, ND_FETCH, (uint8_t)nd_payload_max(),
                                     s_ftag, h, want, AG_LINK_CONTROL, NULL, 0);
    if (rc == AG_OK) {
        int64_t  stall = ag_port_us() + (int64_t)ND_STALL_MS * 1000;
        uint32_t seen = 0;

        for (;;) {
            if (s_fend) {
                if (s_frefused) {
                    /*
                     * Not a failure to fetch, a peer that does not know how:
                     * the caller falls back (see nd_read).  -ENOTSUP is what
                     * serve()'s default case answers an unknown op, and it is
                     * the only way out of here that means "try the old way".
                     */
                    rc = (s_fstatus < 0) ? s_fstatus : -AG_ENOTSUP;
                } else if (s_fstatus == -AG_ENOTSUP) {
                    /*
                     * A stream that RAN and failed with that code is not a
                     * refusal, whatever it says: the far side has already read
                     * and moved its file position, so falling back to a second
                     * read would hand the caller bytes from further into the
                     * file.  Reported as the I/O failure it is, for the same
                     * reason a stream that ends short is.
                     */
                    rc = -AG_EIO;
                } else if (s_fstatus != AG_OK) {
                    rc = s_fstatus;
                } else if (s_fcount != s_foff) {
                    /* The count is only a promise about a transfer the sender
                     * called good; behind an error it is where it stopped. */
                    rc = -AG_EIO;
                } else {
                    rc = (int32_t)s_foff;
                }
                break;
            }
            /*
             * Refreshed by progress, not by the clock alone: this measures a
             * STALL, so a wire that is slow but working is waited out, and one
             * that has stopped is caught in ND_STALL_MS.  The waiting task
             * sleeps between checks rather than spinning - the reader task is
             * the one doing the work, and it is also the task that must keep
             * the wire moving on a link with one clock master.
             */
            if (s_foff != seen) {
                seen = s_foff;
                stall = ag_port_us() + (int64_t)ND_STALL_MS * 1000;
            } else if (ag_port_us() >= stall) {
                rc = -AG_ETIMEDOUT;
                break;
            }
            ag_port_task_delay(ag_port_ms_to_ticks(2));
        }
    }

    nd_unlock();
    return rc;
}

/*
 * A PATH MUST FIT ONE FRAME, and it is checked against the WIRE now rather
 * than against a number of ours.
 *
 * The request used to be cut to ND_MAX_PAYLOAD and sent anyway, so a path
 * longer than that opened whatever its first 232 bytes named - a different
 * file, with no error said anywhere.  A path that cannot travel is refused
 * here, and refused before anything is opened or created on the far side.
 * (net/nodefs.c had the same defect in its fetch and lost it the same way.)
 */
static ag_err_t nd_check_path(const char *rel)
{
    if (rel == NULL) {
        return -AG_EINVAL;
    }
    const size_t n = strlen(rel);
    if (n == 0 || n > nd_payload_max()) {
        return -AG_EINVAL;
    }
    return AG_OK;
}

static ag_err_t nd_open(void *c, const char *rel, uint32_t flags, void **file)
{
    (void)c;
    const ag_err_t e = nd_check_path(rel);
    if (e != AG_OK) {
        return e;
    }
    const int32_t r = ask(ND_OPEN, 0, flags, AG_LINK_CONTROL, rel,
                          (uint16_t)strlen(rel), NULL, NULL);
    if (r < 0) {
        return (ag_err_t)r;
    }
    /* +1 so handle 0 is not NULL, which the core reads as "no file". */
    *file = (void *)(uintptr_t)(uint32_t)(r + 1);
    return AG_OK;
}
static ag_err_t nd_close(void *c, void *file)
{
    (void)c;
    return (ag_err_t)ask(ND_CLOSE, fh(file), 0, AG_LINK_CONTROL, NULL, 0, NULL,
                         NULL);
}
static int32_t nd_read(void *c, void *file, void *buf, size_t len)
{
    (void)c;
    const int32_t n = fetch(fh(file), buf, len);

    if (n != -AG_ENOTSUP) {
        return n;
    }

    /*
     * A PEER THAT DOES NOT KNOW ND_FETCH.  One board flashed and one not is the
     * normal state of this bench while an image is going onto the second, and a
     * mount that stopped reading across that window would be a worse failure
     * than a slow one - so the old shape, one exchange per chunk, is kept as
     * the fallback.
     *
     * And the count is checked here too: the reply's status is the number of
     * bytes the far side read, and its payload is what arrived.  When they
     * disagree the reply was cut somewhere this end cannot see, and reporting
     * the larger number would be the silent truncation this file has already
     * been bitten by.  Fewer bytes than were asked for is not a failure - it is
     * end of file - but fewer bytes than were CLAIMED is.
     */
    uint16_t      got = (len > ND_CHUNK_MAX) ? (uint16_t)ND_CHUNK_MAX
                                             : (uint16_t)len;
    const int32_t r =
        ask(ND_READ, fh(file), (uint32_t)got, AG_LINK_CONTROL, NULL, 0, buf,
            &got);
    if (r < 0) {
        return r;
    }
    return (r == (int32_t)got) ? r : -AG_EIO;
}
static int32_t nd_write(void *c, void *file, const void *buf, size_t len)
{
    (void)c;
    /* One frame's worth, from the wire: a bigger request would be refused by
     * the driver and the write would fail for a reason nobody could see. */
    const uint32_t cap = nd_payload_max();
    if (len > cap) {
        len = cap;
    }
    if (len == 0) {
        return 0;
    }
    return ask(ND_WRITE, fh(file), 0, AG_LINK_BULK, buf, (uint16_t)len, NULL,
               NULL);
}
static int64_t nd_seek(void *c, void *file, int64_t off, int whence)
{
    (void)c;
    uint8_t  p[8];
    uint8_t  o[8];
    uint16_t on = (uint16_t)sizeof(o);
    put64(p, (uint64_t)off);
    const int32_t r =
        ask(ND_SEEK, fh(file), (uint32_t)whence, AG_LINK_CONTROL, p, 8, o, &on);
    if (r < 0) {
        return r;
    }
    return (on >= 8) ? (int64_t)get64(o) : 0;
}
static ag_err_t nd_stat(void *c, const char *rel, ag_stat_t *out)
{
    (void)c;
    /*
     * The mount root is a directory by definition, and answering that here
     * rather than over the wire is not an optimisation: `dir N:\` stats the
     * path first to decide whether it is a directory or a filename pattern, so
     * a root that cannot be stat'ed is a root that gets listed as a PATTERN -
     * which is what it did, and it looked like an empty far card rather than
     * like a failure.
     */
    if (rel == NULL || rel[0] == '\0' ||
        (rel[0] == '/' && rel[1] == '\0')) {
        memset(out, 0, sizeof(*out));
        out->attr = AG_A_DIR;
        return AG_OK;
    }
    uint8_t       o[20];
    uint16_t      on = (uint16_t)sizeof(o);
    const ag_err_t pe = nd_check_path(rel);
    if (pe != AG_OK) {
        return pe;
    }
    const int32_t r = ask(ND_STAT, 0, 0, AG_LINK_CONTROL, rel,
                          (uint16_t)strlen(rel), o, &on);
    if (r < 0) {
        return (ag_err_t)r;
    }
    if (on < 20) {
        return -AG_EIO;
    }
    dec_stat(o, out);
    return AG_OK;
}
static ag_err_t nd_path_op(uint8_t op, const char *rel)
{
    const ag_err_t pe = nd_check_path(rel);
    if (pe != AG_OK) {
        return pe;
    }
    return (ag_err_t)ask(op, 0, 0, AG_LINK_CONTROL, rel, (uint16_t)strlen(rel),
                         NULL, NULL);
}
static ag_err_t nd_unlink(void *c, const char *rel)
{
    (void)c;
    return nd_path_op(ND_UNLINK, rel);
}
static ag_err_t nd_mkdir(void *c, const char *rel)
{
    (void)c;
    return nd_path_op(ND_MKDIR, rel);
}
static ag_err_t nd_rmdir(void *c, const char *rel)
{
    (void)c;
    return nd_path_op(ND_RMDIR, rel);
}
static ag_err_t nd_opendir(void *c, const char *rel, void **dir)
{
    (void)c;
    const ag_err_t pe = nd_check_path(rel);
    if (pe != AG_OK) {
        return pe;
    }
    const int32_t r = ask(ND_OPENDIR, 0, 0, AG_LINK_CONTROL, rel,
                          (uint16_t)strlen(rel), NULL, NULL);
    if (r < 0) {
        return (ag_err_t)r;
    }
    *dir = (void *)(uintptr_t)(uint32_t)(r + 1);
    return AG_OK;
}
static ag_err_t nd_readdir(void *c, void *dir, ag_dirent_t *out)
{
    (void)c;
    uint8_t       o[ND_CHUNK_MAX];
    uint16_t      on = (uint16_t)sizeof(o);
    const int32_t r =
        ask(ND_READDIR, fh(dir), 0, AG_LINK_CONTROL, NULL, 0, o, &on);
    if (r < 0) {
        return (ag_err_t)r;
    }
    if (on < 21) {
        return -AG_EIO;
    }
    memset(out, 0, sizeof(*out));
    dec_stat(o, &out->st);
    o[on - 1] = '\0';
    ag_strlcpy(out->name, (const char *)(o + 20), sizeof(out->name));
    return AG_OK;
}
static ag_err_t nd_closedir(void *c, void *dir)
{
    (void)c;
    return (ag_err_t)ask(ND_CLOSEDIR, fh(dir), 0, AG_LINK_CONTROL, NULL, 0,
                         NULL, NULL);
}
static ag_err_t nd_info(void *c, ag_fsinfo_t *out)
{
    (void)c;
    memset(out, 0, sizeof(*out));
    ag_strlcpy(out->fs, "node", sizeof(out->fs));
    out->removable = true;
    return AG_OK;
}

static const ag_fs_ops_t nd_ops = {
    .name = "node",
    .open = nd_open,
    .close = nd_close,
    .read = nd_read,
    .write = nd_write,
    .seek = nd_seek,
    .stat = nd_stat,
    .unlink = nd_unlink,
    .mkdir = nd_mkdir,
    .rmdir = nd_rmdir,
    .opendir = nd_opendir,
    .readdir = nd_readdir,
    .closedir = nd_closedir,
    .info = nd_info,
};

ag_err_t ag_nodedrv_mount(const char *mountpoint)
{
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    const ag_err_t e = ag_nodedrv_serve();
    if (e != AG_OK) {
        return e;
    }
    return ag_vfs_mount(mountpoint, &nd_ops, NULL, AG_MOUNT_REMOVABLE);
}

ag_err_t ag_nodedrv_unmount(const char *mountpoint)
{
    return ag_vfs_unmount(mountpoint);
}
