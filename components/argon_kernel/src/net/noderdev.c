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

static ag_err_t send_raw(uint8_t op, uint16_t tag, uint32_t h, uint32_t a,
                         uint32_t b, uint8_t cls, const void *pay,
                         uint16_t plen)
{
    uint8_t buf[RD_HDR + RD_MAX_PAYLOAD];
    if (plen > RD_MAX_PAYLOAD) {
        plen = RD_MAX_PAYLOAD;
    }
    buf[0] = op;
    buf[1] = 0;
    put16(buf + 2, tag);
    put32(buf + 4, h);
    put32(buf + 8, a);
    put32(buf + 12, b);
    if (plen > 0 && pay != NULL) {
        memcpy(buf + RD_HDR, pay, plen);
    }
    for (int t = 0; t < 2000; t++) {
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

    const int64_t give_up = ag_port_us() + 4000000;
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

    int32_t rc = (int32_t)send_raw(op, s_tag, h, a, b, cls, pay, plen);
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

/* Enough for a second board's worth.  A third board doubles this, and that is
 * the moment to make it a list rather than an array. */
#define RD_MAX_PROXIES 12
static rdev_t s_proxy[RD_MAX_PROXIES];

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
    /*
     * STREAM, not BULK.  Bytes going to a device are usually somebody's sound
     * or somebody's picture: they have a deadline, and a file copy must not be
     * able to push in front of them.  A device write that is genuinely bulk
     * costs a little latency it does not need; the reverse costs a dropout.
     */
    return ask(RD_WRITE, (uint32_t)p->far_handle, 0, (uint32_t)off,
               AG_LINK_STREAM, buf, (uint16_t)len, NULL, NULL);
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
    (void)send_raw((uint8_t)(op | RD_REPLY), tag, (uint32_t)status, 0, 0, cls,
                   pay, plen);
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
        reply(op, tag, r, AG_LINK_STREAM, obuf,
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
        if (ag_dev_register(&desc, NULL) != AG_OK) {
            p->used = false;
            continue;
        }
        made++;
    }
    s_attached = (made > 0);
    ag_log(AG_LOG_INFO, TAG, "%d device(s) of %s are here now", (int)made,
           peer);
    return made;
}

uint32_t ag_noderdev_detach(void)
{
    const uint32_t gone = ag_dev_revoke_owner((const void *)&s_proxy);
    memset(s_proxy, 0, sizeof(s_proxy));
    s_attached = false;
    return gone;
}
