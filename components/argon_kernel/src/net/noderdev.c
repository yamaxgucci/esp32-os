/*
 * ArgonOS - the other node's devices, in this node's registry.  See
 * argon/noderdev.h for what it is for and what it deliberately is not.
 *
 * Channel 6.  Request and reply with a tag, the same shape as the remote drive
 * next door, and for the same reason: a device read is synchronous by nature
 * and the link is not, so a reply is taken only when its tag matches the
 * question that is outstanding.
 *
 * TWO TASKS ARE INVOLVED AND BOTH MATTER.
 *
 * A proxy's read or write runs on the CALLER's task and blocks there, which is
 * what makes it look like a device.  It may not run on the link's reader task:
 * the answer is delivered by that task, so waiting on it waits for itself.
 *
 * Serving the far node's request may not run on the reader task either, and
 * this one is less obvious: a device operation is entitled to block - an audio
 * write blocks when the buffer is full, which is the whole point of it - and a
 * server that blocks on the reader task stops this node hearing anything at
 * all, including the acknowledgements its own traffic is waiting for.  So the
 * request is handed to a worker and the reader returns at once.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/noderdev.h>

#include <stdio.h>
#include <string.h>

#include <argon/device.h>
#include <argon/display.h>
#include <argon/port/audio.h>
#include <argon/port/mem.h>

#include "proc/proc_internal.h"

#include "net/rdpaint.h"
#include "net/wtrace.h"
#include <argon/libc.h>
#include <argon/linkprov.h>
#include <argon/log.h>
#include <argon/nodedev.h>
#include <argon/nodename.h>
#include <argon/power.h>
#include <argon/proc.h>
#include <argon/textpanel.h>

#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/time.h>
#include <argon/port/place.h>

#define TAG "rdev"

#define RD_CHAN 6u
#define RD_HDR 16u
#define RD_MAX_PAYLOAD 224u

/*
 * THE SIZE OF A POURED DISPLAY MESSAGE COMES FROM THE WIRE, and 224 was a
 * number chosen for the smallest wire this tree has.  Cut to it, a band of
 * pixels became 25 messages and the screen on the other board ran at 910 KB/s
 * over a 20 MHz SPI link that was clocking twice that - the wire was idle half
 * the time while the proxy made 3 800 sends a second.  The requests and
 * replies above stay at RD_MAX_PAYLOAD: they are small, and they are not what
 * was slow.  RD_POUR_CAP is the largest the buffers are made for; the size in
 * use is whatever the bound link says it will carry.
 */
#define RD_POUR_CAP 2024u
/*
 * A piece of SOUND is at most this (27 Sep 2026): a mixer period of 256
 * frames of stereo is 1 024 bytes, and a sound slot sized for a 2 KB picture
 * message was half empty - nine of them, 9 KB of the CYD's memory holding
 * nothing.  The sender cuts sound to this, whole frames: half a period now,
 * see RD_SCR_CAP below.
 */
#define RD_SND_CAP 512u
/*
 * SMALL BLOCKS, AND MORE OF THEM (27 Sep 2026, Maxim: "от размера блока
 * скорость передачи сильно не зависит, поэтому надо брать блоки поменьше,
 * буфер на большее количество блоков").  A picture message is at most this
 * on the wire, whatever the link would carry: a 2 KB message held the one
 * reader of the wire for the time a 2 KB copy and a full queue took, and the
 * sound behind it waited.  In the same memory the picture queue has 28
 * places instead of 9 and sound 18 instead of 9.  rdp_damage needs a run's
 * header and one packed chunk, about 526 bytes; 640 leaves it room.
 */
#define RD_SCR_CAP 640u
#define RD_REPLY 0x80u

/*
 * In the flags byte: these bytes were POURED, not asked about.  No tag, no
 * answer, no retransmission - see rd_write.
 */
#define RD_F_NOREPLY 0x01u

enum {
    RD_OPEN = 1, /* payload = the device's own name (unqualified) */
    RD_CLOSE,
    RD_READ,
    RD_WRITE,
    RD_IOCTL,
    RD_SIZE,
    /*
     * A DEVICE THAT IS NOT A STREAM OF BYTES.
     *
     * open/read/write/ioctl/size is the whole of what a proxy used to forward,
     * and it is enough for anything whose interface IS bytes - which is why a
     * board with no converter could already play through its neighbour's.  A
     * screen is not that: its class vtable is blit_rect, info and the text
     * three, and a proxy without them is a device the graphics layer walks
     * straight past.  Measured before these existed: an application on the
     * screenless board drew 464 700 KB of pixels and 632 bytes crossed the
     * wire.  The devices were VISIBLE and not USABLE.
     */
    RD_GFXINFO,  /* -> width, height, fmt, stride, flags                   */
    RD_BLIT,     /* poured: geometry then pixels, no reply                 */
    RD_TEXTINFO, /* -> cols, rows                                          */
    RD_TEXTROW,  /* poured: row then cells                                 */
    RD_TEXTCUR,  /* poured: col, row, visible                              */
    /*
     * poured, but RELIABLY and never dropped: what changed on the screen
     * since the far end last showed it, packed (net/rdpaint.h).  The far
     * screen is a copy kept by what arrives, so a lost one would stay lost.
     */
    RD_BLITZ,
    /*
     * "that pass is over": poured behind the pass's last RD_BLITZ on the same
     * class, so it arrives after all of them, and answered at once with the
     * same tag - which is how the painter learns the far end has the pass
     * (see rdp_set_sync).  An image from before this answers nothing, and the
     * painter stops asking after a few passes of silence.
     */
    RD_PAINTED,
};

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/*
 * Header: op, reserved, tag, handle, a, b.
 *   OPEN   a = open flags,          payload = the device name
 *   READ   a = length wanted,       b = offset (low 32; a device that needs
 *                                     more than four gigabytes of offset over
 *                                     a wire is not a case this has)
 *   WRITE  b = offset,              payload = the bytes
 *   IOCTL  a = command,             payload = the argument, returned in the
 *                                     reply so an in/out ioctl works
 *   reply: handle field carries the status or the count.
 */
static bool s_serving;

static volatile bool     s_busy;
static volatile uint16_t s_tag;
static volatile bool     s_have;
static volatile int32_t  s_status;
static uint8_t           s_rbuf[RD_MAX_PAYLOAD] AG_PORT_SLOW_BSS;
static volatile uint16_t s_rlen;

/*
 * EVERY MESSAGE HERE GOES TO ONE WIRE AND ONE NODE, named.  It went to "the
 * link", which is whatever `link use` selected last - so with two neighbours
 * the second one's devices were listed and every call to them went to the
 * first (25 Sep 2026).  A proxy knows where its device is; a request being
 * served knows who asked.  Wire -1 is the old meaning, kept for a caller that
 * has neither.
 */
static int32_t link_send(int32_t wire, uint16_t dst, uint8_t cls,
                         const void *buf, uint16_t len)
{
    if (wire < 0) {
        return ag_linkprov_send(RD_CHAN, cls, buf, len);
    }
    return ag_linkprov_send_to_on(wire, dst, RD_CHAN, cls, buf, len);
}

static ag_err_t send_raw(int32_t wire, uint16_t dst, uint8_t op, uint8_t flags,
                         uint16_t tag, uint32_t h, uint32_t a, uint32_t b,
                         uint8_t cls, const void *pay, uint16_t plen)
{
    uint8_t buf[RD_HDR + RD_MAX_PAYLOAD];
    if (plen > RD_MAX_PAYLOAD) {
        plen = RD_MAX_PAYLOAD;
    }
    buf[0] = op;
    buf[1] = flags;
    put16(buf + 2, tag);
    put32(buf + 4, h);
    put32(buf + 8, a);
    put32(buf + 12, b);
    if (plen > 0 && pay != NULL) {
        memcpy(buf + RD_HDR, pay, plen);
    }
    /* Bounded short, and for the same reason as the wait in ask(): this runs
     * with the registry's one device lock held, so a full window must cost a
     * pause and not the machine. */
    for (int t = 0; t < 200; t++) {
        const int32_t r =
            link_send(wire, dst, cls, buf, (uint16_t)(RD_HDR + plen));
        if (r >= 0) {
            return AG_OK;
        }
        if (r != -AG_EAGAIN) {
            return (ag_err_t)r;
        }
        ag_linkprov_poll(2);
    }
    return -AG_ETIMEDOUT;
}

typedef struct rdev rdev_t;
static int32_t proxy_wire(const rdev_t *p);
static uint16_t proxy_node(const rdev_t *p);

static int32_t ask(const rdev_t *p, uint8_t op, uint32_t h, uint32_t a,
                   uint32_t b, uint8_t cls, const void *pay, uint16_t plen,
                   void *out, uint16_t *outlen)
{
    static uint16_t next_tag = 1;

    const int32_t wire = proxy_wire(p);
    if (wire < 0 ? !ag_linkprov_bound() : ag_linkprov_name_at(wire) == NULL) {
        return -AG_ENODEV;
    }
    if (ag_linkprov_in_reader()) {
        return -AG_EBUSY;
    }

    /* Same reason as the deadline below: this waits with the one device lock
     * held, so it may not wait long. */
    const int64_t give_up = ag_port_us() + 1000000;
    while (s_busy) {
        if (ag_port_us() >= give_up) {
            return -AG_ETIMEDOUT;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(2));
    }
    s_busy = true;
    if (++next_tag == 0) {
        next_tag = 1;
    }
    s_tag = next_tag;
    s_have = false;
    s_rlen = 0;

    int32_t rc = (int32_t)send_raw(wire, proxy_node(p), op, 0, s_tag, h, a, b,
                                   cls, pay, plen);
    if (rc == AG_OK) {
        /*
         * SHORT, and the reason is not the wire.
         *
         * Every one of these calls arrives through the device registry, which
         * holds ONE recursive mutex across the whole of a driver's call - so
         * for as long as this waits, no other task on this board can touch any
         * device at all.  Including the console: a blocking audio write holding
         * that lock is what once ate the picture on this project, and fifteen
         * seconds of it is not a latency, it is a freeze that looks like a
         * crash.  The round trip measures three milliseconds; a second and a
         * half is a wide margin for an answer and a short one for a freeze.
         *
         * A call that does time out returns an error, which the caller can act
         * on.  The alternative - waiting patiently - takes the whole machine
         * down with it.
         */
        /*
         * LONGER FOR WHAT HAPPENS ONCE.  The registry lets its lock go for a
         * driver's call now and this waits under the one device's own lock,
         * so what a longer wait costs is that device, not the machine.  An
         * open is rare and on a busy neighbour it is slow - behind a whole
         * frame of a picture being relayed to a 4 Mbaud wire it did not come
         * back in a second and a half, the asker gave up, asked again, and
         * the neighbour closed the session it had just opened, over and over
         * (26 Sep 2026).  Reads and writes, which are many, keep the short one.
         */
        const bool    once = (op != RD_READ && op != RD_WRITE);
        const int64_t deadline = ag_port_us() + (once ? 4000000 : 1500000);
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
    s_tag = 0;
    s_busy = false;
    return rc;
}

/* ---- the proxy ---------------------------------------------------------- */

/*
 * One proxy.  `priv` on the registry entry points at this.
 *
 * The far handle is kept here rather than derived, because a device is opened
 * and closed by the layer above and the far node's handle has to live exactly
 * as long as that.
 */
struct rdev {
    char    name[AG_DEV_NAME_MAX]; /* the device's own name, over there */
    char    qualified[AG_DEV_NAME_MAX];
    int32_t far_handle;
    bool    used;
    /* where it is: this board's wire to it, and the node's name hash */
    int32_t  wire;
    uint16_t node;
    /* whose it is: a neighbour (the wire's index) or a node beyond one */
    int8_t   src;
    /* a screen, and a panel rather than a buffer (see pick_display) */
    bool     display;
    bool     panel;
    /*
     * A sound device's two formats: what the caller writes (`in`) and what
     * goes over the wire and is set on the far device (`wire`).  They differ
     * when the wire cannot carry the caller's - see fit_to_wire.  Zero rate is
     * "no format set", and then bytes go through untouched.
     */
    uint32_t in_rate, wire_rate;
    uint8_t  in_ch, wire_ch;
    /* when the sound written so far is due to have played (see pace_audio) */
    int64_t  due_us;
    /* a far open that failed: not asked again before this (ensure_open) */
    int64_t  open_again_us;
    uint32_t open_backoff_ms;
    int32_t  open_err;
};

static int32_t proxy_wire(const rdev_t *p)
{
    return (p != NULL) ? p->wire : -1;
}

static uint16_t proxy_node(const rdev_t *p)
{
    return (p != NULL) ? p->node : 0u;
}

/*
 * Enough for a real second board, and TAKEN FROM THE HEAP when a link is
 * first bound.
 *
 * It was twelve, as a static array, and twelve is exactly what a CYD does not
 * fit: that board has fifteen devices, and the three that did not make it were
 * sd0, lcd0 and touch0 - the card, the SCREEN and the touchscreen.  Loadable
 * drivers register after the built-in ones, so what falls off the end is
 * always the interesting half, and the whole point of lending a board to a
 * headless neighbour is its screen.
 *
 * Worse than the number was the silence.  `link devs` said "12 device(s) of
 * cyd are here now" and nothing else; the three that did not fit left no
 * trace, and the missing screen read as a feature that does not work.
 *
 * From the heap because most boards are never in a cluster and should not pay
 * .bss for one - the same reasoning, and the same mistake already made once,
 * as the cluster thread table that cost 6 KB and broke TLS.
 */
#define RD_MAX_PROXIES 32
static rdev_t *s_proxy;

static rdev_t *proxy_table(void)
{
    if (s_proxy == NULL) {
        const size_t bytes = sizeof(rdev_t) * RD_MAX_PROXIES;
        /* Outside SRAM first, like the thread table next door: this is read
         * once per device open, so where it lives costs nothing and what it
         * displaces costs a lot. */
        s_proxy = (rdev_t *)ag_port_alloc(bytes, AG_MEM_SLOW | AG_MEM_BYTE);
        if (s_proxy == NULL) {
            s_proxy = (rdev_t *)ag_port_alloc(bytes, AG_MEM_FAST | AG_MEM_BYTE);
        }
        if (s_proxy != NULL) {
            memset(s_proxy, 0, sizeof(rdev_t) * RD_MAX_PROXIES);
        }
    }
    return s_proxy;
}

static ag_err_t rd_open(ag_device_t *dev, uint32_t flags)
{
    rdev_t *p = (rdev_t *)dev->priv;
    if (p == NULL) {
        return -AG_ENODEV;
    }
    const int32_t r = ask(p, RD_OPEN, 0, flags, 0, AG_LINK_CONTROL, p->name,
                          (uint16_t)strlen(p->name), NULL, NULL);
    if (r < 0) {
        return (ag_err_t)r;
    }
    p->far_handle = r;
    return AG_OK;
}

static ag_err_t rd_close(ag_device_t *dev)
{
    rdev_t *p = (rdev_t *)dev->priv;
    if (p == NULL) {
        return -AG_ENODEV;
    }
    const int32_t r = ask(p, RD_CLOSE, (uint32_t)p->far_handle, 0, 0,
                          AG_LINK_CONTROL, NULL, 0, NULL, NULL);
    p->far_handle = -1;
    return (r < 0) ? (ag_err_t)r : AG_OK;
}

static int32_t rd_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    rdev_t *p = (rdev_t *)dev->priv;
    if (p == NULL) {
        return -AG_ENODEV;
    }
    if (len > RD_MAX_PAYLOAD) {
        len = RD_MAX_PAYLOAD;
    }
    uint16_t got = (uint16_t)len;
    return ask(p, RD_READ, (uint32_t)p->far_handle, (uint32_t)len,
               (uint32_t)off, AG_LINK_CONTROL, NULL, 0, buf, &got);
}

/* What this node has poured at the neighbour, for `link` and for a person. */
static uint32_t s_sent_periods;
static uint32_t s_sent_dropped;
/* the longest send of a period this second, and when that was said */
static int64_t  s_sent_max_us, s_sent_said_us;
static uint32_t s_sent_said_n, s_sent_said_drop;

/*
 * POURING: bytes with a deadline, sent and not asked about.
 *
 * Every other write here is a question - it goes out on the reliable class and
 * waits for the far node to say how many bytes it took.  For SOUND that is the
 * wrong shape twice over.  A period is due every ten milliseconds and a round
 * trip is three, so a question costs a third of the budget for nothing; worse,
 * only one question may be outstanding at a time, so the answer to the last
 * period gates the next one - and when the far node is busy the request is
 * refused outright.  That is a dropped period MANUFACTURED BY ASKING, and the
 * answer was of no use to anybody: a period that arrives late is worse than one
 * that never arrives, and there is nothing sensible to do with "it did not fit"
 * except let it go.
 *
 * So: the stream class, no tag, no reply, no retransmission, and no waiting -
 * a period that cannot go on the wire now has missed its moment.
 *
 * THE DECISION IS BY DEVICE CLASS, not by a flag from the caller, and that is
 * deliberate.  Whether bytes need a receipt is a property of what the bytes
 * ARE.  A caller that had to ask for this would be a caller that could forget
 * to, and an application writing to a converter has no business knowing which
 * board the converter is on.
 *
 * THE COST, stated because it is a real one: this returns the length without
 * evidence.  Nothing here can know the far node took the bytes, so nothing
 * downstream should be believed when it says so - the count that means
 * something is the one kept on the RECEIVING side, and this file keeps it (see
 * pour_task).  A check that trusted this return value would be a check that
 * passes with the wire unplugged, and this tree had one.
 */
static int32_t pour(rdev_t *p, const void *buf, size_t len, uint64_t off)
{
    static uint8_t buf2[RD_HDR + RD_POUR_CAP] AG_PORT_SLOW_BSS;
    if (len > RD_POUR_CAP) {
        len = RD_POUR_CAP;
    }
    buf2[0] = RD_WRITE;
    buf2[1] = RD_F_NOREPLY;
    put16(buf2 + 2, 0); /* no tag: nothing will come back to match it */
    put32(buf2 + 4, (uint32_t)p->far_handle);
    put32(buf2 + 8, 0);
    put32(buf2 + 12, (uint32_t)off);
    memcpy(buf2 + RD_HDR, buf, len);

    /*
     * ONE attempt.  send_raw's retry loop is right for a request and wrong for
     * this: waiting for room means the period goes out late, and late is the
     * one thing it must not be.  The stream class does not queue behind the
     * reliable one either - that is what the class is for.
     */
    const int64_t t0 = ag_port_us();
    const int32_t r = link_send(p->wire, p->node, AG_LINK_STREAM, buf2,
                                (uint16_t)(RD_HDR + len));
    const int64_t took = ag_port_us() - t0;
    if (took > s_sent_max_us) {
        s_sent_max_us = took;
    }
    if (t0 - s_sent_said_us >= 1000000) {
        if (s_sent_said_us != 0 && ag_linkprov_reporting()) {
            ag_log(AG_LOG_INFO, TAG,
                   "sound out: %u periods sent, %u dropped, the longest send "
                   "%u us",
                   (unsigned)(s_sent_periods - s_sent_said_n),
                   (unsigned)(s_sent_dropped - s_sent_said_drop),
                   (unsigned)s_sent_max_us);
        }
        s_sent_said_us = t0;
        s_sent_said_n = s_sent_periods;
        s_sent_said_drop = s_sent_dropped;
        s_sent_max_us = 0;
    }
    if (r < 0) {
        s_sent_dropped++;
        return (int32_t)len; /* let go of it, and say the period was taken */
    }
    s_sent_periods++;
    return (int32_t)len;
}

/*
 * SOUND THAT FITS THE WIRE, and all of the caller's period.
 *
 * Two faults lived here.  A write was cut to RD_MAX_PAYLOAD (224 bytes), the
 * size of a request, so a mixer period of 1 764 bytes arrived as its first
 * eighth: the CYD received a fifth of the stream and played nothing a person
 * could hear (25 Sep 2026).  It is now sent in pieces of a stream frame.  And
 * 44.1 kHz stereo is 176 KB/s where the C3-CYD wire carries 82: no piecing
 * fixes that, so the far device is opened at a format the wire can carry and
 * the stream is brought down to it here - to one channel first, then halving
 * the rate - by averaging, which is also the filter the halving needs.  The
 * caller writes what it always wrote; on a wire that can carry it, nothing is
 * changed at all.
 */
static uint8_t s_fit_buf[RD_SND_CAP] AG_PORT_SLOW_BSS;

/*
 * THE WRITE IS THE CLOCK, and a proxy had none.
 *
 * A mixer runs on its writer's call and mixes as many periods as are ready;
 * what keeps it to real time is the converter, whose write waits for room.
 * This write never waited, so the C3 sent sound exactly as fast as its program
 * made it: slower than real time while TONE computed sinf in soft float (the
 * CYD played holes), faster once it read a table (the CYD's ring overflowed) -
 * the same stutter from both sides, with every counter on the wire clean
 * (25 Sep 2026).  So the proxy keeps time as a converter would: no more than
 * PACE_LEAD_US ahead of the clock, which is also the cushion the far board
 * starts with.
 */
/* Half the far ring (10 periods of 5.8 ms): 80 ms was more than the ring held,
 * so it sat full, the receiver paced frames out (508 in 15 s) and a person
 * heard each one as a click. */
/* 20 ms, not 30 (26 Sep 2026): 30 was half the far queue, so the queue sat
 * half full and a burst of periods behind the picture on the same wire
 * overflowed it - 465 periods of 17 018 thrown away on the CYD, a click
 * each.  20 leaves 60 ms of room for a burst. */
#define PACE_LEAD_US 20000

static void pace_audio(rdev_t *p, size_t len)
{
    if (p->in_rate == 0u || p->in_ch == 0u) {
        return;
    }
    const int64_t frames = (int64_t)(len / ((size_t)p->in_ch * 2u));
    const int64_t now = ag_port_us();
    if (p->due_us < now - PACE_LEAD_US) {
        p->due_us = now; /* a new stream, or one that stopped for a while */
    }
    p->due_us += frames * 1000000 / (int64_t)p->in_rate;
    const int64_t ahead = p->due_us - now - PACE_LEAD_US;
    /* not for a program being stopped: asleep in here it is inside the
     * driver, where a kill has to wait for it */
    if (ahead > 1000 && !ag_proc_stopping()) {
        ag_port_task_delay(ag_port_ms_to_ticks((uint32_t)(ahead / 1000)));
    }
}

static int32_t pour_audio(rdev_t *p, const void *buf, size_t len, uint64_t off)
{
    pace_audio(p, len);
    const uint8_t *src = (const uint8_t *)buf;
    const bool     convert = p->in_rate != 0u && p->wire_rate != 0u &&
                         (p->in_rate != p->wire_rate || p->in_ch != p->wire_ch);
    if (!convert) {
        for (size_t at = 0; at < len;) {
            const size_t n = (len - at > RD_SND_CAP) ? RD_SND_CAP : len - at;
            (void)pour(p, src + at, n, off + at);
            at += n;
        }
        return (int32_t)len;
    }

    const uint32_t step = p->in_rate / p->wire_rate; /* 1, 2 or 4 */
    const size_t   in_frame = (size_t)p->in_ch * 2u;
    const size_t   frames = len / in_frame;
    const int16_t *in = (const int16_t *)buf;
    size_t         out = 0;
    for (size_t f = 0; f + step <= frames; f += step) {
        int32_t acc[2] = {0, 0};
        for (uint32_t k = 0; k < step; k++) {
            for (uint8_t c = 0; c < p->in_ch; c++) {
                acc[(p->wire_ch == 1u) ? 0 : c] += in[(f + k) * p->in_ch + c];
            }
        }
        const int32_t div = (int32_t)step * ((p->wire_ch == 1u) ? p->in_ch : 1);
        for (uint8_t c = 0; c < p->wire_ch; c++) {
            const int16_t v = (int16_t)(acc[c] / div);
            s_fit_buf[out++] = (uint8_t)(v & 0xff);
            s_fit_buf[out++] = (uint8_t)((uint16_t)v >> 8);
            if (out + 4u > sizeof(s_fit_buf)) {
                (void)pour(p, s_fit_buf, out, off);
                out = 0;
            }
        }
    }
    if (out > 0) {
        (void)pour(p, s_fit_buf, out, off);
    }
    return (int32_t)len;
}

/*
 * The format the wire can carry, for a caller's: at most two thirds of what
 * the link says it sustains, the rest left to everything else on it.
 */
static void fit_to_wire(rdev_t *p, const ag_audio_fmt_t *want, ag_audio_fmt_t *far)
{
    *far = *want;
    ag_link_info_t li;
    uint32_t       room = 0;
    if (ag_linkprov_info_on(p->wire, &li) == AG_OK) {
        room = li.bytes_per_sec / 3u * 2u;
    }
    if (room != 0u) {
        while ((uint32_t)far->rate * far->channels * 2u > room) {
            if (far->channels > 1u) {
                far->channels = 1;
            } else if (far->rate >= 16000u && want->rate / (far->rate / 2u) <= 4u) {
                far->rate /= 2u;
            } else {
                break;
            }
        }
    }
    p->in_rate = want->rate;
    p->in_ch = want->channels;
    p->wire_rate = far->rate;
    p->wire_ch = far->channels;
    if (far->rate != want->rate || far->channels != want->channels) {
        ag_log(AG_LOG_INFO, TAG,
               "%s: %u Hz %u ch is more than the wire carries; %u Hz %u ch "
               "over it",
               p->qualified, (unsigned)want->rate, (unsigned)want->channels,
               (unsigned)far->rate, (unsigned)far->channels);
    }
}

static int32_t rd_write(ag_device_t *dev, const void *buf, size_t len,
                        uint64_t off)
{
    rdev_t *p = (rdev_t *)dev->priv;
    if (p == NULL) {
        return -AG_ENODEV;
    }
    if (dev->cls != AG_DEV_AUDIO && len > RD_MAX_PAYLOAD) {
        len = RD_MAX_PAYLOAD;
    }

    if (dev->cls == AG_DEV_AUDIO) {
        return pour_audio(p, buf, len, off);
    }

    /*
     * CONTROL, and NOT the stream class, which is the mistake this very line
     * made first.
     *
     * Bytes bound for a device do have a deadline, and STREAM is the class with
     * the deadline - so it looked right.  But STREAM is deliberately
     * UNRELIABLE: unsequenced, unacknowledged, delivered if it arrives.  That
     * is correct for audio being poured at a converter and fatal for a request
     * that waits for an answer: one dropped frame and the caller sits out its
     * whole timeout with nothing to retransmit it.  Measured as a write that
     * took fifteen seconds to report -AG_ETIMEDOUT while the wire was fine.
     *
     * A class that may drop is not a class you can ask a question on.  CONTROL
     * is both reliable and first, which is what this wanted; and it stays
     * bounded because only one request is outstanding at a time, so a device
     * cannot flood the wire however hard it is written to.
     */
    return ask(p, RD_WRITE, (uint32_t)p->far_handle, 0, (uint32_t)off,
               AG_LINK_CONTROL, buf, (uint16_t)len, NULL, NULL);
}

static ag_err_t rd_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                         size_t arglen)
{
    rdev_t *p = (rdev_t *)dev->priv;
    if (p == NULL) {
        return -AG_ENODEV;
    }
    if (arglen > RD_MAX_PAYLOAD) {
        return -AG_ERANGE;
    }
    /*
     * The argument goes out and comes back.  An ioctl is as often a question as
     * an order - a format, a size, a status - and sending only the command
     * would quietly turn every one of those into a no-op that reports success.
     */
    uint16_t back = (uint16_t)arglen;
    if (dev->cls == AG_DEV_AUDIO && cmd == AG_IOC_AUDIO_SETFMT && arg != NULL &&
        arglen >= sizeof(ag_audio_fmt_t) &&
        ((const ag_audio_fmt_t *)arg)->bits == 16u) {
        ag_audio_fmt_t far;
        fit_to_wire(p, (const ag_audio_fmt_t *)arg, &far);
        back = (uint16_t)sizeof(far);
        const int32_t r = ask(p, RD_IOCTL, (uint32_t)p->far_handle, cmd, 0,
                              AG_LINK_CONTROL, &far, (uint16_t)sizeof(far),
                              &far, &back);
        return (r < 0) ? (ag_err_t)r : AG_OK;
    }
    const int32_t r = ask(p, RD_IOCTL, (uint32_t)p->far_handle, cmd, 0,
                          AG_LINK_CONTROL, arg, (uint16_t)arglen, arg, &back);
    return (r < 0) ? (ag_err_t)r : AG_OK;
}

static uint64_t rd_size(ag_device_t *dev)
{
    rdev_t *p = (rdev_t *)dev->priv;
    if (p == NULL) {
        return 0;
    }
    uint8_t  o[8];
    uint16_t on = (uint16_t)sizeof(o);
    const int32_t r = ask(p, RD_SIZE, (uint32_t)p->far_handle, 0, 0,
                          AG_LINK_CONTROL, NULL, 0, o, &on);
    if (r < 0 || on < 8) {
        return 0;
    }
    return (uint64_t)get32(o) | ((uint64_t)get32(o + 4) << 32);
}

/* ---- a screen on the other board --------------------------------------- */

/*
 * WHICH REMOTE SCREEN, and the honest answer is "the one".
 *
 * A class vtable is called with a handle the caller does not fill in -
 * display.c walks the registry and calls ops->blit_rect(0, b) - so a vtable
 * shared by several proxies cannot tell them apart.  Every local driver has
 * the same shape and does not notice, because it has one device.
 *
 * So: the first proxied display wins and a second is said out loud rather than
 * silently shadowing it.  Two boards with screens joined to one that has none
 * is a real arrangement and this is where it will need the handle to mean
 * something.
 */
static rdev_t *s_display_proxy;
static bool    s_display_real; /* it is a panel, not a buffer */

static rdev_t *proxy_of(ag_handle_t h)
{
    (void)h;
    return s_display_proxy;
}

/*
 * Opened on first use rather than at `link devs`.  Attaching a registry must
 * not open anything - see the note on why proxies do not appear at `link use`
 * - and a screen is used by the graphics layer, which never opens a device.
 */
/*
 * A FAR OPEN THAT FAILED IS NOT ASKED AGAIN AT ONCE (2 Oct 2026).  Every write
 * to a device whose session is gone opens it first, and the mixer writes a
 * period every few milliseconds: with the CYD answering -12 to every open the
 * S3 asked hundreds of times a second, each time under the device's own lock
 * and the one request in flight that every proxy shares, until nothing else
 * on the board got an answer - no console, not even Ctrl+C.  Now a failure
 * waits 250 ms before the next try, doubling to 8 s, and a success forgets it.
 */
#define RD_OPEN_BACKOFF_MIN_MS 250u
#define RD_OPEN_BACKOFF_MAX_MS 8000u

static ag_err_t ensure_open(rdev_t *p)
{
    if (p->far_handle >= 0) {
        return AG_OK;
    }
    const int64_t now = ag_port_us();
    if (p->open_backoff_ms != 0u && now < p->open_again_us) {
        return (ag_err_t)p->open_err;
    }
    const int32_t r = ask(p, RD_OPEN, 0, 0, 0, AG_LINK_CONTROL, p->name,
                          (uint16_t)strlen(p->name), NULL, NULL);
    if (r < 0) {
        p->open_backoff_ms =
            (p->open_backoff_ms == 0u) ? RD_OPEN_BACKOFF_MIN_MS
            : (p->open_backoff_ms * 2u > RD_OPEN_BACKOFF_MAX_MS)
                ? RD_OPEN_BACKOFF_MAX_MS
                : p->open_backoff_ms * 2u;
        p->open_again_us = ag_port_us() + (int64_t)p->open_backoff_ms * 1000;
        p->open_err = r;
        return (ag_err_t)r;
    }
    p->open_backoff_ms = 0u;
    p->far_handle = r;
    return AG_OK;
}


/*
 * THE GEOMETRY THAT TRAVELS WITH EVERY SLICE.  Twelve bytes, and the same
 * shape REMDISP puts on its wire, because it is the smallest description of
 * "these pixels belong here" that does not need the far end to remember
 * anything between messages.
 */
#define RD_BLIT_HDR 12u
/* What one poured message may carry on the wire that screen is on. */
static uint16_t rd_pour_payload(const rdev_t *p)
{
    ag_link_info_t li;
    uint32_t       room = RD_MAX_PAYLOAD;
    if (ag_linkprov_info_on(proxy_wire(p), &li) == AG_OK && li.mtu > RD_HDR) {
        room = li.mtu - RD_HDR;
    }
    if (room > RD_SCR_CAP) {
        room = RD_SCR_CAP;
    }
    if (room < RD_BLIT_HDR + 2u) {
        room = RD_BLIT_HDR + 2u;
    }
    return (uint16_t)room;
}

/* The message a blit is cut into: the display path is serialised by the
 * device registry, so one buffer, and not 2 KB on a caller's stack - nor in
 * the kernel's statics, where a board that never draws on a neighbour paid
 * for it and the S3 emulator build ran out of DRAM: made on the first blit. */
static uint8_t *s_blitmsg;

static rdev_t *proxy_of(ag_handle_t h);

static ag_err_t rd_gfxinfo(ag_handle_t h, ag_gfxinfo_t *out)
{
    rdev_t *p = proxy_of(h);
    if (p == NULL || out == NULL) {
        return -AG_ENODEV;
    }
    if (ensure_open(p) != AG_OK) {
        return -AG_ENODEV;
    }
    uint8_t  ans[16];
    uint16_t got = 0;
    const int32_t r = ask(p, RD_GFXINFO, (uint32_t)p->far_handle, 0, 0,
                          AG_LINK_CONTROL, NULL, 0, ans, &got);
    if (r < 0 || got < 12u) {
        return (r < 0) ? (ag_err_t)r : -AG_EIO;
    }
    memset(out, 0, sizeof(*out));
    out->width = get16(ans);
    out->height = get16(ans + 2);
    out->fmt = (ag_pixfmt_t)get16(ans + 4);
    out->stride = get32(ans + 6);
    out->direct = (ans[10] != 0);
    out->double_buf = (ans[11] != 0);
    return AG_OK;
}

/*
 * A painter's payload out: RD_BLITZ, reliable, on the painter's own task -
 * which is why it may wait for the wire, and why one buffer serves.
 */
static uint32_t s_pass_msgs_now; /* messages of the pass being sent */

static ag_err_t rd_send_z(uint32_t handle, const uint8_t *pay, uint16_t len)
{
    if (s_blitmsg == NULL) {
        s_blitmsg = (uint8_t *)ag_port_alloc(RD_HDR + RD_POUR_CAP, AG_MEM_BYTE);
        if (s_blitmsg == NULL) {
            return -AG_ENOMEM;
        }
    }
    if (len > RD_POUR_CAP) {
        return -AG_EINVAL;
    }
    uint8_t *const msg = s_blitmsg;
    msg[0] = RD_BLITZ;
    msg[1] = RD_F_NOREPLY;
    put16(msg + 2, 0);
    put32(msg + 4, handle);
    put32(msg + 8, 0);
    put32(msg + 12, 0);
    memcpy(msg + RD_HDR, pay, len);
    const rdev_t *const p = s_display_proxy;
    if (p == NULL) {
        return -AG_ENODEV;
    }
    const int32_t r = link_send(p->wire, p->node, AG_LINK_BULK, msg,
                                (uint16_t)(RD_HDR + len));
    if (r >= 0) {
        s_pass_msgs_now++;
    }
    return (r >= 0) ? AG_OK : (ag_err_t)r;
}

/* The far screen's pace (rdp_set_sync): passes in flight end to end. */
static volatile uint16_t s_paint_seq;
static volatile uint16_t s_paint_acked;
static volatile bool     s_paint_heard;  /* the far end has answered once */
static uint8_t           s_paint_silent; /* passes unanswered in a row */
static uint32_t          s_paint_waits;  /* passes that waited, for `link` */
static uint32_t          s_paint_timeouts, s_paint_unsent;
static int64_t           s_paint_wait_max;
/* messages each pass in flight took, by its number */
static uint8_t           s_pass_msgs[16];

/* Messages of the passes after `acked`, up to `seq`. */
static uint32_t msgs_in_flight(uint16_t seq, uint16_t acked)
{
    uint32_t n = 0;
    for (uint16_t k = (uint16_t)(acked + 1u), i = 0;
         (uint16_t)(seq - k) < 0x8000u && i < 16u; k++, i++) {
        n += s_pass_msgs[k % 16u];
        if (k == seq) {
            break;
        }
    }
    return n;
}

#define RD_PAINT_WAIT_MS 1000u
/* in a marker's last word: this pass ended its frame; the program that drew
 * it let the screen go (older receivers ignore both) */
#define RD_PASS_WHOLE 1u
#define RD_PASS_DONE  2u
#define RD_PAINT_LEGACY  3u

/*
 * PASSES IN FLIGHT: THREE, OR AS MANY AS THE FAR QUEUE HOLDS (28 Sep 2026).
 * One pass beyond the one being drawn made the picture's rate one pass per
 * round trip, and a file beside it stretched the trip from 10 ms to 17-19:
 * 8 whole frames a second where 14 went without it, with the CYD's drawing
 * waiting for data and half a core idle.  So up to three passes go ahead -
 * and since a pass of noise is some ten messages where a pass of balls is
 * two, never more than RD_PAINT_MSGS messages unanswered, which the far
 * picture queue (RD_SCR slots) takes without its reader waiting for room.
 * (Four and twenty filled it: the reader waited 33 times, and the sound
 * behind the picture came 218 ms late.)
 *
 * EIGHT AND SIXTY-FOUR NOW THAT THE WIRE ORDERS BY BUDGET (2 Oct 2026).
 * 8/64 with a send ring of 16 ms of the wire took a 16 Mbaud wire from 84% to
 * 91-94% under noise - and the sound behind the picture in that one queue
 * came in bursts: 83 periods dropped at the CYD in 40 s against 9 at 2/12.
 * The picture now waits in its own lane and sound (a shorter budget) goes
 * past it (wlink.c); this window is what lets the picture fill that lane.
 */
#define RD_PAINT_AHEAD 8u
#define RD_PAINT_MSGS  64u
/* the window as it is now: `link window <passes> <messages>` measures others */
static uint8_t s_paint_ahead = RD_PAINT_AHEAD, s_paint_msgs = RD_PAINT_MSGS;

void ag_noderdev_paint_window(uint8_t *ahead, uint8_t *msgs, bool set)
{
    if (set) {
        /* at least one, and fewer than the passes counted (s_pass_msgs) */
        s_paint_ahead = (*ahead < 1u) ? 1u : (*ahead > 15u) ? 15u : *ahead;
        s_paint_msgs = (*msgs >= 4u) ? *msgs : 4u;
    }
    *ahead = s_paint_ahead;
    *msgs = s_paint_msgs;
}

static void rd_sync_z(uint32_t handle)
{
    const rdev_t *const p = s_display_proxy;
    if (p == NULL) {
        return;
    }
    /*
     * NOT WAITING IS NOT GIVING UP.  A screen that has not answered yet is
     * still told every pass is over, and the first answer that comes back
     * turns the waiting on.  Giving up for good after three silent passes
     * sounded safe and was the whole failure: the first pass is the entire
     * picture, 150 KB through a 4 Mbaud wire and a relay, its answer came
     * after the second - and the painter went on at the wire's pace for ever,
     * gaps of 170-950 ms between passes, a moving square Maxim called torn
     * (26 Sep 2026).
     */
    const bool wait = s_paint_heard || s_paint_silent < RD_PAINT_LEGACY;
    uint16_t seq = (uint16_t)(s_paint_seq + 1u);
    if (seq == 0u) {
        seq = 1u;
    }
    s_paint_seq = seq;
    s_pass_msgs[seq % 16u] =
        (uint8_t)((s_pass_msgs_now < 255u) ? s_pass_msgs_now : 255u);
    s_pass_msgs_now = 0;
    uint8_t msg[RD_HDR];
    msg[0] = RD_PAINTED;
    msg[1] = RD_F_NOREPLY;
    put16(msg + 2, seq);
    put32(msg + 4, handle);
    put32(msg + 8, 0);
    /* the last pass of its frame: the far end counts the frames it drew
     * whole (older receivers leave this word alone) */
    put32(msg + 12, (rdp_pass_whole() ? RD_PASS_WHOLE : 0u) |
                        (rdp_pass_done() ? RD_PASS_DONE : 0u));
    if (link_send(p->wire, p->node, AG_LINK_BULK, msg, RD_HDR) < 0) {
        s_paint_unsent++;
        return;
    }
    if (!wait) {
        return; /* told; an answer to this one still turns the waiting on */
    }
    /*
     * ONE PASS IN FLIGHT.  Waiting for this pass's own answer put a whole
     * round trip - here, the middle board, the screen and back - between
     * every two passes, and with sound on the same wires that trip took up
     * to 275 ms: the square stalled whenever it did (26 Sep 2026).  The
     * painter now goes on once the pass BEFORE this one is answered, so the
     * trip is hidden behind a pass of drawing and the screen is still never
     * more than two passes behind.
     */
    s_paint_waits++;
    const int64_t t0 = ag_port_us();
    const int64_t until = t0 + (int64_t)RD_PAINT_WAIT_MS * 1000;
    bool          out = false;
    while ((uint16_t)(seq - s_paint_acked) > s_paint_ahead ||
           ((uint16_t)(seq - s_paint_acked) > 1u &&
            msgs_in_flight(seq, s_paint_acked) > s_paint_msgs)) {
        if (ag_port_us() >= until) {
            out = true;
            break;
        }
        ag_port_task_delay(1);
    }
    if (out) {
        s_paint_timeouts++;
    }
    if (ag_port_us() - t0 > s_paint_wait_max) {
        s_paint_wait_max = ag_port_us() - t0;
    }
    if (s_paint_acked != 0u &&
        (uint16_t)(seq - s_paint_acked) <= s_paint_ahead) {
        s_paint_heard = true;
        s_paint_silent = 0;
    } else if (s_paint_silent < 255u) {
        s_paint_silent++;
        if (!s_paint_heard && s_paint_silent == RD_PAINT_LEGACY) {
            ag_log(AG_LOG_INFO, TAG,
                   "%s does not say when a pass has arrived (an older "
                   "image?): painting at the pace of the wire until it does",
                   p->qualified);
        }
    }
}

/*
 * THE FALLBACK, for a board with no memory for the painter's two pictures: a
 * blit cut into pieces that fit.  Not reassembled at the far end: every piece
 * is a complete rectangle with its own geometry, so a piece that is lost costs
 * its own pixels and nothing else.
 */
static void rd_blit_raw(rdev_t *p, const ag_blit_t *b)
{

    const uint8_t *rows = (const uint8_t *)b->px;
    if (s_blitmsg == NULL) {
        s_blitmsg = (uint8_t *)ag_port_alloc(RD_HDR + RD_POUR_CAP, AG_MEM_BYTE);
        if (s_blitmsg == NULL) {
            return;
        }
    }
    uint8_t *const msg = s_blitmsg;
    const uint16_t pixroom = (uint16_t)((rd_pour_payload(p) - RD_BLIT_HDR) / 2u);

    /*
     * WHOLE ROWS WHEN A ROW FITS, so a slice is w by k rather than a strip of
     * one row: on a 2 KB message that is three rows of a 320-wide band in one
     * send instead of nine sends.  A row wider than a message is still cut
     * across, which is what a small wire needs.
     */
    const uint16_t per_rows =
        (b->w <= pixroom) ? (uint16_t)(pixroom / b->w) : 1u;
    const uint16_t per_cols = (b->w <= pixroom) ? b->w : pixroom;

    for (uint16_t r = 0; r < b->h; r = (uint16_t)(r + per_rows)) {
        uint16_t rh = (uint16_t)(b->h - r);
        if (rh > per_rows) {
            rh = per_rows;
        }
        for (uint16_t x = 0; x < b->w; x = (uint16_t)(x + per_cols)) {
            uint16_t n = (uint16_t)(b->w - x);
            if (n > per_cols) {
                n = per_cols;
            }
            msg[0] = RD_BLIT;
            msg[1] = RD_F_NOREPLY;
            put16(msg + 2, 0);
            put32(msg + 4, (uint32_t)p->far_handle);
            put32(msg + 8, 0);
            put32(msg + 12, 0);
            uint8_t *const q = msg + RD_HDR;
            put16(q + 0, (uint16_t)(b->x + x));
            put16(q + 2, (uint16_t)(b->y + r));
            put16(q + 4, n);
            put16(q + 6, rh);
            put16(q + 8, b->surf_w);
            put16(q + 10, b->surf_h);
            uint8_t *dst = q + RD_BLIT_HDR;
            for (uint16_t k = 0; k < rh; k++) {
                const uint16_t *src = (const uint16_t *)(const void *)(
                    rows + (size_t)(r + k) * b->stride);
                memcpy(dst, src + x, (size_t)n * 2u);
                dst += (size_t)n * 2u;
            }
            (void)link_send(
                p->wire, p->node, AG_LINK_STREAM, msg,
                (uint16_t)(RD_HDR + RD_BLIT_HDR + (uint32_t)n * rh * 2u));
        }
    }
}

/*
 * A BLIT IS NOTED, NOT SENT.  This runs under the device registry, from the
 * application's present, and sending here held the registry for as long as
 * the wire took.  The screen's painter (net/rdpaint.c) sends only what
 * changed since the far screen last showed it, packed, on its own task - so
 * nothing the application does knows the screen is across a wire, and a wire
 * slower than the application costs frames, not the registry.
 *
 * Measured before (24 Sep, SPI 40 MHz, noise at 320x240): 29.7 frames/s
 * raw.  Nothing is packed there - noise does not pack - so that number is
 * the floor this has to keep.
 */
/*
 * Blits handed to the neighbour's screen, and the ones that went nowhere:
 * no screen chosen, or the far end would not open it (the last error).  A
 * game's picture reached nothing and nothing here said so (30 Sep 2026).
 */
static uint32_t s_blit_calls, s_blit_noproxy, s_blit_noopen, s_blit_raw;
static int32_t  s_blit_open_err;

void ag_noderdev_blit_counts(uint32_t *calls, uint32_t *noproxy,
                             uint32_t *noopen, int32_t *open_err,
                             uint32_t *raw)
{
    *calls = s_blit_calls;
    *noproxy = s_blit_noproxy;
    *noopen = s_blit_noopen;
    *open_err = s_blit_open_err;
    *raw = s_blit_raw;
}

static void rd_blit(ag_handle_t h, const ag_blit_t *b)
{
    rdev_t *p = proxy_of(h);
    if (b == NULL || b->px == NULL || b->w == 0 || b->h == 0) {
        return;
    }
    s_blit_calls++;
    if (p == NULL) {
        s_blit_noproxy++;
        return;
    }
    const ag_err_t oe = ensure_open(p);
    if (oe != AG_OK) {
        if (s_blit_noopen++ == 0u) {
            ag_log(AG_LOG_WARN, TAG, "%s would not open (%d): the picture "
                   "goes nowhere until it does", p->name, (int)oe);
        }
        s_blit_open_err = (int32_t)oe;
        return;
    }
    rdp_set_sync(rd_sync_z);
    if (!rdp_damage(b, (uint32_t)p->far_handle, rd_pour_payload(p),
                    rd_send_z)) {
        s_blit_raw++;
        rd_blit_raw(p, b); /* no memory for the painter: every pixel, raw */
    }
}

static ag_err_t rd_textinfo(ag_handle_t h, uint16_t *cols, uint16_t *rows)
{
    rdev_t *p = proxy_of(h);
    if (p == NULL) {
        return -AG_ENODEV;
    }
    if (ensure_open(p) != AG_OK) {
        return -AG_ENODEV;
    }
    uint8_t  ans[8];
    uint16_t got = 0;
    const int32_t r = ask(p, RD_TEXTINFO, (uint32_t)p->far_handle, 0, 0,
                          AG_LINK_CONTROL, NULL, 0, ans, &got);
    if (r < 0 || got < 4u) {
        return (r < 0) ? (ag_err_t)r : -AG_EIO;
    }
    if (cols != NULL) {
        *cols = get16(ans);
    }
    if (rows != NULL) {
        *rows = get16(ans + 2);
    }
    return AG_OK;
}

static void rd_textrow(ag_handle_t h, uint16_t row, const ag_textcell_t *cells,
                       uint16_t count)
{
    rdev_t *p = proxy_of(h);
    if (p == NULL || cells == NULL || count == 0) {
        return;
    }
    if (ensure_open(p) != AG_OK) {
        return;
    }
    uint16_t max = (uint16_t)((RD_MAX_PAYLOAD - 4u) / sizeof(ag_textcell_t));
    if (count > max) {
        count = max;
    }
    uint8_t msg[RD_HDR + RD_MAX_PAYLOAD];
    msg[0] = RD_TEXTROW;
    msg[1] = RD_F_NOREPLY;
    put16(msg + 2, 0);
    put32(msg + 4, (uint32_t)p->far_handle);
    put32(msg + 8, 0);
    put32(msg + 12, 0);
    put16(msg + RD_HDR, row);
    put16(msg + RD_HDR + 2, count);
    memcpy(msg + RD_HDR + 4, cells, (size_t)count * sizeof(ag_textcell_t));
    (void)link_send(
        p->wire, p->node, AG_LINK_STREAM, msg,
        (uint16_t)(RD_HDR + 4u + count * sizeof(ag_textcell_t)));
}

static void rd_textcur(ag_handle_t h, uint16_t col, uint16_t row,
                       ag_textcell_t under, bool visible)
{
    rdev_t *p = proxy_of(h);
    if (p == NULL) {
        return;
    }
    if (ensure_open(p) != AG_OK) {
        return;
    }
    uint8_t msg[RD_HDR + 8];
    msg[0] = RD_TEXTCUR;
    msg[1] = RD_F_NOREPLY;
    put16(msg + 2, 0);
    put32(msg + 4, (uint32_t)p->far_handle);
    put32(msg + 8, 0);
    put32(msg + 12, 0);
    put16(msg + RD_HDR, col);
    put16(msg + RD_HDR + 2, row);
    msg[RD_HDR + 4] = visible ? 1u : 0u;
    /* The cell under the caret travels with it: a caret drawn as a blank rubs
     * out the character it is standing on. */
    msg[RD_HDR + 5] = under.ch;
    msg[RD_HDR + 6] = under.attr;
    (void)link_send(p->wire, p->node, AG_LINK_STREAM, msg,
                    (uint16_t)(RD_HDR + 7u));
}

static const ag_display_ops_t k_proxy_display_ops = {
    /*
     * THE SIZE IS NOT DECORATION.  AG_HAS asks whether the table is long
     * enough to contain the member before it looks at it, so a table that
     * leaves this zero has NO members at all as far as every caller is
     * concerned - which is a screen that is registered, listed, and walked
     * straight past.
     */
    .size = sizeof(ag_display_ops_t),
    .info = rd_gfxinfo,
    .blit_rect = rd_blit,
    .text_info = rd_textinfo,
    .text_row = rd_textrow,
    .text_cursor = rd_textcur,
};

static const ag_dev_ops_t k_proxy_ops = {
    .open = rd_open,
    .close = rd_close,
    .read = rd_read,
    .write = rd_write,
    .ioctl = rd_ioctl,
    .size = rd_size,
};

/* ---- serving the far node ----------------------------------------------- */

/*
 * Who asked, and down which wire: taken on the reader when the request
 * arrives, because the worker that answers it is not the reader and would
 * otherwise answer down the selected wire, to the selected node.
 */
static int32_t  s_req_wire = -1;
static uint16_t s_req_from;

static void reply_to(int32_t wire, uint16_t dst, uint8_t op, uint16_t tag,
                     int32_t status, uint8_t cls, const void *pay,
                     uint16_t plen)
{
    const ag_err_t e = send_raw(wire, dst, (uint8_t)(op | RD_REPLY), 0, tag,
                                (uint32_t)status, 0, 0, cls, pay, plen);
    if (e != AG_OK) {
        /*
         * Said, because the shape of this failure is deceptive: the asking node
         * sees a timeout and blames the wire, while the truth is that this node
         * answered and the answer could not be sent.  One is a lost frame, the
         * other is a full window or a link that has gone - and they are fixed
         * in different places.
         */
        ag_log(AG_LOG_WARN, TAG, "op %u answered %d but the reply did not go "
                                 "out: %d",
               (unsigned)op, (int)status, (int)e);
    }
}

static void reply(uint8_t op, uint16_t tag, int32_t status, uint8_t cls,
                  const void *pay, uint16_t plen)
{
    reply_to(s_req_wire, s_req_from, op, tag, status, cls, pay, plen);
}

/*
 * What the far node has open here.  Small on purpose: this is one neighbour's
 * use of this node's devices, not a general handle table.
 */
#define RD_MAX_SESSIONS 8
static ag_device_t *s_session[RD_MAX_SESSIONS];
/* who opened each: the asking node's name hash (see RD_OPEN) */
static uint16_t s_session_from[RD_MAX_SESSIONS];
/* A sound session's channels, from the SETFMT it was opened with: what a
 * frame is, when frames have to be taken out (see keep_pace). */
static uint8_t s_session_ch[RD_MAX_SESSIONS];

/* The request being served, and the worker serving it. */
static uint8_t        s_req[RD_HDR + RD_MAX_PAYLOAD] AG_PORT_SLOW_BSS;
static volatile uint16_t s_req_len;
static volatile bool  s_working;
static ag_port_task_t s_worker;
/*
 * ONE WORKER, MADE AT START-UP (2 Oct 2026).  It was made for each request
 * and gone after it - 4 KB of stack asked for at the moment a neighbour
 * wanted something, and on the CYD, once its picture and sound had their
 * places, the largest piece left was 4 KB or less: every open from the S3
 * answered -12, sound and screen alike, until a restart.  Made once while
 * there is memory, it waits on s_req_sem; a board where it could not be made
 * falls back to one a request.  Its stack is in PSRAM where there is any
 * (ag_proc_task_create_slow): in internal memory it held 4 KB for good and
 * the emulated S3 then had too little left for a program's threads.  Where
 * there is no PSRAM, only on a board that is a screen or a speaker to its
 * neighbours (the emulated C3 held the 4 KB for nothing).
 */
static ag_port_sem_t  s_req_sem;
static bool           s_standing;

/* ---- receiving what was poured ------------------------------------------ */

/*
 * FOUR periods, and a task that stands rather than one that is made.
 *
 * A request that expects an answer gets a task of its own, which is affordable
 * because it happens when somebody asks.  A poured period arrives every ten
 * milliseconds for as long as there is sound: a task each would be a hundred
 * tasks a second, and a server that took them one at a time would refuse most
 * of them.  So one task waits here, and a small ring absorbs the difference
 * between a wire that delivers in bursts and a converter that consumes evenly.
 *
 * Ten, not four.  Four was "about forty milliseconds" of 10 ms periods, but a
 * period is not what arrives: the link packs messages into its frames, and a
 * 2 KB frame on the C3-CYD wire carries seven 256-byte periods at once.  Four
 * slots held three of them and dropped the rest - exactly a third of the
 * C3's sound on the CYD, 1 720 of 5 168 periods, twice, with the wire
 * perfect (25 Sep 2026).  Ten holds a whole frame's worth and a little more;
 * 20 KB, taken only on a board that something is poured at.
 *
 * FULL MEANS THE NEWEST IS DROPPED, not the oldest, and that is what makes this
 * safe without a lock: only the reader task moves the write end and only the
 * pouring task moves the read end.  It is also the honest choice - a period
 * that cannot be taken now has missed its moment, which is the same rule the
 * sending side follows.
 */
/*
 * TWO QUEUES, ONE FOR SOUND AND ONE FOR THE PICTURE (26 Sep 2026).
 *
 * They were one ring of ten, taken strictly in order by one task, and a sound
 * period waits for the converter to have room - so the picture waited behind
 * the sound: the sender keeps 30 ms of sound ahead, five periods, and every
 * rectangle arriving behind them waited for all five to play.  The ring sat
 * at 9 of 10, the reader waited for it, the wire behind the reader, and the
 * square on the CYD ran at 30 passes a second where it runs 62 without
 * sound.  Now each has its own queue out of the same slots, and the task
 * draws whatever picture there is whenever the converter has no room.
 */
/* 14 periods of sound, 40 ms at 44.1 kHz - the sender keeps 30 ms ahead -
 * not 18: the CYD, which has no PSRAM, had no room left for its picture
 * once sound had its places (2 Oct 2026) */
#define RD_SND  14
/*
 * 8 pieces of picture, not 28 (2 Oct 2026): 28 of 640 bytes on the CYD,
 * which has no PSRAM, left it no room for its own picture's strips nor for
 * the worker a request from the S3 is served in ("lcd0 would not open (-12)")
 * once sound had its places too.  The CYD draws faster than the wire brings
 * a picture: its reader waited for room 0-2 times in 16 s with 28.
 */
#define RD_SCR  8
/* Where there is PSRAM for them, the places the pair had before 2 Oct: 45 ms
 * of sound and 28 pieces of picture.  Chosen by what the board has at
 * start-up (ring_make), never by which board it is. */
#define RD_SND_BIG 18
#define RD_SCR_BIG 28

typedef struct {
    /*
     * Into s_ring_mem, which is made the first time anything is poured at this
     * board.  Every board carries this file, the C3 has no PSRAM, and a board
     * that is never somebody else's screen or speaker should not pay 8 KB of
     * internal memory for the possibility.
     */
    uint8_t *data;
    uint16_t len;
    uint32_t handle;
    uint32_t off;
    /*
     * WHICH OPERATION THIS IS, and leaving it out was a real defect rather
     * than an omission.  Everything poured used to be written at the device
     * with ag_dev_write, because everything poured was an audio period - so
     * when a screen's blits started arriving the same way they were written at
     * the panel as raw bytes.  Against the far board's SOFT framebuffer that
     * SUCCEEDED (it takes bytes), and the counters read "1 019 808 bytes, 0
     * lost" while nothing was ever drawn; against its real ST7789, which has
     * no write, every one was counted lost.  Both readings were wrong in the
     * same way: the message was never dispatched by what it says it is.
     */
    uint8_t  op;
    /* where it came from: a pass's marker is answered from the queue */
    int32_t  wire;
    uint16_t from;
    uint16_t tag;
} rd_period_t;

/* Said once: a flood of identical warnings is a flood, not a diagnosis. */
static bool              s_no_sink_said;

/*
 * What was drawn on this glass this second besides the painter's picture:
 * raw rectangles and text, poured and asked for.  A neighbour's console on
 * the glass beside its painter draws over the picture, and the painter does
 * not know (26 Sep 2026).
 */
static uint32_t s_scr_raw, s_scr_text, s_scr_asked;

static uint32_t serve_poured_class(ag_device_t *dev,
                                   const rd_period_t *pd);
static void     glass_back_if_quiet(void);
static void     glass_back(const char *why);
static void     screen_report(void);
static void     screen_lost(void);

typedef struct {
    rd_period_t      *e;
    uint32_t          n;    /* slots; it holds n - 1                       */
    volatile uint32_t w, r;
} rd_q_t;

/* made with s_ring_mem: forty-six slots are 1.3 KB of static memory the S3
 * does not have spare */
static rd_period_t   *s_ring;
static rd_q_t         s_snd = {NULL, RD_SND, 0, 0};
static rd_q_t         s_scr = {NULL, RD_SCR, 0, 0};
static uint8_t       *s_ring_mem;
static ag_port_task_t s_pour_task;

static uint32_t q_used(const rd_q_t *q)
{
    return (q->w + q->n - q->r) % q->n;
}

/*
 * THE COUNT THAT MEANS SOMETHING.
 *
 * A poured write returns success on the sending node without evidence - there
 * is nothing to have evidence of, since nothing comes back.  So "all the bytes
 * were taken" over there is not a fact about this wire, and a test that read it
 * would pass with the far node switched off.  These are counted where the bytes
 * actually landed, and said out loud when the sound stops.
 */
static uint32_t s_pour_periods;
static uint32_t s_pour_bytes;
static uint32_t s_pour_lost;
/*
 * WHEN AND WHY, the first few (29 Sep 2026): Maxim hears clicks at the start
 * and none after, and both rooms lost some fifteen periods in five minutes -
 * a count that cannot say whether they were the start.  Milliseconds since
 * the first period poured here, and the reason.
 */
enum {
    RD_LOST_NOTHING = 1, /* a piece of picture that drew nothing */
    RD_LOST_NODEV,       /* no open device for its handle yet */
    RD_LOST_SHORT,       /* the converter took only part of it in 40 ms */
    RD_LOST_NOMEM,       /* no memory for the ring or its task */
    RD_LOST_FULL,        /* the queue was full */
};
#define RD_LOST_KEEP 8u
static int64_t  s_pour_first_us;
static uint32_t s_lost_at[RD_LOST_KEEP];
static uint8_t  s_lost_why[RD_LOST_KEEP];
static uint32_t s_lost_n;

static void pour_lost(uint8_t why)
{
    s_pour_lost++;
    if (s_lost_n < RD_LOST_KEEP) {
        const int64_t now = ag_port_us();
        s_lost_at[s_lost_n] =
            (s_pour_first_us != 0) ? (uint32_t)((now - s_pour_first_us) / 1000)
                                   : 0u;
        s_lost_why[s_lost_n] = why;
        s_lost_n++;
    }
}

uint32_t ag_noderdev_lost_at(uint32_t *at_ms, uint8_t *why, uint32_t cap)
{
    const uint32_t n = (s_lost_n < cap) ? s_lost_n : cap;
    for (uint32_t i = 0; i < n; i++) {
        at_ms[i] = s_lost_at[i];
        why[i] = s_lost_why[i];
    }
    return n;
}
/* of those: a full ring on arrival, and a converter that stayed full */
static uint32_t s_pour_full;
static uint32_t s_pour_short;
/* frames taken out to keep up with this converter (keep_pace) */
static uint32_t s_pour_paced;
static uint32_t s_pour_said_ms;
static uint32_t s_pour_said_periods;
static uint32_t s_pour_said_lost;
static uint32_t s_pour_last_ms;
static bool     s_pour_said;
/* of the pieces not drawn: no device for the handle, and nothing drawn -
 * with the last such piece's op and length, for link */
static uint32_t s_scr_nodev, s_scr_nopx;
static uint8_t  s_scr_last_op;
static uint16_t s_scr_last_len;

/*
 * A NEIGHBOUR THAT RESTARTED LEAVES ITS SESSIONS HERE (6 Oct 2026).  Every
 * handle it held died with it, and nothing closed them on this side: three
 * restarts of the S3 against a CYD left up for 79 minutes filled all eight
 * slots, and the S3's screen open came back -ENFILE 2575 times - Fallout ran
 * blind.  Only an exclusive device opened again by the same node was caught
 * (A SESSION ITS ASKER HAS LOST, below).  Now a neighbour saying its name anew
 * marks its node here, and the worker closes all it had before its next
 * request - on the worker, as RD_CLOSE does, not on the reader that heard it.
 */
/* one slot a wire, then one a node beyond a neighbour (far_heard) */
#define RD_REBORN_SLOTS 32
static volatile uint16_t s_reborn[RD_REBORN_SLOTS];
static volatile uint32_t s_reborn_bits;

static void reborn_mark(int32_t slot, uint16_t node)
{
    if (slot < 0 || slot >= RD_REBORN_SLOTS) {
        return;
    }
    s_reborn[slot] = node;
    s_reborn_bits |= 1u << (uint32_t)slot;
}

static void close_reborn(void)
{
    while (s_reborn_bits != 0u) {
        const uint32_t w = (uint32_t)__builtin_ctz(s_reborn_bits);
        const uint16_t node = s_reborn[w];
        s_reborn_bits &= ~(1u << w);
        for (int i = 0; i < RD_MAX_SESSIONS; i++) {
            ag_device_t *const dev = s_session[i];
            if (dev != NULL && s_session_from[i] == node) {
                ag_log(AG_LOG_INFO, TAG,
                       "%s: its asker restarted - closing the session it had",
                       dev->name);
                s_session[i] = NULL;
                (void)ag_dev_close(dev);
            }
        }
    }
}

static void serve_one(const uint8_t *b, uint16_t n)
{
    close_reborn();
    const uint8_t  op = b[0];
    const uint16_t tag = get16(b + 2);
    const uint32_t h = get32(b + 4);
    const uint32_t a = get32(b + 8);
    const uint32_t off = get32(b + 12);
    const uint8_t *pay = b + RD_HDR;
    const uint16_t plen = (uint16_t)(n - RD_HDR);
    uint8_t        obuf[RD_MAX_PAYLOAD];

    if (op == RD_OPEN) {
        char name[AG_DEV_NAME_MAX];
        uint16_t k = plen;
        if (k >= sizeof(name)) {
            k = (uint16_t)(sizeof(name) - 1u);
        }
        memcpy(name, pay, k);
        name[k] = '\0';

        ag_device_t *dev = ag_dev_find(name);
        if (dev == NULL) {
            reply(op, tag, -AG_ENODEV, AG_LINK_CONTROL, NULL, 0);
            return;
        }
        /*
         * A SESSION ITS ASKER HAS LOST.  An exclusive device opened again by
         * the node that already holds it here: that node's own registry lets
         * one holder of the proxy at a time, so the first session is one it no
         * longer knows about - its open timed out while the answer was on the
         * way (a busy middle board, 26 Sep 2026), or it restarted.  Held on to,
         * it made the Zero's converter "busy" (-16) for every open after, for
         * good.  Closed here, and the new open goes ahead.
         */
        if ((dev->flags & AG_DEVF_EXCLUSIVE) != 0u) {
            for (int i = 0; i < RD_MAX_SESSIONS; i++) {
                if (s_session[i] == dev && s_session_from[i] == s_req_from) {
                    ag_log(AG_LOG_INFO, TAG,
                           "%s: its asker opens it again - closing the session "
                           "it lost",
                           dev->name);
                    s_session[i] = NULL;
                    (void)ag_dev_close(dev);
                }
            }
        }
        int slot = -1;
        for (int i = 0; i < RD_MAX_SESSIONS; i++) {
            if (s_session[i] == NULL) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            reply(op, tag, -AG_ENFILE, AG_LINK_CONTROL, NULL, 0);
            return;
        }
        const ag_err_t e = ag_dev_open(dev, a);
        if (e != AG_OK) {
            reply(op, tag, (int32_t)e, AG_LINK_CONTROL, NULL, 0);
            return;
        }
        s_session[slot] = dev;
        s_session_from[slot] = s_req_from;
        reply(op, tag, slot, AG_LINK_CONTROL, NULL, 0);
        return;
    }

    if (h >= (uint32_t)RD_MAX_SESSIONS || s_session[h] == NULL) {
        reply(op, tag, -AG_EINVAL, AG_LINK_CONTROL, NULL, 0);
        return;
    }
    ag_device_t *dev = s_session[h];

    switch (op) {
    case RD_CLOSE:
        s_session[h] = NULL;
        reply(op, tag, (int32_t)ag_dev_close(dev), AG_LINK_CONTROL, NULL, 0);
        break;
    case RD_READ: {
        uint32_t want = a;
        if (want > RD_MAX_PAYLOAD) {
            want = RD_MAX_PAYLOAD;
        }
        const int32_t r = ag_dev_read(dev, obuf, want, off);
        /* The answer to a question, so the reliable class - same reason as the
         * write above, in the other direction. */
        reply(op, tag, r, AG_LINK_CONTROL, obuf,
              (uint16_t)((r > 0) ? r : 0));
        break;
    }
    case RD_WRITE:
        reply(op, tag, ag_dev_write(dev, pay, plen, off), AG_LINK_CONTROL,
              NULL, 0);
        break;
    case RD_IOCTL: {
        uint16_t k = plen;
        if (k > sizeof(obuf)) {
            k = (uint16_t)sizeof(obuf);
        }
        memcpy(obuf, pay, k);
        const ag_err_t e = ag_dev_ioctl(dev, a, obuf, k);
        if (e == AG_OK && a == AG_IOC_AUDIO_SETFMT && k >= sizeof(ag_audio_fmt_t) &&
            h < (uint32_t)RD_MAX_SESSIONS) {
            s_session_ch[h] = ((const ag_audio_fmt_t *)obuf)->channels;
        }
        reply(op, tag, (int32_t)e, AG_LINK_CONTROL, obuf, k);
        break;
    }
    case RD_SIZE: {
        const uint64_t sz = ag_dev_size(dev);
        put32(obuf, (uint32_t)sz);
        put32(obuf + 4, (uint32_t)(sz >> 32));
        reply(op, tag, 0, AG_LINK_CONTROL, obuf, 8);
        break;
    }
    case RD_GFXINFO: {
        const ag_display_ops_t *d = (const ag_display_ops_t *)dev->class_ops;
        ag_gfxinfo_t            gi;
        if (d == NULL || d->info == NULL || d->info(0, &gi) != AG_OK) {
            reply(op, tag, -AG_ENOTSUP, AG_LINK_CONTROL, NULL, 0);
            break;
        }
        put16(obuf + 0, gi.width);
        put16(obuf + 2, gi.height);
        put16(obuf + 4, (uint16_t)gi.fmt);
        put32(obuf + 6, gi.stride);
        obuf[10] = gi.direct ? 1u : 0u;
        obuf[11] = gi.double_buf ? 1u : 0u;
        reply(op, tag, 0, AG_LINK_CONTROL, obuf, 12);
        break;
    }
    case RD_BLIT: {
        /*
         * Poured, so there is nothing to answer and nothing to retransmit: a
         * slice that does not arrive is a rectangle of pixels that stays as it
         * was.  Each one carries its own geometry, so nothing is remembered
         * between them.
         */
        const ag_display_ops_t *d = (const ag_display_ops_t *)dev->class_ops;
        if (d == NULL || d->blit_rect == NULL || plen < RD_BLIT_HDR) {
            break;
        }
        const ag_blit_t bl = {
            .px = pay + RD_BLIT_HDR,
            .stride = (uint32_t)get16(pay + 4) * 2u,
            .surf_w = get16(pay + 8),
            .surf_h = get16(pay + 10),
            .x = get16(pay + 0),
            .y = get16(pay + 2),
            .w = get16(pay + 4),
            .h = get16(pay + 6),
        };
        if ((uint32_t)bl.w * bl.h * 2u <= (uint32_t)(plen - RD_BLIT_HDR)) {
            d->blit_rect(0, &bl);
            s_scr_asked++;
        }
        break;
    }
    case RD_TEXTINFO: {
        const ag_display_ops_t *d = (const ag_display_ops_t *)dev->class_ops;
        uint16_t                cols = 0, rows = 0;
        if (d == NULL || d->text_info == NULL ||
            d->text_info(0, &cols, &rows) != AG_OK) {
            reply(op, tag, -AG_ENOTSUP, AG_LINK_CONTROL, NULL, 0);
            break;
        }
        put16(obuf + 0, cols);
        put16(obuf + 2, rows);
        reply(op, tag, 0, AG_LINK_CONTROL, obuf, 4);
        break;
    }
    case RD_TEXTROW: {
        const ag_display_ops_t *d = (const ag_display_ops_t *)dev->class_ops;
        if (d == NULL || d->text_row == NULL || plen < 4u) {
            break;
        }
        const uint16_t row = get16(pay);
        uint16_t       n = get16(pay + 2);
        const uint16_t room =
            (uint16_t)((plen - 4u) / sizeof(ag_textcell_t));
        if (n > room) {
            n = room;
        }
        d->text_row(0, row, (const ag_textcell_t *)(const void *)(pay + 4), n);
        s_scr_asked++;
        break;
    }
    case RD_TEXTCUR: {
        const ag_display_ops_t *d = (const ag_display_ops_t *)dev->class_ops;
        if (d == NULL || d->text_cursor == NULL || plen < 5u) {
            break;
        }
        ag_textcell_t under = {.ch = ' ', .attr = 0};
        if (plen >= 7u) {
            under.ch = pay[5];
            under.attr = pay[6];
        }
        d->text_cursor(0, get16(pay), get16(pay + 2), under, pay[4] != 0u);
        s_scr_asked++;
        break;
    }
    default:
        reply(op, tag, -AG_ENOTSUP, AG_LINK_CONTROL, NULL, 0);
        break;
    }
}

static void worker(void *arg)
{
    (void)arg;
    serve_one(s_req, s_req_len);
    s_working = false;
    ag_port_task_delete(NULL);
}

static void standing_worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (!ag_port_sem_take(s_req_sem, ag_port_ms_to_ticks(1000))) {
            continue;
        }
        if (s_working) {
            serve_one(s_req, s_req_len);
            s_working = false;
        }
    }
}

static void worker_make(void)
{
    if (s_standing) {
        return;
    }
    if (s_req_sem == NULL) {
        s_req_sem = ag_port_sem_new_binary();
    }
    if (s_req_sem == NULL) {
        return;
    }
    /* PSRAM first where there is any: it lives as long as the board */
    s_standing = ag_proc_task_create_slow(standing_worker, "rdevsrv", 4096,
                                          NULL, 1, AG_PORT_ANY_CORE,
                                          &s_worker);
}

/*
 * Says what arrived, once per spell of sound rather than once per period.
 *
 * Per period would be a hundred lines a second into a four-kilobyte journal,
 * which would bury the thing it is there to show.  At the end of a burst it is
 * one line, it is the count kept on the side that received the bytes, and it is
 * the only number in this path that is evidence of anything.
 */
void ag_noderdev_pour_counts(uint32_t *periods, uint32_t *lost,
                             uint32_t *full, uint32_t *paced)
{
    *periods = s_pour_periods;
    *lost = s_pour_lost;
    *full = s_pour_full;
    *paced = s_pour_paced;
}

static void pour_report(void)
{
    /*
     * ANYTHING to report, not only successes.
     *
     * This asked for `s_pour_periods == 0` and returned - so a pour in which
     * EVERY period was lost said nothing at all, and the one case the counter
     * exists for was the one case it stayed quiet in.  On the bench that read
     * as "the sound never arrived", while the periods were arriving and being
     * dropped one by one for a reason nobody printed.
     */
    if (s_pour_periods == 0 && s_pour_lost == 0) {
        return;
    }
    const uint32_t now = (uint32_t)(ag_port_us() / 1000);
    if ((uint32_t)(now - s_pour_last_ms) < 400u) {
        return;
    }
    /*
     * SAID AGAIN WHEN THE NUMBERS HAVE MOVED, and once was not enough.
     *
     * A burst of sound ends and one line at the end of it is the whole story.
     * A SCREEN never ends: the pixels keep coming for as long as something is
     * drawing, and a line printed after the first period and then never again
     * reported "1 period, 160 bytes" for an eight-second run - which reads as
     * a transport that delivered almost nothing, when it had delivered
     * everything. There is no number anywhere else that says how much arrived.
     *
     * So the line repeats when the counters have actually moved, and no faster
     * than once a second, which is a journal that can be read rather than a
     * journal that is a flood.
     */
    if (s_pour_said && (uint32_t)(now - s_pour_said_ms) < 1000u) {
        return;
    }
    if (s_pour_said && s_pour_periods == s_pour_said_periods &&
        s_pour_lost == s_pour_said_lost) {
        return;
    }
    ag_log(AG_LOG_INFO, TAG,
           "poured %u periods, %u bytes, %u lost (%u to a full ring, %u to a "
           "full converter), %u frames paced out",
           (unsigned)s_pour_periods, (unsigned)s_pour_bytes,
           (unsigned)s_pour_lost, (unsigned)s_pour_full,
           (unsigned)s_pour_short, (unsigned)s_pour_paced);
    s_pour_said = true;
    s_pour_said_ms = now;
    s_pour_said_periods = s_pour_periods;
    s_pour_said_lost = s_pour_lost;
}

/*
 * TWO CLOCKS, AND THIS BOARD'S IS THE ONE THAT PLAYS.
 *
 * The sender makes sound at its own clock's rate and this converter plays it
 * at another's; they are never quite the same, and on the CYD they are not
 * close: its DAC, asked for 22 050 Hz near the bottom of what its divider
 * reaches, plays about 16% slow.  The ring then fills steadily and whole
 * periods were thrown away at the full end - a regular hole a person hears as
 * a rhythm (811 of 5 168 periods, 25 Sep 2026).
 *
 * So the queue sets the pace: past half full, frames are taken out evenly
 * through the period - one in eight, one in five past three quarters - which
 * moves the pitch by that much for as long as it lasts instead of cutting the
 * sound.  Frames, not bytes: a stereo session loses both channels of a frame.
 * Only for a session whose format this board was told.
 */
static uint32_t keep_pace(rd_period_t *pd)
{
    /*
     * NOTHING WAS TAKEN OUT OF A PERIOD from 27 Sep 2026 (Maxim: "не надо
     * ужимать звук"): this had squeezed a period by up to 5% whenever the
     * queue stood above half, which bent a steady tone, and before that cut
     * the last frame out of every period near full - a grain.  A period that
     * found the queue full was dropped whole instead.
     *
     * AND THAT WAS A CLICK EVERY THIRTEEN SECONDS (29 Sep 2026).  With the
     * picture's holes gone the losses could be timed: none for the first
     * hundred seconds, then one period every 13-14 s on the Zero and on the
     * CYD, every one of them to a full queue.  The C3 makes sound some 0.08%
     * faster than either converter plays it; the queue filled in a hundred
     * seconds and then overflowed by one period a period's worth of drift.
     * Two crystals never agree, so something has to give.
     *
     * So past three quarters full a period is made ONE frame shorter, drawn
     * again over its whole length by straight lines between its frames - no
     * jump anywhere in it, 0.4% higher for that one period - and nothing
     * else changes.  It runs about one period in five, which is the 0.08%
     * the clocks disagree by.  Only for a session whose format this board
     * was told, and 16-bit mono or stereo.
     */
    const uint8_t ch = (pd->handle < (uint32_t)RD_MAX_SESSIONS)
                           ? s_session_ch[pd->handle]
                           : 0u;
    if (ch == 0u || ch > 2u || pd->op != RD_WRITE ||
        q_used(&s_snd) * 4u < (s_snd.n - 1u) * 3u) {
        return pd->len;
    }
    const uint32_t frames = pd->len / (ch * 2u);
    if (frames < 16u) {
        return pd->len;
    }
    int16_t *const s = (int16_t *)(void *)pd->data;
    const uint32_t out = frames - 1u;
    /* frame j of the shorter period is read at j * (frames-1) / (out-1) of
     * the longer, in 16.16: never behind j, so in place, front to back */
    const uint32_t step =
        (uint32_t)(((uint64_t)(frames - 1u) << 16) / (out - 1u));
    for (uint32_t j = 0; j < out; j++) {
        const uint32_t p = j * step;
        const uint32_t i = p >> 16;
        const int32_t  fr = (int32_t)(p & 0xFFFFu);
        if (i >= frames - 1u) {
            for (uint32_t c = 0; c < ch; c++) {
                s[j * ch + c] = s[(frames - 1u) * ch + c]; /* the last */
            }
            continue;
        }
        for (uint32_t c = 0; c < ch; c++) {
            const int32_t a = s[i * ch + c];
            const int32_t b = s[(i + 1u) * ch + c];
            s[j * ch + c] = (int16_t)(a + (((b - a) * fr) >> 16));
        }
    }
    s_pour_paced++;
    return out * ch * 2u;
}

/* The period being written, and how far: kept between laps, since the task
 * draws the picture while the converter is full instead of waiting. */
static uint32_t s_snd_len;
static uint32_t s_snd_done;
static uint32_t s_snd_waited;
static bool     s_snd_begun;

static void pour_pass_drawn(const rd_period_t *pd);
/* since when (ms, 0 for not) the pour task has had nothing to draw while
 * pixels were held */
static uint32_t s_idle_since;
#define RD_IDLE_DRAW_MS 20u
/*
 * 1: a pass is answered once drawn; 0: on arrival.  Measured both ways on 27
 * Sep 2026, a minute of sound and picture on the CYD: 1.7% of its sound
 * periods lost when answered once drawn, 0.5% on arrival - the painter paced
 * by the glass sent its passes in bursts the sound then waited behind.
 * Answering once drawn only while the glass was behind was worse again:
 * 79 lost against 26.  On
 * arrival, with a pass in flight and passes of at most RDP_PASS_MAX_PX, the
 * picture queue does not stay full anyway.
 */
#define RD_ACK_WHEN_DRAWN 1

/* One entry of the picture's queue, drawn. */
static void pour_screen_one(void)
{
    rd_period_t *pd = &s_scr.e[s_scr.r];
    if (pd->op == RD_PAINTED) {
        if ((pd->off & (RD_PASS_WHOLE | RD_PASS_DONE)) != 0u) {
            /* a frame is on the glass whole when it says so */
            rdp_draw_flush(RDP_FL_FRAME);
        }
        pour_pass_drawn(pd);
        if ((pd->off & RD_PASS_DONE) != 0u) {
            glass_back("the neighbour's program let it go");
        }
        s_scr.r = (s_scr.r + 1u) % s_scr.n;
        return;
    }
    ag_device_t *dev = (pd->handle < (uint32_t)RD_MAX_SESSIONS)
                           ? s_session[pd->handle]
                           : NULL;
    /*
     * A class call rather than bytes: the pixels of a rectangle, a row of
     * characters, the caret.  It goes to the device's class vtable, and it is
     * here rather than on the reader task for the same reason a write is -
     * putting a band on a panel takes as long as the panel takes.
     */
    const uint32_t faults = rdp_draw_faults();
    const uint32_t did = (dev != NULL) ? serve_poured_class(dev, pd) : 0u;
    if (dev == NULL) {
        s_scr_nodev++;
    } else if (did == 0u) {
        s_scr_nopx++;
        s_scr_last_op = pd->op;
        s_scr_last_len = pd->len;
    }
    if (did > 0u) {
        s_pour_periods++;
        s_pour_bytes += did;
    } else {
        pour_lost(RD_LOST_NOTHING);
    }
    if (did == 0u || rdp_draw_faults() != faults) {
        screen_lost();
    }
    s_scr.r = (s_scr.r + 1u) % s_scr.n;
}

/*
 * As much of the head sound period as the converter takes now.  True when the
 * period is finished with - written, given up on, or had nowhere to go - and
 * the next can start; false when the converter is full and it is to be tried
 * again after whatever picture has arrived meanwhile.
 */
/* the sound task's writes: the longest one, and the longest between two that
 * took something (ag_noderdev_snd_writes) */
static int64_t s_snd_write_max, s_snd_wgap_max, s_snd_wrote_at;

void ag_noderdev_snd_writes(uint32_t *write_max_ms, uint32_t *gap_max_ms)
{
    *write_max_ms = (uint32_t)(s_snd_write_max / 1000);
    *gap_max_ms = (uint32_t)(s_snd_wgap_max / 1000);
}

static bool pour_sound_some(void)
{
    rd_period_t *pd = &s_snd.e[s_snd.r];
    ag_device_t *dev = (pd->handle < (uint32_t)RD_MAX_SESSIONS)
                           ? s_session[pd->handle]
                           : NULL;
    if (dev == NULL) {
        /*
         * Nothing to write it to.  Said once, by name of the handle: a period
         * with no session is not a wire fault and not a converter fault, it
         * is an open that did not happen or did not survive, and the three
         * are indistinguishable from a count of losses alone.
         */
        if (!s_no_sink_said) {
            ag_log(AG_LOG_WARN, TAG,
                   "poured sound has nowhere to go: no open device for "
                   "handle %u", (unsigned)pd->handle);
            s_no_sink_said = true;
        }
        pour_lost(RD_LOST_NODEV);
        return true;
    }
    if (!s_snd_begun) {
        s_snd_len = keep_pace(pd);
        s_snd_done = 0;
        s_snd_waited = 0;
        s_snd_begun = true;
    }
    /*
     * All of the period, as room is made.  A converter's write does not wait
     * (audio_hw.c says why: its caller may hold the registry) and takes what
     * fits - so a period that arrived in a burst of frames found the
     * converter full and was counted lost whole: a third of the C3's sound on
     * the CYD, with the wire perfect.  After 40 ms without room the rest is
     * given up on rather than delaying every period behind it.
     */
    for (;;) {
        if (s_snd_done >= s_snd_len) {
            break;
        }
        const int64_t w0 = ag_port_us();
        const int32_t n = ag_dev_write(dev, pd->data + s_snd_done,
                                       s_snd_len - s_snd_done,
                                       pd->off + s_snd_done);
        const int64_t w1 = ag_port_us();
        if (w1 - w0 > s_snd_write_max) {
            s_snd_write_max = w1 - w0;
        }
        if (n > 0) {
            if (s_snd_wrote_at != 0 && w1 - s_snd_wrote_at > s_snd_wgap_max) {
                s_snd_wgap_max = w1 - s_snd_wrote_at;
            }
            s_snd_wrote_at = w1;
        }
        if (n < 0) {
            break;
        }
        if (n == 0) {
            if (++s_snd_waited >= 40u) {
                break;
            }
            return false; /* full: the picture meanwhile */
        }
        s_snd_done += (uint32_t)n;
    }
    if (s_snd_done > 0) {
        s_pour_periods++;
        s_pour_bytes += s_snd_done;
    }
    if (s_snd_done < s_snd_len) {
        pour_lost(RD_LOST_SHORT);
        s_pour_short++;
    }
    return true;
}

/*
 * THE CONVERTER'S OWN TASK (27 Sep 2026).  One task drew the picture and fed
 * the converter in turn, and while it drew a rectangle nobody fed it: the
 * converter ran dry between periods, which no counter here saw and Maxim
 * heard as clicks far more often than the four a minute counted.  Now sound
 * has this, above the drawing, and does nothing else.
 */
static ag_port_task_t s_snd_task;
static uint32_t       s_snd_late; /* periods that came > RD_SND_LATE_US late */
#define RD_SND_LATE_US 30000

static void sound_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_snd.r == s_snd.w) {
            (void)ag_port_notify_take(true, ag_port_ms_to_ticks(20));
            continue;
        }
        if (pour_sound_some()) {
            s_snd_begun = false;
            s_pour_last_ms = (uint32_t)(ag_port_us() / 1000);
            s_pour_said = false;
            s_snd.r = (s_snd.r + 1u) % s_snd.n;
            continue;
        }
        /* the converter is full: it plays a millisecond, and lets its lock
         * go meanwhile for anybody else who wants the device */
        ag_port_task_delay(1);
    }
}

static void pour_task(void *arg)
{
    (void)arg;
    for (;;) {
        /*
         * A message of the picture, then the sound, in turn.  All of the
         * picture first was up to fifteen milliseconds of drawing with six
         * messages waiting, and the sound that arrived meanwhile - three
         * periods on top of the sender's lead - overflowed its queue a
         * few times a minute (27 Sep 2026).
         */
        bool busy = false;
        if (s_scr.r != s_scr.w) {
            pour_screen_one();
            busy = true;
            s_idle_since = 0u;
        }
        bool full = false;
        if (s_snd_task == NULL && s_snd.r != s_snd.w) {
            if (pour_sound_some()) {
                s_snd_begun = false;
                s_pour_last_ms = (uint32_t)(ag_port_us() / 1000);
                s_pour_said = false;
                s_snd.r = (s_snd.r + 1u) % s_snd.n;
                /*
                 * A TICK BETWEEN PERIODS, for everybody else who wants the
                 * device.  Writing without a pause, this task let the
                 * device's lock go and took it again before a waiter below it
                 * could run - and the waiter was the worker opening the same
                 * converter for the node that asked: it waited out the
                 * asker's patience every time (lockwatch: "held 30 s by
                 * 'rdevpour'").  Woken early by a rectangle arriving; and
                 * none at all while one is waiting to be drawn - drawing it
                 * lets the converter's lock go just the same.
                 */
                if (s_scr.r == s_scr.w) {
                    (void)ag_port_notify_take(true, 1);
                }
                continue;
            }
            full = true;
        }
        if (busy) {
            continue;
        }
        /*
         * Pixels gathered and nothing more arriving: on the glass with them,
         * once RD_IDLE_DRAW_MS has passed with nothing to go on with.  A
         * frame's end draws them anyway, by its marker; this is for a frame
         * that never ends.  After 2 ms it drew the tail of every pass - the
         * next one is a wire's time away - 165 times a second, and noise went
         * to the glass in windows a pass long instead of six rows (30 Sep).
         */
        if (rdp_draw_holds()) {
            const uint32_t now = (uint32_t)(ag_port_us() / 1000);
            if (s_idle_since == 0u) {
                s_idle_since = now | 1u;
            }
            if (now - s_idle_since >= RD_IDLE_DRAW_MS) {
                rdp_draw_flush(RDP_FL_IDLE);
                s_idle_since = 0u;
                continue;
            }
            ag_port_notify_take(true, full ? 1 : 2);
            continue;
        }
        if (full) {
            /* the converter plays a millisecond; a rectangle wakes us sooner */
            (void)ag_port_notify_take(true, 1);
            continue;
        }
        /* Woken by the next period, and on the clock anyway so the end of a
         * burst gets its line without anything having to notice. */
        ag_port_notify_take(true, ag_port_ms_to_ticks(100));
        pour_report();
        glass_back_if_quiet();
        screen_report();
    }
}

/*
 * A period off the wire.  Runs on the link's reader task, so it copies and
 * returns - the converter is written to on the task above.
 */
/*
 * A poured CLASS call, carried out on the pour task.  Returns the bytes it
 * accounted for, or 0 when the device cannot do it - which is what the lost
 * counter is for.
 */
/*
 * THE GLASS, LENT AND GIVEN BACK.  A neighbour's picture is drawn straight
 * onto this board's panel, and this board's console did not know: it went on
 * painting the rows that changed over the picture, and when the neighbour's
 * program ended the glass kept whatever was last on it - Maxim found the CYD
 * showing a white screen with nothing wrong with it (26 Sep 2026).  So while
 * pictures arrive the console keeps off the panel, and it has the glass back
 * and repaints all of it when the neighbour's program lets the screen go
 * (a marker with RD_PASS_DONE), when the neighbour starts over, or after
 * RD_GLASS_QUIET_US with neither a picture nor the link.
 *
 * NOT WHEN THE PICTURE STANDS STILL (30 Sep 2026).  It went back after
 * RD_GLASS_QUIET_US without a picture, and Fallout's main menu does not
 * move: a second and a half after it was drawn the CYD showed its own
 * console, and the game - drawing nothing, because nothing had changed -
 * never learned it had to draw again.  Maxim: the game is not running.
 */
#define RD_GLASS_QUIET_US 1500000
static bool    s_glass_lent;
static int64_t s_glass_last_us;
/*
 * THE CONSOLE DREW OVER THE NEIGHBOUR'S PICTURE, and the neighbour has to be
 * told.  Its painter sends only what changed against what it believes this
 * screen shows; after the glass went back to the console for a pause longer
 * than RD_GLASS_QUIET_US it believed wrong, and every pass after drew a
 * moving square's new edges on a screen of text: white, with pieces of noise
 * and a square the wrong colour (26 Sep 2026, Maxim watching).  The next
 * "pass painted" answer carries RD_PAINTED_LOST, and the painter sends every
 * row again.
 */
#define RD_PAINTED_LOST 1
/*
 * OR THAT IT HAS NO SUCH SESSION AT ALL (29 Sep 2026): it restarted, and
 * every handle it gave out went with it.  The CYD is not the Zero's
 * neighbour - the C3 is - so the Zero is not told by a hello, and it painted
 * into a handle that was no longer there: 82 935 pieces with no device, 88 s
 * of a dead screen, until the Zero itself was restarted.  The answer says
 * which handle, in its last word, and the painter opens the screen again
 * and sends all of it.
 */
#define RD_PAINTED_GONE 2
/*
 * And in the answer's next word, what this screen can draw beyond rows: a
 * rectangle sent as one run (rdpaint.c, 29 Sep 2026).  A painter that is
 * never told so sends rows, which every screen draws; a screen that is older
 * leaves the word 0.
 */
#define RD_CAN_RECTS 1u
#define RD_CAN_IDX   2u
/* undoes copies in a rectangle of indices (rdc_pack8z, 3 Oct 2026) */
#define RD_CAN_Z     4u
/* bits 16..31: the most pixels one rectangle may carry, in 256s (2 Oct
 * 2026); 0 from an image before that, which holds the smaller size */
#define RD_CAN_RECT_SHIFT 16u
static volatile bool s_glass_owed;
/*
 * AND ANY PIECE OF THE PICTURE NOT DRAWN owes the same: a message dropped
 * from a full queue, one the panel refused, one that could not be read to
 * its end.  The painter believes it arrived, so without this the part of the
 * screen it covered stays wrong until those pixels happen to change - and a
 * still background never does.  Counted (s_scr_lost), so the screen report
 * says so.
 */
static uint32_t s_scr_lost;
static uint32_t s_scr_waits; /* the reader waited for room in the picture */

static void screen_lost(void)
{
    s_scr_lost++;
    s_glass_owed = true;
}

/*
 * What this screen does with a neighbour's picture, a second at a time: the
 * passes it was told of (the frames per second a person sees), the messages
 * and pixels drawn, the time spent drawing them, and how full the ring got.
 * The painter's own report says what was SENT; this is what arrived.
 */
static uint32_t s_scr_passes, s_scr_msgs, s_scr_px, s_scr_ring_max;
/* this second: the longest wait between two passes, and between two periods
 * of sound, and the fullest the sound queue got */
static int64_t  s_scr_pass_at, s_scr_gap_max, s_snd_at, s_snd_gap_max;
static uint32_t s_snd_q_max;
/* the same since the board started, for `link` */
static int64_t  s_all_gap_max, s_all_snd_gap_max;
static uint32_t s_all_passes, s_all_snd_q_max;
/*
 * THE SECOND-BY-SECOND REPORT IS ASKED FOR, NOT GIVEN (27 Sep 2026).  A
 * journal line waits for the console, and the CYD's console is a UART at
 * 115 200: while it printed, the task that feeds the converter waited with
 * it, and the sound queue overflowed once in every sixteen seconds - exactly
 * as often as the measuring script asked for the journal.  `link report on`
 * turns it on; `link` shows the totals without it.
 */
static volatile bool s_report;

void ag_noderdev_screen_done(void) { rdp_screen_done(); }

void ag_noderdev_screen_taken(void) { rdp_screen_taken(); }

void ag_noderdev_stable_surface(const uint16_t *px, uint16_t w, uint16_t h)
{
    rdp_stable_surface(px, w, h);
}

void ag_noderdev_report(bool on)
{
    s_report = on;
}

void ag_noderdev_screen_counts(uint32_t *passes, uint32_t *lost,
                               uint32_t *gap_ms, uint32_t *snd_gap_ms,
                               uint32_t *snd_q_max, uint32_t *snd_q_cap)
{
    *passes = s_all_passes;
    *lost = s_scr_lost;
    *gap_ms = (uint32_t)(s_all_gap_max / 1000);
    *snd_gap_ms = (uint32_t)(s_all_snd_gap_max / 1000);
    *snd_q_max = s_all_snd_q_max;
    *snd_q_cap = s_snd.n - 1u;
}

void ag_noderdev_draw_misses(uint32_t *nodev, uint32_t *nopx, uint32_t *op,
                             uint32_t *len, uint32_t *faults)
{
    *nodev = s_scr_nodev;
    *nopx = s_scr_nopx;
    *op = s_scr_last_op;
    *len = s_scr_last_len;
    *faults = rdp_draw_faults();
}

void ag_noderdev_wait_counts(uint32_t *reader_waits, uint32_t *late)
{
    *reader_waits = s_scr_waits;
    *late = s_snd_late;
}
static int64_t  s_scr_us, s_scr_said_us;

/*
 * THE ANSWER GOES OUT FROM A TASK OF ITS OWN.  Sent from the pour task, it
 * waited up to 20 ms for room on the wire back, and the pour task is the one
 * that feeds the converter: with every pass answered that way the CYD threw
 * away 285 periods of sound in a minute, where it had thrown away 30.  The
 * pour task only posts it here and goes on.
 */
#define RD_ACKS 4u

typedef struct {
    int32_t  wire;
    uint16_t from;
    uint16_t tag;
    uint8_t  status;  /* 0, RD_PAINTED_LOST or RD_PAINTED_GONE */
    uint32_t handle;  /* the pass's, for a GONE */
} rd_ack_t;

static rd_ack_t          s_ack[RD_ACKS];
static volatile uint32_t s_ack_w, s_ack_r;
static ag_port_task_t    s_acker;

/* "That pass is drawn", with what this screen can draw. */
static void paint_answer(int32_t wire, uint16_t from, uint16_t tag,
                         uint8_t status, uint32_t handle)
{
    const ag_err_t e = send_raw(
        wire, from, (uint8_t)(RD_PAINTED | RD_REPLY), 0, tag, status,
        (rdp_draws_rects() ? RD_CAN_RECTS : 0u) |
            (rdp_draws_idx() ? (RD_CAN_IDX | RD_CAN_Z) : 0u) |
            ((rdp_draw_rect_px() / 256u) << RD_CAN_RECT_SHIFT),
        handle, AG_LINK_CONTROL, NULL,
        0);
    if (e != AG_OK) {
        ag_log(AG_LOG_WARN, TAG, "a pass's answer did not go out: %d", (int)e);
    }
}

static void acker_task(void *arg)
{
    (void)arg;
    for (;;) {
        while (s_ack_r != s_ack_w) {
            const rd_ack_t a = s_ack[s_ack_r % RD_ACKS];
            s_ack_r++;
            paint_answer(a.wire, a.from, a.tag, a.status, a.handle);
        }
        ag_port_notify_take(true, ag_port_ms_to_ticks(100));
    }
}

/* The task the painter's answers go out from: at start-up on a board with a
 * screen (2 Oct 2026), for the same reason as pour_tasks_make. */
static bool acker_make(void)
{
    return s_acker != NULL ||
           ag_proc_task_create(acker_task, "rdevack", 2048, NULL, 6,
                               AG_PORT_ANY_CORE, &s_acker);
}

/* frames drawn whole here, and the waits between them (ag_noderdev_frames_seen) */
#define RD_SEEN_SLOW_US  250000
#define RD_SEEN_STALL_US 1000000
static uint32_t s_seen_frames, s_seen_slow, s_seen_stalls;
/* the frame that ended the longest wait: an early one is a program starting */
static uint32_t s_seen_gap_nr;
static int64_t  s_seen_at, s_seen_slow_us, s_seen_gap_max;

void ag_noderdev_frames_seen(uint32_t *frames, uint32_t *slow, uint32_t *stalls,
                             uint32_t *slow_ms, uint32_t *gap_max_ms,
                             uint32_t *gap_max_nr)
{
    *gap_max_nr = s_seen_gap_nr;
    *frames = s_seen_frames;
    *slow = s_seen_slow;
    *stalls = s_seen_stalls;
    *slow_ms = (uint32_t)(s_seen_slow_us / 1000);
    *gap_max_ms = (uint32_t)(s_seen_gap_max / 1000);
}

void ag_noderdev_paint_waits(uint32_t *timeouts, uint32_t *wait_max_ms,
                             uint32_t *unsent)
{
    *timeouts = s_paint_timeouts;
    *wait_max_ms = (uint32_t)(s_paint_wait_max / 1000);
    *unsent = s_paint_unsent;
}

/* A pass's marker, reached: every message of the pass is on the glass. */
static void pour_pass_drawn(const rd_period_t *pd)
{
    s_scr_passes++;
    const int64_t now = ag_port_us();
    if ((pd->off & RD_PASS_WHOLE) != 0u) {
        s_seen_frames++;
        const int64_t gap = (s_seen_at != 0) ? now - s_seen_at : 0;
        if (gap >= RD_SEEN_SLOW_US) {
            s_seen_slow++;
            s_seen_slow_us += gap;
        }
        if (gap >= RD_SEEN_STALL_US) {
            s_seen_stalls++;
        }
        if (gap > s_seen_gap_max) {
            s_seen_gap_max = gap;
            s_seen_gap_nr = s_seen_frames;
        }
        s_seen_at = now;
    }
    s_all_passes++;
    if (s_scr_pass_at != 0 && now - s_scr_pass_at > s_all_gap_max) {
        s_all_gap_max = now - s_scr_pass_at;
    }
    if (s_scr_pass_at != 0 && now - s_scr_pass_at > s_scr_gap_max) {
        s_scr_gap_max = now - s_scr_pass_at;
    }
    s_scr_pass_at = now;
    const bool lost = s_glass_owed;
    s_glass_owed = false;
    const bool gone = pd->handle >= (uint32_t)RD_MAX_SESSIONS ||
                      s_session[pd->handle] == NULL;
    const uint8_t status = gone ? RD_PAINTED_GONE
                                : (lost ? RD_PAINTED_LOST : 0u);
    if (s_acker == NULL && !acker_make()) {
        /* no task: answered here, as it was, rather than not at all */
        paint_answer(pd->wire, pd->from, pd->tag, status, pd->handle);
        return;
    }
    if (s_ack_w - s_ack_r >= RD_ACKS) {
        /* four behind: the painter's wait will cover this one - but not
         * its "lost", which the next answer carries instead */
        s_glass_owed = s_glass_owed || lost;
        return;
    }
    s_ack[s_ack_w % RD_ACKS] =
        (rd_ack_t){pd->wire, pd->from, pd->tag, status, pd->handle};
    s_ack_w++;
    ag_port_notify_give(s_acker);
}

static void screen_report(void)
{
    const int64_t now = ag_port_us();
    if (s_scr_said_us == 0) {
        s_scr_said_us = now;
        return;
    }
    if (now - s_scr_said_us < 1000000) {
        return;
    }
    const bool screen = s_report &&
                        (s_scr_msgs != 0u || s_scr_passes != 0u ||
                         s_scr_raw != 0u || s_scr_text != 0u ||
                         s_scr_asked != 0u);
    if (screen) {
        const uint32_t ms = (uint32_t)((now - s_scr_said_us) / 1000);
        uint32_t       un_us, bl_us, blits, why[RDP_FL_N];
        rdp_draw_times(&un_us, &bl_us, &blits, why);
        ag_log(AG_LOG_INFO, TAG,
               "drawing: %u ms unpacking, %u ms in %u blits; strips drawn "
               "%u full, %u under a new piece, %u at a frame's end, %u idle, "
               "%u other",
               (unsigned)(un_us / 1000u), (unsigned)(bl_us / 1000u),
               (unsigned)blits, (unsigned)why[RDP_FL_EVICT],
               (unsigned)why[RDP_FL_UNDER], (unsigned)why[RDP_FL_FRAME],
               (unsigned)why[RDP_FL_IDLE], (unsigned)why[RDP_FL_OTHER]);
        ag_log(AG_LOG_INFO, TAG,
               "screen: %u passes in %u ms, %u messages, %u px, %u ms of it "
               "drawing, queue up to %u of %u, %u not drawn so far",
               (unsigned)s_scr_passes, (unsigned)ms, (unsigned)s_scr_msgs,
               (unsigned)s_scr_px, (unsigned)(s_scr_us / 1000),
               (unsigned)s_scr_ring_max, (unsigned)(s_scr.n - 1u),
               (unsigned)s_scr_lost);
    }
    /* apart, because a journal line has a length and the first one had
     * grown past it; and for sound alone too */
    if (s_report && (screen || s_snd_gap_max != 0 || s_snd_q_max != 0u)) {
        ag_log(AG_LOG_INFO, TAG,
               "timing: passes up to %u ms apart, sound up to %u ms apart, "
               "its queue up to %u of %u; besides the picture %u raw, %u "
               "text, %u asked",
               (unsigned)(s_scr_gap_max / 1000), (unsigned)(s_snd_gap_max / 1000),
               (unsigned)s_snd_q_max, (unsigned)(s_snd.n - 1u),
               (unsigned)s_scr_raw, (unsigned)s_scr_text,
               (unsigned)s_scr_asked);
    }
    s_scr_said_us = now;
    s_scr_passes = s_scr_msgs = s_scr_px = s_scr_ring_max = 0u;
    s_scr_raw = s_scr_text = s_scr_asked = 0u;
    s_scr_gap_max = s_snd_gap_max = 0;
    s_snd_q_max = 0u;
    s_scr_us = 0;
}

static void glass_lend(void)
{
    s_glass_last_us = ag_port_us();
    if (!s_glass_lent) {
        s_glass_lent = true;
        /*
         * UNDER THE LOCK THE CONSOLE PAINTS UNDER (27 Sep 2026).  The panel's
         * driver has no lock of its own, and the console's render reads "am I
         * enabled" once and then paints its rows: switched off from here in
         * the middle of that, the glass had two painters at once - this task
         * and the console's - in one SPI device, and the console's task waited
         * for ever for a transfer the other had taken (a CYD whose console
         * died, drawing on).  Taking the registry lock waits a render out.
         */
        ag_dev_lock_hold();
        ag_textpanel_enable(false);
        ag_dev_lock_release();
        ag_power_kernel_full(true); /* drawn at the full clock, not in cruise */
        ag_log(AG_LOG_INFO, TAG, "a neighbour is drawing on this screen");
    }
}

static void glass_back(const char *why)
{
    if (!s_glass_lent) {
        return;
    }
    s_glass_lent = false;
    ag_power_kernel_full(false);
    ag_textpanel_enable(true);
    s_glass_owed = true;
    ag_log(AG_LOG_INFO, TAG, "%s; the screen is this board's again", why);
}

/* the neighbour started over: what it drew is nobody's (pour task's to act on) */
static volatile bool s_glass_orphaned;

static void glass_back_if_quiet(void)
{
    if (s_glass_orphaned) {
        s_glass_orphaned = false;
        glass_back("the neighbour started over");
        return;
    }
    if (s_glass_lent && ag_port_us() - s_glass_last_us > RD_GLASS_QUIET_US &&
        !ag_linkprov_up()) {
        glass_back("the neighbour is not heard");
    }
}

static uint32_t serve_poured_class(ag_device_t *dev, const rd_period_t *pd)
{
    const ag_display_ops_t *d = (const ag_display_ops_t *)dev->class_ops;
    const uint8_t          *pay = pd->data;
    const uint16_t          plen = pd->len;

    switch (pd->op) {
    case RD_BLIT: {
        s_scr_raw++;
        if (d == NULL || d->blit_rect == NULL || plen < RD_BLIT_HDR) {
            return 0;
        }
        const ag_blit_t bl = {
            .px = pay + RD_BLIT_HDR,
            .stride = (uint32_t)get16(pay + 4) * 2u,
            .surf_w = get16(pay + 8),
            .surf_h = get16(pay + 10),
            .x = get16(pay + 0),
            .y = get16(pay + 2),
            .w = get16(pay + 4),
            .h = get16(pay + 6),
        };
        const uint32_t want = (uint32_t)bl.w * bl.h * 2u;
        if (want == 0u || want > (uint32_t)(plen - RD_BLIT_HDR)) {
            return 0;
        }
        glass_lend();
        d->blit_rect(0, &bl);
        return want;
    }
    case RD_BLITZ: {
        glass_lend();
        wt_note(WT_DRAW_IN, plen);
        const int64_t  t0 = ag_port_us();
        const uint32_t px = rdp_draw(d, pay, plen);
        wt_note(WT_DRAW_OUT, px);
        s_scr_us += ag_port_us() - t0;
        s_scr_msgs++;
        s_scr_px += px;
        return px;
    }
    case RD_TEXTROW: {
        s_scr_text++;
        if (d == NULL || d->text_row == NULL || plen < 4u) {
            return 0;
        }
        const uint16_t row = get16(pay);
        uint16_t       cnt = get16(pay + 2);
        const uint16_t room = (uint16_t)((plen - 4u) / sizeof(ag_textcell_t));
        if (cnt > room) {
            cnt = room;
        }
        if (cnt == 0u) {
            return 0;
        }
        d->text_row(0, row, (const ag_textcell_t *)(const void *)(pay + 4),
                    cnt);
        return (uint32_t)cnt * sizeof(ag_textcell_t);
    }
    case RD_TEXTCUR: {
        s_scr_text++;
        if (d == NULL || d->text_cursor == NULL || plen < 5u) {
            return 0;
        }
        ag_textcell_t under = {.ch = ' ', .attr = 0};
        if (plen >= 7u) {
            under.ch = pay[5];
            under.attr = pay[6];
        }
        d->text_cursor(0, get16(pay), get16(pay + 2), under, pay[4] != 0u);
        return 1u;
    }
    default:
        return 0;
    }
}

/*
 * The places sound and picture are poured into, made once.  At start-up on a
 * board with a converter of its own (ag_noderdev_serve, 2 Oct 2026): made on
 * the first period instead, the CYD found its 27 KB taken by the neighbour's
 * picture by then, and every period of sound was lost for want of it ("no
 * memory") while the picture went on.  Still made late where it was not made
 * at the start.
 */
static bool ring_make(void)
{
    if (s_ring_mem != NULL) {
        return true;
    }
    /*
     * Twenty kilobytes, out of PSRAM where there is any.  Taken from
     * internal memory on the Zero it was the last of it, and the
     * converter these periods are for could then not have the 4 KB its
     * DMA descriptors need: sound arrived and nothing played (26 Sep
     * 2026).  A period is copied out of here once; PSRAM is fast enough.
     */
    static const uint8_t sizes[2][2] = {{RD_SND_BIG, RD_SCR_BIG},
                                        {RD_SND, RD_SCR}};
    for (unsigned k = 0; k < 2u && s_ring == NULL; k++) {
        const uint32_t nsnd = sizes[k][0], nscr = sizes[k][1];
        const size_t   bytes =
            (size_t)nsnd * RD_SND_CAP + (size_t)nscr * RD_SCR_CAP;
        /* the larger set only in PSRAM: in internal memory it would be the
         * CYD's picture and its converter that went without */
        s_ring_mem = (uint8_t *)ag_port_alloc(bytes,
                                              AG_MEM_SLOW | AG_MEM_BYTE);
        if (s_ring_mem == NULL && k == 1u) {
            s_ring_mem = (uint8_t *)ag_port_alloc(bytes, AG_MEM_BYTE);
        }
        if (s_ring_mem == NULL) {
            continue;
        }
        /* and its places beside it: 46 of them are 1.3 KB, and on an
         * emulated S3 that was the internal memory the next request's
         * worker could not then be made in - the far end answered -12 and
         * the mixer had nowhere to send sound (nodetest, 29 Sep 2026) */
        const size_t places = (size_t)(nsnd + nscr) * sizeof(rd_period_t);
        s_ring = (rd_period_t *)ag_port_alloc(places,
                                              AG_MEM_SLOW | AG_MEM_BYTE);
        if (s_ring == NULL && k == 1u) {
            s_ring = (rd_period_t *)ag_port_alloc(places, AG_MEM_BYTE);
        }
        if (s_ring == NULL) {
            ag_port_free(s_ring_mem);
            s_ring_mem = NULL;
            continue;
        }
        memset(s_ring, 0, places);
        s_snd.n = nsnd;
        s_scr.n = nscr;
        s_snd.e = s_ring;
        s_scr.e = s_ring + nsnd;
        for (uint32_t i = 0; i < nsnd + nscr; i++) {
            s_ring[i].data =
                (i < nsnd) ? s_ring_mem + (size_t)i * RD_SND_CAP
                           : s_ring_mem + (size_t)nsnd * RD_SND_CAP +
                                 (size_t)(i - nsnd) * RD_SCR_CAP;
        }
    }
    if (s_ring == NULL) {
        return false;
    }
    return true;
}

static void pour_task(void *arg);
static void sound_task(void *arg);

/* The tasks that draw and play what is poured here (2 Oct 2026: at start-up
 * on a board with a converter, as ring_make - made on the first period, the
 * CYD had no 3 KB left for the converter's one once a picture was drawing). */
static bool pour_tasks_make(void)
{
    if (s_pour_task == NULL &&
        !ag_proc_task_create(pour_task, "rdevpour", 4096, NULL, 1,
                             AG_PORT_ANY_CORE, &s_pour_task)) {
        return false;
    }
    /* the converter's task, above the drawing: without it the pour task
     * feeds the converter as before */
    if (s_snd_task == NULL) {
        (void)ag_proc_task_create(sound_task, "rdevsnd", 3072, NULL, 7,
                                  AG_PORT_ANY_CORE, &s_snd_task);
    }
    return true;
}

static void take_period(const uint8_t *b, uint16_t n)
{
    if (s_pour_first_us == 0) {
        s_pour_first_us = ag_port_us();
    }
    if (!ring_make()) {
        pour_lost(RD_LOST_NOMEM);
        return;
    }
    if (s_pour_task == NULL) {
        /*
         * Made on the first period rather than at start-up: a board that never
         * has sound poured at it should not carry a task and its stack for the
         * possibility.  Below applications, like every other task this file's
         * neighbours make.
         */
        /* 4 KB: at 3 it had 260 bytes left on the Zero (`mem /t`). */
        /* internal memory, else PSRAM, as the server's workers: on an
         * emulated S3 there were not 4 KB inside, the task was never made,
         * and every period poured at it was lost (nodetest, 27 Sep 2026) */
        if (!pour_tasks_make()) {
            pour_lost(RD_LOST_NOMEM);
            return;
        }
        ag_log(AG_LOG_INFO, TAG, "sound is being poured at us from %s",
               ag_node_peer_name() != NULL ? ag_node_peer_name() : "the wire");
    }

    rd_q_t *const q = (b[0] == RD_WRITE) ? &s_snd : &s_scr;
    if (q == &s_snd) {
        const int64_t now = ag_port_us();
        if (s_snd_at != 0 && now - s_snd_at > RD_SND_LATE_US) {
            s_snd_late++;
        }
        if (s_snd_at != 0 && now - s_snd_at > s_all_snd_gap_max) {
            s_all_snd_gap_max = now - s_snd_at;
        }
        if (q_used(q) > s_all_snd_q_max) {
            s_all_snd_q_max = q_used(q);
        }
        if (s_snd_at != 0 && now - s_snd_at > s_snd_gap_max) {
            s_snd_gap_max = now - s_snd_at;
        }
        s_snd_at = now;
        if (q_used(q) > s_snd_q_max) {
            s_snd_q_max = q_used(q);
        }
        wt_note(WT_SND_Q, q_used(q));
    }
    uint32_t      next = (q->w + 1u) % q->n;
    /*
     * SOUND DOES NOT WAIT HERE, and it used to - up to 20 ms for room, on the
     * reader, which is the one task that reads the wire.  The sender keeps
     * 30 ms of sound ahead and the queue holds about 36, so it was nearly
     * always full: the reader spent its time asleep in here, and everything
     * else on the wire waited with it - the painter's "pass painted" answers
     * most of all, which the painter waits for before its next pass.  With
     * sound going, a square drawn 63 times a second alone was drawn 3 times
     * (26 Sep 2026).  A period that finds the queue full is dropped and
     * counted, which is what the stream class is for.
     */
    if (next == q->r && (b[0] == RD_BLITZ || b[0] == RD_PAINTED)) {
        /*
         * A screen kept by what changed cannot miss a change: this waits for
         * the pour task, and the wire waits behind it - which is the screen
         * being slower than the wire, said as back-pressure.
         */
        s_scr_waits++;
        wt_note(WT_SCR_FULL, q_used(q));
        int k = 0;
        for (; k < 1000 && next == q->r; k++) {
            ag_port_task_delay(1);
            next = (q->w + 1u) % q->n;
        }
        wt_note(WT_SCR_ROOM, (uint32_t)k);
    }
    if (next == q->r) {
        pour_lost(RD_LOST_FULL); /* full: this one missed its moment */
        s_pour_full++;
        if (q == &s_scr) {
            screen_lost();
        }
        return;
    }
    rd_period_t *pd = &q->e[q->w];
    uint16_t     plen = (uint16_t)(n - RD_HDR);
    const uint16_t cap = (q == &s_snd) ? (uint16_t)RD_SND_CAP
                                       : (uint16_t)RD_SCR_CAP;
    if (plen > cap) {
        plen = cap; /* an older sender's longer piece: its tail is gone */
        s_pour_short++;
    }
    memcpy(pd->data, b + RD_HDR, plen);
    pd->len = plen;
    pd->handle = get32(b + 4);
    pd->off = get32(b + 12);
    pd->op = b[0];
    pd->wire = ag_linkprov_current();
    pd->from = ag_linkprov_from();
    pd->tag = get16(b + 2);
    q->w = next;
    if (q == &s_scr && q_used(q) > s_scr_ring_max) {
        s_scr_ring_max = q_used(q);
    }
    if (q == &s_snd && s_snd_task != NULL) {
        ag_port_notify_give(s_snd_task);
    } else {
        ag_port_notify_give(s_pour_task);
    }
}

static void on_msg(uint8_t chan, const void *data, uint16_t n, void *ctx)
{
    (void)chan;
    (void)ctx;
    if (n < RD_HDR) {
        return;
    }
    const uint8_t *b = (const uint8_t *)data;

    /*
     * The painter's "that pass is over", and its answer - neither goes near
     * the request machinery: no worker, no single outstanding question.
     * Answered here, on the reader, because arriving here is the whole of
     * what it asks: everything poured before it has arrived too (same class,
     * in order), and the RD_BLITZ ahead of it were taken into the ring.
     */
    if (b[0] == RD_PAINTED) {
        /*
         * INTO THE QUEUE BEHIND THE PASS, AND ANSWERED WHEN IT IS DRAWN
         * (26 Sep 2026).  Answered here, on arrival, it told the painter a
         * pass was over while it was still waiting to be drawn, so the
         * painter went on at the pace of the wire and not of this screen:
         * the CYD draws about 157 000 pixels a second, and 62 passes of a
         * moving square are 155 000 - the picture queue stayed full, this
         * reader waited for room in it, and the sound behind the picture on
         * the same wire was turned away until the CYD fell silent.  Answered
         * from the queue, the painter runs at the speed the glass is drawn.
         */
#if RD_ACK_WHEN_DRAWN
        take_period(b, n);
#else
        {
            rd_period_t now_pd;
            memset(&now_pd, 0, sizeof(now_pd));
            now_pd.wire = ag_linkprov_current();
            now_pd.from = ag_linkprov_from();
            now_pd.tag = get16(b + 2);
            pour_pass_drawn(&now_pd); /* on arrival, as before */
        }
#endif
        return;
    }
    if (b[0] == (uint8_t)(RD_PAINTED | RD_REPLY)) {
        s_paint_acked = get16(b + 2);
        s_paint_heard = true; /* this screen answers: wait for it from now */
        rdp_far_rects((get32(b + 8) & RD_CAN_RECTS) != 0u);
        rdp_far_rect_px((get32(b + 8) >> RD_CAN_RECT_SHIFT) * 256u);
        rdp_far_idx((get32(b + 8) & RD_CAN_IDX) != 0u);
        rdp_far_z((get32(b + 8) & RD_CAN_Z) != 0u);
        rdev_t *const dp = s_display_proxy;
        if ((int32_t)get32(b + 4) == RD_PAINTED_GONE && dp != NULL &&
            dp->far_handle >= 0 && (uint32_t)dp->far_handle == get32(b + 12)) {
            /* the screen restarted: opened again on the next present */
            ag_log(AG_LOG_INFO, TAG,
                   "%s has no session %d any more (it restarted?): opening "
                   "it again", dp->qualified, (int)dp->far_handle);
            dp->far_handle = -1;
            rdp_forget();
            return;
        }
        if ((int32_t)get32(b + 4) == RD_PAINTED_LOST) {
            rdp_forget(); /* its console drew over it: all of it again */
        }
        return;
    }

    if ((b[0] & RD_REPLY) != 0) {
        const uint16_t tag = get16(b + 2);
        if (!s_busy || s_have || tag == 0 || tag != s_tag) {
            return;
        }
        s_status = (int32_t)get32(b + 4);
        uint16_t plen = (uint16_t)(n - RD_HDR);
        if (plen > sizeof(s_rbuf)) {
            plen = (uint16_t)sizeof(s_rbuf);
        }
        memcpy(s_rbuf, b + RD_HDR, plen);
        s_rlen = plen;
        s_have = true;
        return;
    }

    /*
     * Poured bytes.  Not a request: nothing is expected back, so there is no
     * tag to match, nothing to refuse with, and nobody to refuse to.  Into the
     * ring and out of this task.
     */
    if ((b[1] & RD_F_NOREPLY) != 0) {
        take_period(b, n);
        return;
    }

    /*
     * A request.  Off this task at once - see the note at the top: a device
     * operation is entitled to block, and blocking here would stop this node
     * hearing anything, including the acks its own traffic waits for.
     */
    const int32_t  wire = ag_linkprov_current();
    const uint16_t from = ag_linkprov_from();
    if (s_working) {
        reply_to(wire, from, b[0], get16(b + 2), -AG_EBUSY, AG_LINK_CONTROL,
                 NULL, 0);
        return;
    }
    if (n > sizeof(s_req)) {
        n = (uint16_t)sizeof(s_req);
    }
    memcpy(s_req, b, n);
    s_req_len = n;
    s_req_wire = wire;
    s_req_from = from;
    s_working = true;
    if (s_standing) {
        (void)ag_port_sem_give(s_req_sem);
        return;
    }
    /* As nodeproc's worker: internal stack, else PSRAM (see there). */
    if (!ag_proc_task_create(worker, "rdevsrv", 4096, NULL, 1,
                             AG_PORT_ANY_CORE, &s_worker)) {
        s_working = false;
        reply(b[0], get16(b + 2), -AG_ENOMEM, AG_LINK_CONTROL, NULL, 0);
    }
}

ag_err_t ag_noderdev_serve(void)
{
    if (s_serving) {
        return AG_OK;
    }
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    const ag_err_t e = ag_linkprov_listen(RD_CHAN, on_msg, NULL);
    if (e != AG_OK) {
        return e;
    }
    s_serving = true;
    /*
     * AT START-UP ONLY WHERE THERE IS NO PSRAM (2 Oct 2026).  Without it what
     * is not taken now is not there later - the CYD - and then the screen
     * first, the sound after: a board that is a neighbour's screen needs its
     * 9 KB in one piece, and once sound has taken its places there is no such
     * piece left.  With it everything here can be had when first wanted, and
     * made at start-up the tasks' stacks took 2.5 KB of the emulated S3's
     * internal memory that a program's threads then did not find (nodetest).
     */
    if (ag_port_mem_free(AG_MEM_SLOW) > 0u) {
        /*
         * ONE REQUEST WORKER HERE TOO, ITS STACK IN PSRAM (6 Oct 2026).  A
         * worker made for each request and deleted after it gives its stack
         * back only when the idle task gets round to it; a busy emulated S3
         * serving its sound while a neighbour drew on it had the next
         * request's 4 KB refused (-12) while the last one's was still
         * waiting to be freed - nodetest's far sound failed twice in three
         * runs.  Standing, in PSRAM, it costs no internal memory.
         */
        worker_make();
        return AG_OK;
    }
    if (ag_display_ready()) {
        (void)rdp_draw_prepare();
        (void)acker_make();
    }
    /* before the sound - and only on a board that is a screen or a speaker
     * to its neighbours: the emulated C3, neither, held 4 KB for good and a
     * program's 32 KB arena then did not fit (xclutest, 3 Oct 2026) */
    if (ag_display_ready() || ag_port_audio_present()) {
        worker_make();
    }
    if (ag_port_audio_present() && ring_make()) {
        (void)pour_tasks_make(); /* now, while there is memory for them */
    }
    return AG_OK;
}

/* ---- attaching ---------------------------------------------------------- */

/*
 * EVERY NEIGHBOUR'S DEVICES, AT ONCE, EACH WHERE IT IS.
 *
 * This was one neighbour: the one `link use` had selected, all its proxies in
 * one owner, and a second `link devs` took the first neighbour's away.  A board
 * in the middle of three has two, and what Maxim asked for is the system he
 * describes - "the boards know nothing about wires; a list of devices simply
 * appears".  So proxies are owned per wire, a wire's neighbour can come and go
 * without touching the others', and each proxy carries its own wire and node.
 *
 * One owner token per wire: ag_dev_revoke_owner takes a neighbour's devices
 * out and leaves everybody else's.
 */
/*
 * AND BEYOND THEM.  A node two hops away - the CYD, seen from the Zero across
 * the C3 - is a source of devices like a neighbour, reached out of the wire it
 * is heard through and addressed by its own name, so the board in between
 * passes every question on and every answer back.  Its sources come after the
 * wires': RD_FAR of them, learned from the cluster's list of far nodes.
 */
#define RD_FAR  4
#define RD_SRCS ((int32_t)AG_LINK_WIRES + RD_FAR)
static uint8_t s_owner_tok[RD_SRCS];
static bool    s_attached_on[RD_SRCS];
static int32_t s_far_wire[RD_FAR];
static char    s_far_name[RD_FAR][AG_NODE_NAME_MAX];
static uint8_t s_far_starts[RD_FAR]; /* as told when last attached */
/*
 * When a far node's devices last failed to attach, and so when to ask again.
 * The cluster's list of far nodes comes every two seconds and each one is a
 * chance; the CYD heard of the Zero while the Zero was still restarting, its
 * three tries timed out, and it never asked again (26 Sep 2026).
 */
#define RD_FAR_RETRY_MS 30000u
static uint32_t s_far_failed_ms[RD_FAR];
static bool     s_far_failed[RD_FAR];

/*
 * The screen, chosen among every neighbour's: A PANEL BEATS A BUFFER IN
 * MEMORY, and that is a fact about screens rather than about registries - the
 * same shape as "a real converter beats the null one", which is why the audio
 * list lives in the mixer.  Taking whichever came first sent a megabyte of
 * pixels to the far board's SOFT framebuffer, where nobody was looking, while
 * its ST7789 sat beside it in the same listing.  Chosen again whenever a
 * neighbour comes or goes, so a screen that left is not drawn on.
 */
static void pick_display(void)
{
    rdev_t *best = NULL;
    for (int i = 0; s_proxy != NULL && i < RD_MAX_PROXIES; i++) {
        rdev_t *p = &s_proxy[i];
        if (!p->used || !p->display) {
            continue;
        }
        if (best == NULL || (p->panel && !best->panel)) {
            best = p;
        }
    }
    if (best != s_display_proxy) {
        if (best != NULL) {
            ag_log(AG_LOG_INFO, TAG, "%s is the neighbour's screen",
                   best->qualified);
        }
        s_display_proxy = best;
        s_display_real = (best != NULL) && best->panel;
    }
}

static uint32_t detach_src(int32_t src)
{
    if (src < 0 || src >= RD_SRCS) {
        return 0;
    }
    const uint32_t gone = ag_dev_revoke_owner((const void *)&s_owner_tok[src]);
    for (int i = 0; s_proxy != NULL && i < RD_MAX_PROXIES; i++) {
        if (s_proxy[i].used && s_proxy[i].src == src) {
            if (s_display_proxy == &s_proxy[i]) {
                s_display_proxy = NULL;
                s_display_real = false;
            }
            memset(&s_proxy[i], 0, sizeof(s_proxy[i]));
        }
    }
    s_attached_on[src] = false;
    pick_display();
    return gone;
}

static int32_t attach_locked(int32_t src, uint32_t timeout_ms);

/*
 * One attach at a time: the shell's `link devs` and the docking task can both
 * be here, and a device listing and the proxy table are single things.
 */
static ag_port_mutex_t s_att_lock;

static int32_t attach_src(int32_t src, uint32_t timeout_ms)
{
    if (s_att_lock == NULL) {
        s_att_lock = ag_port_mutex_new();
        if (s_att_lock == NULL) {
            return -AG_ENOMEM;
        }
    }
    if (!ag_port_mutex_take(s_att_lock, ag_port_ms_to_ticks(20000))) {
        return -AG_EBUSY;
    }
    const int32_t r = attach_locked(src, timeout_ms);
    (void)ag_port_mutex_give(s_att_lock);
    return r;
}

int32_t ag_noderdev_attach_on(int32_t wire, uint32_t timeout_ms)
{
    if (wire < 0 || wire >= (int32_t)AG_LINK_WIRES) {
        return -AG_ENODEV;
    }
    return attach_src(wire, timeout_ms);
}

static int32_t attach_locked(int32_t src, uint32_t timeout_ms)
{
    if (src < 0 || src >= RD_SRCS) {
        return -AG_EINVAL;
    }
    const bool    far = src >= (int32_t)AG_LINK_WIRES;
    const int32_t wire = far ? s_far_wire[src - (int32_t)AG_LINK_WIRES] : src;
    if (wire < 0 || wire >= (int32_t)AG_LINK_WIRES ||
        ag_linkprov_name_at(wire) == NULL) {
        return -AG_ENODEV;
    }
    const char *peer = far ? s_far_name[src - (int32_t)AG_LINK_WIRES]
                           : ag_node_peer_name_at(wire);
    if (peer == NULL || peer[0] == '\0') {
        /*
         * No name, no qualified names - and a proxy called `pcm0` beside this
         * node's own `pcm0` is exactly the ambiguity the naming scheme exists
         * to prevent.  Better to wait for the hello than to register something
         * whose name will be wrong.
         */
        return -AG_EAGAIN;
    }
    const ag_err_t se = ag_noderdev_serve();
    if (se != AG_OK) {
        return (int32_t)se;
    }
    if (proxy_table() == NULL) {
        ag_log(AG_LOG_ERROR, TAG, "no memory for the neighbour's devices");
        return -AG_ENOMEM;
    }

    /*
     * Into a bounded buffer first.  ag_node_peer_name_at returns a pointer and
     * the compiler cannot see that what is behind it is already short, so the
     * qualified name below would be built from something of unknown length.
     * The copy is exact - a name is validated against this same bound when it
     * arrives - and it makes the bound visible where it is used.
     */
    char pn[AG_NODE_NAME_MAX];
    ag_strlcpy(pn, peer, sizeof(pn));

    /* On the heap: 32 records are 1.8 KB, and this runs on a small task. */
    ag_devinfo_t *info =
        (ag_devinfo_t *)ag_port_alloc(sizeof(ag_devinfo_t) * RD_MAX_PROXIES,
                                      AG_MEM_SLOW | AG_MEM_BYTE);
    if (info == NULL) {
        info = (ag_devinfo_t *)ag_port_alloc(
            sizeof(ag_devinfo_t) * RD_MAX_PROXIES, AG_MEM_BYTE);
    }
    if (info == NULL) {
        return -AG_ENOMEM;
    }
    const uint16_t node = ag_node_hash(pn);
    const int32_t  n =
        ag_nodedev_list_to(wire, node, info, RD_MAX_PROXIES, timeout_ms);
    if (n < 0) {
        ag_port_free(info);
        return n;
    }

    /* What was there before goes first: a neighbour that says hello again has
     * started over, and every handle it gave out is gone with it. */
    (void)detach_src(src);

    int32_t made = 0;
    int32_t skipped = 0;
    int     slot = 0;
    for (int32_t i = 0; i < n && i < (int32_t)RD_MAX_PROXIES; i++) {
        /*
         * Not the link itself.  A proxy for the far node's own link device
         * would be a device reached over the very thing it describes, and
         * `link use sound.node0` would be a loop with no bottom.
         */
        if (info[i].cls == AG_DEV_LINK) {
            continue;
        }
        /*
         * Nor the far node's own proxies - its OTHER neighbours' devices.
         * `c3.cyd.pcm0` on the Zero would be the CYD's converter reached as a
         * device of the C3, every sample served twice on the way; and it does
         * not fit a device name anyway.  A board two hops away is reached by
         * the link's relay, not by stacking proxies.
         */
        if (ag_stricmp(info[i].driver, "node") == 0) {
            skipped++;
            continue;
        }
        /*
         * Built somewhere big enough and then CHECKED, rather than truncated
         * into the registry's name field.  A truncated qualified name is not a
         * cosmetic problem: two long device names that differ only past the cut
         * become the same name, the registry refuses the second one as already
         * present, and what an operator sees is one device inexplicably
         * missing.  Refusing it by name, out loud, is the answer that can be
         * acted on - and the cure is to give the node a shorter name.
         */
        char   full[AG_NODE_NAME_MAX + 1 + AG_DEV_NAME_MAX];
        size_t at = 0;
        while (pn[at] != '\0' && at < sizeof(pn) - 1u) {
            full[at] = pn[at];
            at++;
        }
        full[at++] = '.';
        /*
         * By length, not by NUL: this name came off a wire, and nothing on the
         * other side of a wire is obliged to have terminated it.  The bound is
         * the field's own size.
         */
        for (size_t k = 0; k < sizeof(info[i].name) && info[i].name[k] != '\0';
             k++) {
            if (at + 1u >= sizeof(full)) {
                break;
            }
            full[at++] = info[i].name[k];
        }
        full[at] = '\0';
        if (at >= AG_DEV_NAME_MAX) {
            ag_log(AG_LOG_WARN, TAG,
                   "%s does not fit a device name; give the node a shorter one",
                   full);
            continue;
        }

        while (slot < RD_MAX_PROXIES && s_proxy[slot].used) {
            slot++;
        }
        if (slot >= RD_MAX_PROXIES) {
            /*
             * And what did NOT fit, by name.  A listing that quietly stops at
             * a limit is the worst kind of answer: everything looks healthy and
             * the device somebody actually wanted is simply absent.
             */
            ag_log(AG_LOG_WARN, TAG,
                   "%s and what follows did not fit (room for %d devices of "
                   "every neighbour); raise RD_MAX_PROXIES",
                   full, (int)RD_MAX_PROXIES);
            break;
        }
        rdev_t *p = &s_proxy[slot];
        memset(p, 0, sizeof(*p));
        ag_strlcpy(p->name, info[i].name, sizeof(p->name));
        ag_strlcpy(p->qualified, full, sizeof(p->qualified));
        p->far_handle = -1;
        p->used = true;
        p->wire = wire;
        p->node = node;
        p->src = (int8_t)src;
        /*
         * A display gets the class vtable as well as the byte one.  Without it
         * the graphics layer walks past this device and the screen on the
         * other board is listed and unusable.  Which of several is THE screen
         * is pick_display's; the far board's driver name says panel or buffer.
         */
        p->display = (info[i].cls == AG_DEV_DISPLAY);
        p->panel = p->display && (ag_stricmp(info[i].driver, "soft") != 0);

        const ag_dev_desc_t desc = {
            .name = p->qualified,
            .driver = "node",
            .cls = info[i].cls,
            .flags = info[i].flags,
            .ops = &k_proxy_ops,
            .class_ops = p->display ? (const void *)&k_proxy_display_ops : NULL,
            .priv = p,
            .owner = (const void *)&s_owner_tok[src],
        };
        ag_err_t re = ag_dev_register(&desc, NULL);
        /*
         * THE OLD ONE OF THIS NAME, REVOKED AND STILL HELD (2 Oct 2026).  The
         * neighbour restarted and its devices were revoked a moment ago; one
         * still open keeps its name until its holder closes it, which the
         * mixer does on its next period.  Registered at once, the CYD's
         * converter came back -17 and the sound stayed on pcmnull for good.
         */
        for (int t = 0; re == -AG_EEXIST && t < 50; t++) {
            ag_port_task_delay(ag_port_ms_to_ticks(20));
            re = ag_dev_register(&desc, NULL);
        }
        if (re != AG_OK) {
            /*
             * SAID OUT LOUD.  This used to be a bare `continue`, and a device
             * that could not be registered simply was not there: the CYD
             * offers sixteen, thirteen arrived, and the three missing were the
             * screen, the touchscreen and the link itself.  The link is
             * excluded on purpose above; the other two vanished without a
             * word, which reads as "the cluster cannot share a screen".
             *
             * A refusal with a reason is a bug report.  A refusal without one
             * is a mystery, and mysteries cost days.
             */
            ag_log(AG_LOG_WARN, TAG, "%s could not be registered here: %d",
                   full, (int)re);
            memset(p, 0, sizeof(*p));
            continue;
        }
        made++;
    }
    ag_port_free(info);
    s_attached_on[src] = (made > 0);
    pick_display();
    ag_log(AG_LOG_INFO, TAG, "%d device(s) of %s are here now (%s %s)",
           (int)made, pn, far ? "through" : "wire", ag_linkprov_name_at(wire));
    if (skipped > 0) {
        ag_log(AG_LOG_INFO, TAG,
               "%d more are %s's own neighbours', reached through it, not "
               "listed here",
               (int)skipped, pn);
    }
    return made;
}

int32_t ag_noderdev_attach(uint32_t timeout_ms)
{
    const int32_t wire = ag_linkprov_current();
    if (wire < 0) {
        return -AG_ENODEV;
    }
    return ag_noderdev_attach_on(wire, timeout_ms);
}

uint32_t ag_noderdev_detach(void)
{
    uint32_t gone = 0;
    for (int32_t src = 0; src < RD_SRCS; src++) {
        gone += detach_src(src);
    }
    return gone;
}

/* ---- by itself, when the board docked ----------------------------------- */

/*
 * A BOARD THAT ASKED TO DOCK GETS ITS NEIGHBOURS' DEVICES WITHOUT ASKING AGAIN.
 *
 * `link devs` was the only way in, on the reasoning that pulling a far
 * registry into this one is a thing to ask for rather than discover.  It is -
 * and a board whose SYSTEM.CFG binds its wires at boot HAS asked, once, for
 * all of it.  Maxim, 25 Sep 2026: all devices attach by themselves when a
 * board has asked for cluster docking; the boards know nothing about wires, a
 * list of devices simply appears.  So a wire bound at boot attaches its
 * neighbour's devices the moment the neighbour says its name, and again every
 * time it says it anew (it restarted: every handle it gave out is gone).  A
 * wire bound by hand at the prompt still waits for `link devs`.
 *
 * Not on the reader, where the hello arrives: attaching asks the neighbour for
 * its list and waits for the answer, which the reader brings.  So a small task
 * does it, made when there is something to do and gone after.
 */
static volatile bool     s_auto;
static volatile uint32_t s_want; /* wires whose neighbour has named itself */
static volatile bool     s_att_running;
static volatile int32_t  s_att_wire = -1; /* being attached right now */
static ag_port_task_t    s_att_task;

void ag_noderdev_auto(bool on)
{
    s_auto = on;
}

int32_t ag_noderdev_wire_for(uint32_t axe_flags)
{
    if ((axe_flags & AG_AXE_NEEDS_GFX) != 0u && s_display_proxy != NULL &&
        s_display_proxy->used) {
        return s_display_proxy->wire;
    }
    return -1;
}

static void attach_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t bits = s_want;
        if (bits == 0u) {
            s_att_running = false;
            if (s_want == 0u) {
                break;
            }
            s_att_running = true;
            continue;
        }
        int32_t w = 0;
        while (((bits >> w) & 1u) == 0u) {
            w++;
        }
        s_want &= ~(1u << w);
        s_att_wire = w; /* a source, which for a neighbour is its wire */
        /*
         * Three tries a second apart.  The neighbour that has just said hello
         * is still starting its own services, and a listing it has no answer
         * for yet is a timeout here, not an absence of devices.
         */
        int32_t r = -AG_ETIMEDOUT;
        for (int t = 0; t < 3; t++) {
            r = attach_src(w, 4000u);
            if (r >= 0 || (r != -AG_ETIMEDOUT && r != -AG_EAGAIN &&
                           r != -AG_EBUSY)) {
                break;
            }
            ag_port_task_delay(ag_port_ms_to_ticks(1000));
        }
        if (w >= (int32_t)AG_LINK_WIRES) {
            const int k = (int)(w - (int32_t)AG_LINK_WIRES);
            s_far_failed[k] = (r < 0);
            s_far_failed_ms[k] = (uint32_t)(ag_port_us() / 1000);
        }
        if (r < 0) {
            ag_log(AG_LOG_WARN, TAG,
                   "%s's devices did not attach: %d (asked again later)",
                   w >= (int32_t)AG_LINK_WIRES
                       ? s_far_name[w - (int32_t)AG_LINK_WIRES]
                   : ag_node_peer_name_at(w) != NULL ? ag_node_peer_name_at(w)
                                                     : "the neighbour",
                   (int)r);
        }
        s_att_wire = -1;
    }
    ag_port_task_delete(NULL);
}

static void want_src(int32_t src);

void ag_noderdev_peer_named(int32_t wire, bool again)
{
    if (again) {
        s_glass_orphaned = true; /* whatever it drew here, it drew before */
        const char *const pn = (wire >= 0 && wire < (int32_t)AG_LINK_WIRES)
                                   ? ag_node_peer_name_at(wire)
                                   : NULL;
        if (pn != NULL) {
            reborn_mark(wire, ag_node_hash(pn));
        }
    }
    if (!s_auto || wire < 0 || wire >= (int32_t)AG_LINK_WIRES) {
        return;
    }
    /*
     * An answer to this board's own hello attaches only a wire that has
     * nothing yet.  Both halves of one exchange used to attach, so every
     * neighbour's devices were listed, registered, revoked and registered
     * again a few milliseconds apart at every boot.
     */
    if (!again && (s_attached_on[wire] || s_att_wire == wire ||
                   (s_want & (1u << wire)) != 0u)) {
        return;
    }
    want_src(wire);
}

static void want_src(int32_t src)
{
    s_want |= (1u << src);
    if (s_att_running) {
        return;
    }
    s_att_running = true;
    /* As the other cluster workers: an internal stack, else PSRAM. */
    if (!ag_proc_task_create(attach_task, "rdevatt", 4096, NULL, 1,
                             AG_PORT_ANY_CORE, &s_att_task)) {
        s_att_running = false;
        ag_log(AG_LOG_WARN, TAG, "no task to attach the neighbour's devices; "
                                 "`link devs` does it by hand");
    }
}

/*
 * AND A NODE BEYOND A NEIGHBOUR THAT RESTARTED (6 Oct 2026).  Its hello does
 * not reach this board, but its count of starts does, relayed with its name:
 * a count that changed is a restart, and its sessions here go the way a
 * neighbour's do (A NEIGHBOUR THAT RESTARTED, above).  Kept apart from the
 * attach slots below, which only exist when docking is on - sessions are
 * served either way.
 */
static char    s_heard_name[RD_FAR][AG_NODE_NAME_MAX];
static uint8_t s_heard_starts[RD_FAR];

static void far_heard(const char *name, uint8_t starts)
{
    int free_slot = -1;
    for (int k = 0; k < RD_FAR; k++) {
        if (strcmp(s_heard_name[k], name) == 0) {
            if (s_heard_starts[k] != starts) {
                s_heard_starts[k] = starts;
                reborn_mark((int32_t)AG_LINK_WIRES + k, ag_node_hash(name));
            }
            return;
        }
        if (free_slot < 0 && s_heard_name[k][0] == '\0') {
            free_slot = k;
        }
    }
    if (free_slot >= 0) {
        ag_strlcpy(s_heard_name[free_slot], name, sizeof(s_heard_name[0]));
        s_heard_starts[free_slot] = starts;
    }
}

void ag_noderdev_far_named(int32_t via, const char *name, uint8_t starts)
{
    if (name != NULL && name[0] != '\0' && strlen(name) < AG_NODE_NAME_MAX) {
        far_heard(name, starts);
    }
    if (!s_auto || via < 0 || via >= (int32_t)AG_LINK_WIRES || name == NULL ||
        name[0] == '\0' || strlen(name) >= AG_NODE_NAME_MAX) {
        return;
    }
    int free_slot = -1;
    for (int k = 0; k < RD_FAR; k++) {
        if (strcmp(s_far_name[k], name) == 0) {
            if (s_far_wire[k] == via && s_far_starts[k] != starts &&
                s_attached_on[(int32_t)AG_LINK_WIRES + k]) {
                /* it started over: what it lent is gone, attach it again */
                ag_log(AG_LOG_INFO, TAG,
                       "%s started over; its devices again", name);
                s_far_starts[k] = starts;
                s_attached_on[(int32_t)AG_LINK_WIRES + k] = false;
                s_far_failed[k] = false;
                want_src((int32_t)AG_LINK_WIRES + k);
                return;
            }
            if (s_far_wire[k] == via &&
                (s_attached_on[(int32_t)AG_LINK_WIRES + k] ||
                 s_att_wire == (int32_t)AG_LINK_WIRES + k ||
                 (s_want & (1u << ((int32_t)AG_LINK_WIRES + k))) != 0u)) {
                return; /* heard of again, and already here or on its way */
            }
            /* a failure is asked again, but not at every list */
            if (s_far_wire[k] == via && s_far_failed[k] &&
                (uint32_t)(ag_port_us() / 1000) - s_far_failed_ms[k] <
                    RD_FAR_RETRY_MS) {
                return;
            }
            free_slot = k;
            break;
        }
        if (free_slot < 0 && s_far_name[k][0] == '\0') {
            free_slot = k;
        }
    }
    if (free_slot < 0) {
        ag_log(AG_LOG_WARN, TAG,
               "%s is beyond a neighbour, and there is room for %d such nodes' "
               "devices; raise RD_FAR",
               name, RD_FAR);
        return;
    }
    ag_strlcpy(s_far_name[free_slot], name, sizeof(s_far_name[free_slot]));
    s_far_wire[free_slot] = via;
    s_far_starts[free_slot] = starts;
    want_src((int32_t)AG_LINK_WIRES + free_slot);
}
