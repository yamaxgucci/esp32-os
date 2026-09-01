/*
 * ArgonOS - external radio (.SYS): an AG_DEV_NET device backed by a coprocessor
 * on UART, speaking RLINK (apps/common/rlink).  Bind it with `net use extradio`
 * and the whole socket path - wget, ftp, ssh, mqtt, httpd - runs over the wire
 * instead of the built-in stack, so a board can carry no radio of its own.
 *
 * The coprocessor is the far end of a UART.  In QEMU that is a third serial the
 * fake radio (tools/rlinkd.py) answers; on hardware it is an ESP8266/ESP32/
 * modem running RLINK firmware.  Which chip, and whether it speaks AT or a
 * hosted protocol underneath, is the coprocessor's business - up here it is
 * just RLINK.
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include --include apps/common --include apps/common/rlink \
 *       -o build\apps\EXTRADIO.SYS apps/extradio/extradio.c apps/common/rlink/ag_rlink.c
 *   drv load a:\extradio.sys      (registers /dev/extradio)
 *   net use extradio              (route the network through it)
 *
 * NO BACKGROUND TASK.  A .SYS runs its ag_driver_init in kernel context, where
 * there is no current process, so api->task->create refuses it (a thread must
 * belong to a process that can be made to end).  This is the same bind HostFS
 * is in, and the same answer: pump the UART inline.  Every RLINK frame the
 * coprocessor sends - the reply to a request, or an unsolicited DATA/EVENT push
 * - is read by whichever call happens to be touching the wire.  rpc() reads
 * frames until its own reply arrives, buffering any pushes it passes on the way;
 * recv_now/wait_readable read whatever has already arrived and drain the per-
 * channel rings.  One mutex serialises all of it, so there is a single reader
 * and the frame assembler needs no locking of its own.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

#include "rlink/ag_rlink.h"

AG_DRV("EXTRADIO", "0.1", "argon");

#define EXT_UART_DEFAULT 2      /* the free UART a board wires the radio to  */
#define EXT_BAUD         115200u
#define EXT_MAX_CHAN     8      /* coprocessor socket ids must land in [0,8) */
#define EXT_RING         2048u  /* per-channel receive ring */
#define EXT_RPC_MS       8000u  /* an RPC that gets no reply is a dead link */
#define EXT_SCRATCH      256u

/* frame assembler states */
enum { SM_SYNC = 0, SM_HDR, SM_PAYLOAD };

typedef struct {
    bool     used;
    bool     eof; /* far side closed; drain the ring, then it is EOF */
    uint32_t r, w, fill;
    uint8_t  buf[EXT_RING];
} chan_t;

typedef struct {
    int        uart;
    ag_mutex_t lock; /* serialises the wire, the assembler and the rings */
    uint16_t   seq;

    /* frame assembler (single reader, so no lock of its own) */
    int            sm;
    uint8_t        win[4];
    uint8_t        hdr[RL_HDR_SIZE];
    uint32_t       hlen;
    ag_rlink_hdr_t cur;
    uint8_t        payload[RL_MAX_PAYLOAD];
    uint32_t       plen;

    /* the reply the current rpc() is waiting for */
    bool           waiting;
    uint16_t       want_seq;
    bool           got;
    ag_rlink_hdr_t rep;
    uint8_t        repbuf[RL_MAX_PAYLOAD];

    bool     ready;
    uint32_t ifaddr;

    chan_t chan[EXT_MAX_CHAN];
    bool   nb[EXT_MAX_CHAN]; /* O_NONBLOCK per channel */
} ext_t;

static ext_t s_ext;

static const ag_io_api_t   *IO;
static const ag_task_api_t *TASK;

/* ---------------------------------------------------------------------- */
/* UART                                                                    */
/* ---------------------------------------------------------------------- */

static void uart_send(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t         left = len;
    while (left > 0) {
        const int32_t n = IO->uart_write(s_ext.uart, p, left);
        if (n <= 0) {
            return; /* link gone; the pending rpc will time out */
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
}

/* ---------------------------------------------------------------------- */
/* Channels                                                                */
/* ---------------------------------------------------------------------- */

static void chan_open(int ch)
{
    chan_t *c = &s_ext.chan[ch];
    c->used = true;
    c->eof = false;
    c->r = c->w = c->fill = 0;
    s_ext.nb[ch] = false;
}

static void ring_put(chan_t *c, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (c->fill < EXT_RING) {
            c->buf[c->w] = data[i];
            c->w = (c->w + 1) % EXT_RING;
            c->fill++;
        }
        /* else dropped: v1 has no window back to the coprocessor; the ring
         * absorbs a burst, not a flood. */
    }
}

/* ---------------------------------------------------------------------- */
/* Frame assembler - fed one byte at a time, dispatches complete frames    */
/* ---------------------------------------------------------------------- */

static void dispatch(const ag_rlink_hdr_t *h, const uint8_t *payload)
{
    if ((h->flags & RL_F_RESPONSE) != 0) {
        if (s_ext.waiting && h->seq == s_ext.want_seq) {
            s_ext.rep = *h;
            if (h->len > 0) {
                memcpy(s_ext.repbuf, payload, h->len);
            }
            s_ext.got = true;
        }
        return; /* a reply with no waiter is stale */
    }
    if (h->op == RL_OP_DATA) {
        const int ch = h->ch;
        if (ch >= 0 && ch < EXT_MAX_CHAN) {
            ring_put(&s_ext.chan[ch], payload, h->len);
            if ((h->flags & RL_F_EOF) != 0) {
                s_ext.chan[ch].eof = true;
            }
        }
    } else if (h->op == RL_OP_EVENT) {
        if (h->a0 == RL_EV_GOTIP) {
            s_ext.ready = true;
            s_ext.ifaddr = h->a1;
        } else if (h->a0 == RL_EV_LINKDOWN) {
            s_ext.ready = false;
        }
    }
}

static void feed(uint8_t b)
{
    switch (s_ext.sm) {
    case SM_SYNC:
        s_ext.win[0] = s_ext.win[1];
        s_ext.win[1] = s_ext.win[2];
        s_ext.win[2] = s_ext.win[3];
        s_ext.win[3] = b;
        if (s_ext.win[0] == 0x52 && s_ext.win[1] == 0x4C &&
            s_ext.win[2] == 0x4E && s_ext.win[3] == 0x4B) { /* 'RLNK' */
            memcpy(s_ext.hdr, s_ext.win, 4);
            s_ext.hlen = 4;
            s_ext.sm = SM_HDR;
        }
        break;
    case SM_HDR:
        s_ext.hdr[s_ext.hlen++] = b;
        if (s_ext.hlen == RL_HDR_SIZE) {
            if (!ag_rlink_unpack(&s_ext.cur, s_ext.hdr)) {
                s_ext.sm = SM_SYNC; /* bad frame: resync */
                break;
            }
            if (s_ext.cur.len == 0) {
                dispatch(&s_ext.cur, s_ext.payload);
                s_ext.sm = SM_SYNC;
            } else {
                s_ext.plen = 0;
                s_ext.sm = SM_PAYLOAD;
            }
        }
        break;
    case SM_PAYLOAD:
        s_ext.payload[s_ext.plen++] = b;
        if (s_ext.plen == s_ext.cur.len) {
            dispatch(&s_ext.cur, s_ext.payload);
            s_ext.sm = SM_SYNC;
        }
        break;
    default:
        s_ext.sm = SM_SYNC;
        break;
    }
}

/* Read whatever the UART has (waiting up to block_ms for the first bytes) and
 * feed it through the assembler.  Caller holds the lock. */
static void pump(uint32_t block_ms)
{
    uint8_t scratch[EXT_SCRATCH];
    const int32_t n = IO->uart_read(s_ext.uart, scratch, sizeof(scratch),
                                    block_ms);
    for (int32_t i = 0; i < n; i++) {
        feed(scratch[i]);
    }
}

/* ---------------------------------------------------------------------- */
/* Request / reply                                                         */
/* ---------------------------------------------------------------------- */

static ag_err_t rpc(ag_rlink_hdr_t *req, const void *payload,
                    ag_rlink_hdr_t *rep, uint8_t *repbuf)
{
    TASK->mutex_lock(s_ext.lock, UINT32_MAX);

    s_ext.seq++;
    if (s_ext.seq == 0) {
        s_ext.seq = 1;
    }
    req->seq = s_ext.seq;
    s_ext.want_seq = s_ext.seq;
    s_ext.waiting = true;
    s_ext.got = false;

    uint8_t hdr[RL_HDR_SIZE];
    ag_rlink_pack(hdr, req);
    uart_send(hdr, RL_HDR_SIZE);
    if (req->len > 0 && payload != NULL) {
        uart_send(payload, req->len);
    }

    ag_err_t r = -AG_ETIMEDOUT;
    uint32_t waited = 0;
    while (waited < EXT_RPC_MS) {
        pump(50);
        if (s_ext.got) {
            *rep = s_ext.rep;
            if (repbuf != NULL && rep->len > 0) {
                memcpy(repbuf, s_ext.repbuf, rep->len);
            }
            r = AG_OK;
            break;
        }
        waited += 50;
    }
    s_ext.waiting = false;

    TASK->mutex_unlock(s_ext.lock);
    return r;
}

static ag_err_t rpc_simple(ag_rlink_hdr_t *req, ag_rlink_hdr_t *rep)
{
    return rpc(req, NULL, rep, NULL);
}

/* Take the channel id a reply handed back.  Out-of-range means the coprocessor
 * used more sockets than we can track: close it rather than lose it. */
static int adopt_chan(int ch)
{
    if (ch < 0) {
        return ch; /* an -AG_E* passthrough */
    }
    if (ch >= EXT_MAX_CHAN) {
        ag_rlink_hdr_t req, rep;
        ag_rlink_req(&req, RL_OP_CLOSE, 0);
        req.ch = ch;
        (void)rpc_simple(&req, &rep);
        return -AG_ENFILE;
    }
    TASK->mutex_lock(s_ext.lock, UINT32_MAX);
    chan_open(ch);
    TASK->mutex_unlock(s_ext.lock);
    return ch;
}

/* Pull up to len bytes already buffered.  Caller holds the lock. */
static int32_t drain_locked(int ch, void *buf, size_t len)
{
    if (ch < 0 || ch >= EXT_MAX_CHAN) {
        return -AG_EBADF;
    }
    uint8_t *o = (uint8_t *)buf;
    uint32_t n = 0;
    chan_t  *c = &s_ext.chan[ch];
    while (n < len && c->fill > 0) {
        o[n++] = c->buf[c->r];
        c->r = (c->r + 1) % EXT_RING;
        c->fill--;
    }
    if (n > 0) {
        return (int32_t)n;
    }
    return c->eof ? 0 : -AG_EAGAIN;
}

/* ---------------------------------------------------------------------- */
/* The AG_DEV_NET class vtable                                             */
/* ---------------------------------------------------------------------- */

static ag_err_t ext_start(ag_device_t *dev)
{
    (void)dev;
    ag_rlink_hdr_t req, rep;
    ag_rlink_req(&req, RL_OP_START, 0);
    const ag_err_t e = rpc_simple(&req, &rep);
    if (e != AG_OK) {
        return e;
    }
    return (rep.status < 0) ? (ag_err_t)rep.status : AG_OK;
}

static bool ext_ready(ag_device_t *dev)
{
    (void)dev;
    TASK->mutex_lock(s_ext.lock, UINT32_MAX);
    pump(0); /* catch a GOTIP event that may already be sitting on the wire */
    const bool r = s_ext.ready;
    TASK->mutex_unlock(s_ext.lock);
    return r;
}

static ag_err_t ext_ifaddr(ag_device_t *dev, uint32_t *addr)
{
    (void)dev;
    if (addr == NULL) {
        return -AG_EINVAL;
    }
    TASK->mutex_lock(s_ext.lock, UINT32_MAX);
    pump(0);
    const bool     ready = s_ext.ready;
    const uint32_t a = s_ext.ifaddr;
    TASK->mutex_unlock(s_ext.lock);
    if (!ready) {
        return -AG_EAGAIN;
    }
    *addr = a;
    return AG_OK;
}

static ag_err_t ext_resolve(ag_device_t *dev, const char *host, uint32_t *addr)
{
    (void)dev;
    if (host == NULL || addr == NULL) {
        return -AG_EINVAL;
    }
    const size_t hlen = strlen(host);
    if (hlen == 0 || hlen > RL_MAX_PAYLOAD) {
        return -AG_EINVAL;
    }
    ag_rlink_hdr_t req, rep;
    ag_rlink_req(&req, RL_OP_RESOLVE, 0);
    req.len = (uint32_t)hlen;
    const ag_err_t e = rpc(&req, host, &rep, NULL);
    if (e != AG_OK) {
        return e;
    }
    if (rep.status < 0) {
        return (ag_err_t)rep.status;
    }
    *addr = rep.a0;
    return AG_OK;
}

static int ext_listen(ag_device_t *dev, uint16_t port)
{
    (void)dev;
    ag_rlink_hdr_t req, rep;
    ag_rlink_req(&req, RL_OP_LISTEN, 0);
    req.a0 = port;
    const ag_err_t e = rpc_simple(&req, &rep);
    if (e != AG_OK) {
        return (int)e;
    }
    if (rep.status < 0) {
        return (int)rep.status;
    }
    return adopt_chan(rep.ch);
}

static int ext_accept(ag_device_t *dev, int lfd, uint32_t timeout_ms)
{
    (void)dev;
    ag_rlink_hdr_t req, rep;
    ag_rlink_req_accept(&req, 0, lfd, timeout_ms);
    const ag_err_t e = rpc_simple(&req, &rep);
    if (e != AG_OK) {
        return (int)e;
    }
    if (rep.status < 0) {
        return (int)rep.status;
    }
    return adopt_chan(rep.ch);
}

static int ext_connect(ag_device_t *dev, uint32_t addr, uint16_t port,
                       uint32_t timeout_ms)
{
    (void)dev;
    ag_rlink_hdr_t req, rep;
    ag_rlink_req_connect(&req, 0, addr, port, timeout_ms);
    const ag_err_t e = rpc_simple(&req, &rep);
    if (e != AG_OK) {
        return (int)e;
    }
    if (rep.status < 0) {
        return (int)rep.status;
    }
    return adopt_chan(rep.ch);
}

static int32_t ext_send(ag_device_t *dev, int fd, const void *buf, size_t len)
{
    (void)dev;
    if (fd < 0 || fd >= EXT_MAX_CHAN) {
        return -AG_EBADF;
    }
    if (len == 0) {
        return 0;
    }
    const uint32_t chunk =
        (len > RL_MAX_PAYLOAD) ? RL_MAX_PAYLOAD : (uint32_t)len;
    ag_rlink_hdr_t req, rep;
    ag_rlink_req_send(&req, 0, fd, chunk);
    const ag_err_t e = rpc(&req, buf, &rep, NULL);
    if (e != AG_OK) {
        return (int32_t)e;
    }
    return rep.status; /* bytes accepted, or -AG_E* (incl. -AG_EAGAIN) */
}

static int32_t ext_recv(ag_device_t *dev, int fd, void *buf, size_t len)
{
    (void)dev;
    for (;;) {
        TASK->mutex_lock(s_ext.lock, UINT32_MAX);
        pump(5);
        const int32_t r = drain_locked(fd, buf, len);
        const bool    nb = (fd >= 0 && fd < EXT_MAX_CHAN) ? s_ext.nb[fd] : true;
        TASK->mutex_unlock(s_ext.lock);
        if (r != -AG_EAGAIN) {
            return r;
        }
        if (nb) {
            return -AG_EAGAIN;
        }
        TASK->sleep_ms(1);
    }
}

static int32_t ext_recv_now(ag_device_t *dev, int fd, void *buf, size_t len)
{
    (void)dev;
    TASK->mutex_lock(s_ext.lock, UINT32_MAX);
    pump(0);
    const int32_t r = drain_locked(fd, buf, len);
    TASK->mutex_unlock(s_ext.lock);
    return r;
}

static void ext_close(ag_device_t *dev, int fd)
{
    (void)dev;
    if (fd < 0 || fd >= EXT_MAX_CHAN) {
        return;
    }
    ag_rlink_hdr_t req, rep;
    ag_rlink_req(&req, RL_OP_CLOSE, 0);
    req.ch = fd;
    (void)rpc_simple(&req, &rep);
    TASK->mutex_lock(s_ext.lock, UINT32_MAX);
    s_ext.chan[fd].used = false;
    s_ext.chan[fd].eof = false;
    s_ext.chan[fd].r = s_ext.chan[fd].w = s_ext.chan[fd].fill = 0;
    TASK->mutex_unlock(s_ext.lock);
}

static ag_err_t ext_nonblock(ag_device_t *dev, int fd, bool on)
{
    (void)dev;
    if (fd < 0 || fd >= EXT_MAX_CHAN) {
        return -AG_EBADF;
    }
    s_ext.nb[fd] = on;
    return AG_OK;
}

static int ext_wait_readable(ag_device_t *dev, int fd, uint32_t timeout_ms)
{
    (void)dev;
    if (fd < 0 || fd >= EXT_MAX_CHAN) {
        return -AG_EBADF;
    }
    uint32_t waited = 0;
    for (;;) {
        TASK->mutex_lock(s_ext.lock, UINT32_MAX);
        pump(5);
        const bool avail = s_ext.chan[fd].fill > 0 || s_ext.chan[fd].eof;
        TASK->mutex_unlock(s_ext.lock);
        if (avail) {
            return 1;
        }
        waited += 5;
        if (timeout_ms != UINT32_MAX && waited >= timeout_ms) {
            return 0;
        }
    }
}

static const ag_dev_ops_t k_dev_ops = {0};

static const ag_net_ops_t k_net_ops = {
    .size = sizeof(ag_net_ops_t),
    .start = ext_start,
    .ready = ext_ready,
    .ifaddr = ext_ifaddr,
    .resolve = ext_resolve,
    .listen = ext_listen,
    .accept = ext_accept,
    .connect = ext_connect,
    .send = ext_send,
    .recv = ext_recv,
    .net_close = ext_close,
    .nonblock = ext_nonblock,
    .wait_readable = ext_wait_readable,
    .recv_now = ext_recv_now,
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

    memset(&s_ext, 0, sizeof(s_ext));
    s_ext.sm = SM_SYNC;

    /*
     * Which UART the coprocessor is on.  A real board wires it to a free UART -
     * UART2 by default - and can say so with radio.uart in BOARD/SYSTEM.CFG.
     * The QEMU machine (board "generic") emulates only UART0 (the console) and
     * UART1, so the fake coprocessor has to live on UART1 there; default to it
     * when that is the board, so the emulated path works with no config file.
     */
    int def_uart = EXT_UART_DEFAULT;
    {
        ag_sysinfo_t si;
        memset(&si, 0, sizeof(si));
        ag_sysinfo_get(&si);
        if (ag_strcmp(si.board, "generic") == 0) {
            def_uart = 1;
        }
    }
    s_ext.uart = def_uart;
    if (AG_HAS(ag_api()->cfg, get_int)) {
        s_ext.uart = ag_api()->cfg->get_int("radio.uart", def_uart);
    }
    s_ext.lock = TASK->mutex_create();
    if (s_ext.lock == NULL) {
        return -AG_ENOMEM;
    }

    /* Bring the UART up.  The pins come from BOARD.CFG (uart<n>.tx/rx); a board
     * with a radio wired to it sets them, and the generic QEMU profile carries
     * defaults so the fake coprocessor is reachable. */
    if (IO->uart_config != NULL) {
        const ag_err_t ue = IO->uart_config(s_ext.uart, EXT_BAUD, 8, 0, 1);
        if (ue != AG_OK) {
            ag_log(AG_LOG_WARN, "extradio",
                   "uart%d did not come up: %d (set uart%d.tx/rx in BOARD.CFG)",
                   s_ext.uart, (int)ue, s_ext.uart);
        }
    }

    /* Handshake: who is out there, and what can it do.  Not fatal if silent -
     * the wire may come up later, and the device should exist so that
     * `net use extradio` has something to bind. */
    {
        ag_rlink_hdr_t req, rep;
        ag_rlink_req(&req, RL_OP_HELLO, 0);
        req.a0 = RL_PROTO_VERSION;
        if (rpc_simple(&req, &rep) == AG_OK) {
            ag_log(AG_LOG_INFO, "extradio", "coprocessor proto %u caps 0x%x",
                   (unsigned)rep.a0, (unsigned)rep.a1);
        } else {
            ag_log(AG_LOG_WARN, "extradio",
                   "no coprocessor on uart%d yet (no HELLO reply)", s_ext.uart);
        }
    }

    const ag_dev_add_t desc = {
        .name = "extradio",
        .driver = "EXTRADIO",
        .cls = AG_DEV_NET,
        .flags = 0,
        .ops = &k_dev_ops,       /* registry requires one; empty = ENOTSUP     */
        .class_ops = &k_net_ops, /* the net class vtable the kernel reads      */
        .priv = &s_ext,
    };
    const ag_err_t err = ag_dev_add(&desc);
    if (err != AG_OK) {
        return err;
    }
    ag_log(AG_LOG_INFO, "extradio",
           "/dev/extradio on uart%d - bind with `net use extradio`",
           s_ext.uart);
    return AG_OK;
}
