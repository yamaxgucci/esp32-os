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
#include <argon/port/mem.h>

#include "net/rdpaint.h"
#include <argon/libc.h>
#include <argon/linkprov.h>
#include <argon/log.h>
#include <argon/nodedev.h>
#include <argon/nodename.h>

#include <argon/port/task.h>
#include <argon/port/time.h>

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
static bool s_attached;

static volatile bool     s_busy;
static volatile uint16_t s_tag;
static volatile bool     s_have;
static volatile int32_t  s_status;
static uint8_t           s_rbuf[RD_MAX_PAYLOAD];
static volatile uint16_t s_rlen;

static ag_err_t send_raw(uint8_t op, uint8_t flags, uint16_t tag, uint32_t h,
                         uint32_t a, uint32_t b, uint8_t cls, const void *pay,
                         uint16_t plen)
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
            ag_linkprov_send(RD_CHAN, cls, buf, (uint16_t)(RD_HDR + plen));
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

static int32_t ask(uint8_t op, uint32_t h, uint32_t a, uint32_t b, uint8_t cls,
                   const void *pay, uint16_t plen, void *out, uint16_t *outlen)
{
    static uint16_t next_tag = 1;

    if (!ag_linkprov_bound()) {
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

    int32_t rc = (int32_t)send_raw(op, 0, s_tag, h, a, b, cls, pay, plen);
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
        const int64_t deadline = ag_port_us() + 1500000;
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
typedef struct {
    char    name[AG_DEV_NAME_MAX]; /* the device's own name, over there */
    char    qualified[AG_DEV_NAME_MAX];
    int32_t far_handle;
    bool    used;
} rdev_t;

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
    const int32_t r = ask(RD_OPEN, 0, flags, 0, AG_LINK_CONTROL, p->name,
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
    const int32_t r = ask(RD_CLOSE, (uint32_t)p->far_handle, 0, 0,
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
    return ask(RD_READ, (uint32_t)p->far_handle, (uint32_t)len, (uint32_t)off,
               AG_LINK_CONTROL, NULL, 0, buf, &got);
}

/* What this node has poured at the neighbour, for `link` and for a person. */
static uint32_t s_sent_periods;
static uint32_t s_sent_dropped;

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
    uint8_t buf2[RD_HDR + RD_MAX_PAYLOAD];
    if (len > RD_MAX_PAYLOAD) {
        len = RD_MAX_PAYLOAD;
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
    const int32_t r = ag_linkprov_send(RD_CHAN, AG_LINK_STREAM, buf2,
                                       (uint16_t)(RD_HDR + len));
    if (r < 0) {
        s_sent_dropped++;
        return (int32_t)len; /* let go of it, and say the period was taken */
    }
    s_sent_periods++;
    return (int32_t)len;
}

static int32_t rd_write(ag_device_t *dev, const void *buf, size_t len,
                        uint64_t off)
{
    rdev_t *p = (rdev_t *)dev->priv;
    if (p == NULL) {
        return -AG_ENODEV;
    }
    if (len > RD_MAX_PAYLOAD) {
        len = RD_MAX_PAYLOAD;
    }

    if (dev->cls == AG_DEV_AUDIO) {
        return pour(p, buf, len, off);
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
    return ask(RD_WRITE, (uint32_t)p->far_handle, 0, (uint32_t)off,
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
    const int32_t r = ask(RD_IOCTL, (uint32_t)p->far_handle, cmd, 0,
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
    const int32_t r = ask(RD_SIZE, (uint32_t)p->far_handle, 0, 0,
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
static ag_err_t ensure_open(rdev_t *p)
{
    if (p->far_handle >= 0) {
        return AG_OK;
    }
    const int32_t r = ask(RD_OPEN, 0, 0, 0, AG_LINK_CONTROL, p->name,
                          (uint16_t)strlen(p->name), NULL, NULL);
    if (r < 0) {
        return (ag_err_t)r;
    }
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
/* What one poured message may carry on the wire that is bound now. */
static uint16_t rd_pour_payload(void)
{
    ag_link_info_t li;
    uint32_t       room = RD_MAX_PAYLOAD;
    if (ag_linkprov_info(&li) == AG_OK && li.mtu > RD_HDR) {
        room = li.mtu - RD_HDR;
    }
    if (room > RD_POUR_CAP) {
        room = RD_POUR_CAP;
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
    const int32_t r = ask(RD_GFXINFO, (uint32_t)p->far_handle, 0, 0,
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
    const int32_t r = ag_linkprov_send(RD_CHAN, AG_LINK_BULK, msg,
                                       (uint16_t)(RD_HDR + len));
    return (r >= 0) ? AG_OK : (ag_err_t)r;
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
    const uint16_t pixroom = (uint16_t)((rd_pour_payload() - RD_BLIT_HDR) / 2u);

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
            (void)ag_linkprov_send(
                RD_CHAN, AG_LINK_STREAM, msg,
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
static void rd_blit(ag_handle_t h, const ag_blit_t *b)
{
    rdev_t *p = proxy_of(h);
    if (p == NULL || b == NULL || b->px == NULL || b->w == 0 || b->h == 0) {
        return;
    }
    if (ensure_open(p) != AG_OK) {
        return;
    }
    if (!rdp_damage(b, (uint32_t)p->far_handle, rd_pour_payload(),
                    rd_send_z)) {
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
    const int32_t r = ask(RD_TEXTINFO, (uint32_t)p->far_handle, 0, 0,
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
    (void)ag_linkprov_send(
        RD_CHAN, AG_LINK_STREAM, msg,
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
    (void)ag_linkprov_send(RD_CHAN, AG_LINK_STREAM, msg,
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

static void reply(uint8_t op, uint16_t tag, int32_t status, uint8_t cls,
                  const void *pay, uint16_t plen)
{
    const ag_err_t e = send_raw((uint8_t)(op | RD_REPLY), 0, tag,
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

/*
 * What the far node has open here.  Small on purpose: this is one neighbour's
 * use of this node's devices, not a general handle table.
 */
#define RD_MAX_SESSIONS 8
static ag_device_t *s_session[RD_MAX_SESSIONS];

/* The request being served, and the worker serving it. */
static uint8_t        s_req[RD_HDR + RD_MAX_PAYLOAD];
static volatile uint16_t s_req_len;
static volatile bool  s_working;
static ag_port_task_t s_worker;

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
 * Four because that is about forty milliseconds - enough to ride out a burst,
 * short enough that a period which has been waiting that long is already too
 * old to be worth playing.
 *
 * FULL MEANS THE NEWEST IS DROPPED, not the oldest, and that is what makes this
 * safe without a lock: only the reader task moves the write end and only the
 * pouring task moves the read end.  It is also the honest choice - a period
 * that cannot be taken now has missed its moment, which is the same rule the
 * sending side follows.
 */
#define RD_RING 4

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
} rd_period_t;

/* Said once: a flood of identical warnings is a flood, not a diagnosis. */
static bool              s_no_sink_said;

static uint32_t serve_poured_class(ag_device_t *dev,
                                   const rd_period_t *pd);

static rd_period_t       s_ring[RD_RING];
static uint8_t          *s_ring_mem;
static volatile uint32_t s_ring_w;
static volatile uint32_t s_ring_r;
static ag_port_task_t    s_pour_task;

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
static uint32_t s_pour_said_ms;
static uint32_t s_pour_said_periods;
static uint32_t s_pour_said_lost;
static uint32_t s_pour_last_ms;
static bool     s_pour_said;

static void serve_one(const uint8_t *b, uint16_t n)
{
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

/*
 * Says what arrived, once per spell of sound rather than once per period.
 *
 * Per period would be a hundred lines a second into a four-kilobyte journal,
 * which would bury the thing it is there to show.  At the end of a burst it is
 * one line, it is the count kept on the side that received the bytes, and it is
 * the only number in this path that is evidence of anything.
 */
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
    ag_log(AG_LOG_INFO, TAG, "poured %u periods, %u bytes, %u lost",
           (unsigned)s_pour_periods, (unsigned)s_pour_bytes,
           (unsigned)s_pour_lost);
    s_pour_said = true;
    s_pour_said_ms = now;
    s_pour_said_periods = s_pour_periods;
    s_pour_said_lost = s_pour_lost;
}

static void pour_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_ring_r == s_ring_w) {
            /* Woken by the next period, and on the clock anyway so the end of
             * a burst gets its line without anything having to notice. */
            ag_port_notify_take(true, ag_port_ms_to_ticks(100));
            pour_report();
            continue;
        }

        rd_period_t *pd = &s_ring[s_ring_r];
        ag_device_t *dev = (pd->handle < (uint32_t)RD_MAX_SESSIONS)
                               ? s_session[pd->handle]
                               : NULL;
        if (dev != NULL) {
            /*
             * An ordinary device write, which is exactly the point: it takes
             * that device's own lock and no longer the registry's, so a
             * converter that makes this wait stops nothing else on this board.
             * Before that was true, this path froze the machine.
             */
            if (pd->op != RD_WRITE) {
                /*
                 * A class call rather than bytes: the pixels of a rectangle,
                 * a row of characters, the caret.  It goes to the device's
                 * class vtable, and it is here rather than on the reader task
                 * for the same reason a write is - putting a band on a panel
                 * takes as long as the panel takes.
                 */
                const uint32_t did = serve_poured_class(dev, pd);
                if (did > 0u) {
                    s_pour_periods++;
                    s_pour_bytes += did;
                } else {
                    s_pour_lost++;
                }
                s_ring_r = (s_ring_r + 1u) % RD_RING;
                continue;
            }
            const int32_t n = ag_dev_write(dev, pd->data, pd->len, pd->off);
            if (n > 0) {
                s_pour_periods++;
                s_pour_bytes += (uint32_t)n;
            } else {
                s_pour_lost++;
            }
        } else {
            /*
             * Nothing to write it to.  Said once, by name of the handle: a
             * period with no session is not a wire fault and not a converter
             * fault, it is an open that did not happen or did not survive, and
             * the three are indistinguishable from a count of losses alone.
             */
            if (!s_no_sink_said) {
                ag_log(AG_LOG_WARN, TAG,
                       "poured sound has nowhere to go: no open device for "
                       "handle %u", (unsigned)pd->handle);
                s_no_sink_said = true;
            }
            s_pour_lost++;
        }
        s_pour_last_ms = (uint32_t)(ag_port_us() / 1000);
        s_pour_said = false;
        s_ring_r = (s_ring_r + 1u) % RD_RING;
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
static uint32_t serve_poured_class(ag_device_t *dev, const rd_period_t *pd)
{
    const ag_display_ops_t *d = (const ag_display_ops_t *)dev->class_ops;
    const uint8_t          *pay = pd->data;
    const uint16_t          plen = pd->len;

    switch (pd->op) {
    case RD_BLIT: {
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
        d->blit_rect(0, &bl);
        return want;
    }
    case RD_BLITZ:
        return rdp_draw(d, pay, plen);
    case RD_TEXTROW: {
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

static void take_period(const uint8_t *b, uint16_t n)
{
    if (s_ring_mem == NULL) {
        s_ring_mem = (uint8_t *)ag_port_alloc((size_t)RD_RING * RD_POUR_CAP,
                                              AG_MEM_BYTE);
        if (s_ring_mem == NULL) {
            s_pour_lost++;
            return;
        }
        for (uint32_t i = 0; i < RD_RING; i++) {
            s_ring[i].data = s_ring_mem + (size_t)i * RD_POUR_CAP;
        }
    }
    if (s_pour_task == NULL) {
        /*
         * Made on the first period rather than at start-up: a board that never
         * has sound poured at it should not carry a task and its stack for the
         * possibility.  Below applications, like every other task this file's
         * neighbours make.
         */
        if (!ag_port_task_create(pour_task, "rdevpour", 3072, NULL, 1,
                                 AG_PORT_ANY_CORE, 0, &s_pour_task)) {
            s_pour_lost++;
            return;
        }
        ag_log(AG_LOG_INFO, TAG, "sound is being poured at us from %s",
               ag_node_peer_name() != NULL ? ag_node_peer_name() : "the wire");
    }

    uint32_t next = (s_ring_w + 1u) % RD_RING;
    if (next == s_ring_r && b[0] == RD_BLITZ) {
        /*
         * A screen kept by what changed cannot miss a change: this waits for
         * the pour task, and the wire waits behind it - which is the screen
         * being slower than the wire, said as back-pressure.
         */
        for (int k = 0; k < 1000 && next == s_ring_r; k++) {
            ag_port_task_delay(1);
            next = (s_ring_w + 1u) % RD_RING;
        }
    }
    if (next == s_ring_r) {
        s_pour_lost++; /* full: this one missed its moment */
        return;
    }
    rd_period_t *pd = &s_ring[s_ring_w];
    uint16_t     plen = (uint16_t)(n - RD_HDR);
    if (plen > RD_POUR_CAP) {
        plen = (uint16_t)RD_POUR_CAP;
    }
    memcpy(pd->data, b + RD_HDR, plen);
    pd->len = plen;
    pd->handle = get32(b + 4);
    pd->off = get32(b + 12);
    pd->op = b[0];
    s_ring_w = next;
    ag_port_notify_give(s_pour_task);
}

static void on_msg(uint8_t chan, const void *data, uint16_t n, void *ctx)
{
    (void)chan;
    (void)ctx;
    if (n < RD_HDR) {
        return;
    }
    const uint8_t *b = (const uint8_t *)data;

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
    if (s_working) {
        reply(b[0], get16(b + 2), -AG_EBUSY, AG_LINK_CONTROL, NULL, 0);
        return;
    }
    if (n > sizeof(s_req)) {
        n = (uint16_t)sizeof(s_req);
    }
    memcpy(s_req, b, n);
    s_req_len = n;
    s_working = true;
    if (!ag_port_task_create(worker, "rdevsrv", 4096, NULL, 1,
                             AG_PORT_ANY_CORE, 0, &s_worker)) {
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
    return AG_OK;
}

/* ---- attaching ---------------------------------------------------------- */

int32_t ag_noderdev_attach(uint32_t timeout_ms)
{
    const char *peer = ag_node_peer_name();
    if (peer == NULL) {
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
    if (s_attached) {
        (void)ag_noderdev_detach();
    }

    ag_devinfo_t  info[RD_MAX_PROXIES];
    const int32_t n = ag_nodedev_list(info, RD_MAX_PROXIES, timeout_ms);
    if (n < 0) {
        return n;
    }

    /*
     * Into a bounded buffer first.  ag_node_peer_name returns a pointer and the
     * compiler cannot see that what is behind it is already short, so the
     * qualified name below would be built from something of unknown length.
     * The copy is exact - a name is validated against this same bound when it
     * arrives - and it makes the bound visible where it is used.
     */
    char pn[AG_NODE_NAME_MAX];
    ag_strlcpy(pn, peer, sizeof(pn));

    int32_t made = 0;
    if (proxy_table() == NULL) {
        ag_log(AG_LOG_ERROR, TAG, "no memory for the neighbour's devices");
        return 0;
    }

    for (int32_t i = 0; i < n && made < RD_MAX_PROXIES; i++) {
        /*
         * Not the link itself.  A proxy for the far node's own link device
         * would be a device reached over the very thing it describes, and
         * `link use sound.node0` would be a loop with no bottom.
         */
        if (info[i].cls == AG_DEV_LINK) {
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
        char full[AG_NODE_NAME_MAX + 1 + AG_DEV_NAME_MAX];
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

        rdev_t *p = &s_proxy[made];
        memset(p, 0, sizeof(*p));
        ag_strlcpy(p->name, info[i].name, sizeof(p->name));
        ag_strlcpy(p->qualified, full, sizeof(p->qualified));
        p->far_handle = -1;
        p->used = true;

        /*
         * A display gets the class vtable as well as the byte one.  Without it
         * the graphics layer walks past this device and the screen on the
         * other board is listed and unusable.
         */
        const bool is_display = (info[i].cls == AG_DEV_DISPLAY);
        if (is_display) {
            /*
             * A PANEL BEATS A BUFFER IN MEMORY, and that is a fact about
             * screens rather than about registries - the same shape as "a real
             * converter beats the null one", which is why the audio list lives
             * in the mixer.  Taking whichever came first sent a megabyte of
             * pixels to the far board's SOFT framebuffer, where nobody was
             * looking, while its ST7789 sat beside it in the same listing.
             *
             * The far board's driver name is what says which is which; it
             * travels in the device record already.  Same family of mistake as
             * pcmnull always being present.
             */
            const bool real = (ag_stricmp(info[i].driver, "soft") != 0);
            const bool better =
                (s_display_proxy == NULL) ||
                (real && !s_display_real);
            if (better) {
                if (s_display_proxy != NULL && s_display_proxy != p) {
                    ag_log(AG_LOG_INFO, TAG,
                           "%s (%s) takes the screen from %s", full,
                           info[i].driver, s_display_proxy->qualified);
                }
                s_display_proxy = p;
                s_display_real = real;
            } else if (s_display_proxy != p) {
                ag_log(AG_LOG_INFO, TAG, "%s: %s already has the screen", full,
                       s_display_proxy->qualified);
            }
        }

        const ag_dev_desc_t desc = {
            .name = p->qualified,
            .driver = "node",
            .cls = info[i].cls,
            .flags = info[i].flags,
            .ops = &k_proxy_ops,
            .class_ops = is_display ? (const void *)&k_proxy_display_ops : NULL,
            .priv = p,
            .owner = (const void *)&s_proxy,
        };
        const ag_err_t re = ag_dev_register(&desc, NULL);
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
            p->used = false;
            continue;
        }
        made++;
    }
    s_attached = (made > 0);
    ag_log(AG_LOG_INFO, TAG, "%d device(s) of %s are here now", (int)made,
           peer);
    /*
     * And what did NOT fit, by name of the count.  A listing that quietly
     * stops at a limit is the worst kind of answer: everything looks healthy
     * and the device somebody actually wanted is simply absent.
     */
    if (n > (int32_t)RD_MAX_PROXIES) {
        ag_log(AG_LOG_WARN, TAG,
               "%d of %s's devices did not fit (room for %d); raise "
               "RD_MAX_PROXIES",
               (int)(n - (int32_t)RD_MAX_PROXIES), peer, (int)RD_MAX_PROXIES);
    }
    return made;
}

uint32_t ag_noderdev_detach(void)
{
    const uint32_t gone = ag_dev_revoke_owner((const void *)&s_proxy);
    /*
     * The TABLE, not the pointer to it.  When this was an array `sizeof`
     * meant the whole thing; the moment it became a heap pointer the same
     * line meant four bytes, and the table would have been left full of
     * handles to a node that is no longer there.  GCC caught it
     * (-Wsizeof-pointer-memaccess); it is the reason that warning exists.
     */
    if (s_proxy != NULL) {
        memset(s_proxy, 0, sizeof(rdev_t) * RD_MAX_PROXIES);
    }
    s_attached = false;
    return gone;
}
