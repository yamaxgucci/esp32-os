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
    uint8_t  data[RD_MAX_PAYLOAD];
    uint16_t len;
    uint32_t handle;
    uint32_t off;
} rd_period_t;

static rd_period_t       s_ring[RD_RING];
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
    if (s_pour_said || s_pour_periods == 0) {
        return;
    }
    const uint32_t now = (uint32_t)(ag_port_us() / 1000);
    if ((uint32_t)(now - s_pour_last_ms) < 400u) {
        return;
    }
    ag_log(AG_LOG_INFO, TAG, "poured %u periods, %u bytes, %u lost",
           (unsigned)s_pour_periods, (unsigned)s_pour_bytes,
           (unsigned)s_pour_lost);
    s_pour_said = true;
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
            const int32_t n = ag_dev_write(dev, pd->data, pd->len, pd->off);
            if (n > 0) {
                s_pour_periods++;
                s_pour_bytes += (uint32_t)n;
            } else {
                s_pour_lost++;
            }
        } else {
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
static void take_period(const uint8_t *b, uint16_t n)
{
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

    const uint32_t next = (s_ring_w + 1u) % RD_RING;
    if (next == s_ring_r) {
        s_pour_lost++; /* full: this one missed its moment */
        return;
    }
    rd_period_t *pd = &s_ring[s_ring_w];
    uint16_t     plen = (uint16_t)(n - RD_HDR);
    if (plen > sizeof(pd->data)) {
        plen = (uint16_t)sizeof(pd->data);
    }
    memcpy(pd->data, b + RD_HDR, plen);
    pd->len = plen;
    pd->handle = get32(b + 4);
    pd->off = get32(b + 12);
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

        const ag_dev_desc_t desc = {
            .name = p->qualified,
            .driver = "node",
            .cls = info[i].cls,
            .flags = info[i].flags,
            .ops = &k_proxy_ops,
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
