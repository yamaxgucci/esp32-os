/*
 * ArgonOS - ATRADIO.SYS: an external Wi-Fi radio on its FACTORY AT firmware.
 *
 * The other half of "both variants" (see apps/extradio for RLINK, our own
 * coprocessor firmware).  This one talks to a stock ESP-01 exactly as it comes
 * out of the drawer - AT+CWJAP to join, AT+CIPSTART to open TCP, +IPD for
 * arriving bytes - so the first result on hardware needs no reflash, only a
 * wire.  It publishes the same ag_net_ops_t EXTRADIO does; `net use atradio`
 * binds it and the kernel cannot tell which radio answers.
 *
 * What AT buys and what it does not: sockets, yes; monitor mode, ESP-NOW and
 * injection, no.  Those are why our own RLINK firmware exists.
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include --include apps/common --include apps/common/atproto \
 *       -o build\apps\ATRADIO.SYS apps/atradio/atradio.c apps/common/atproto/ag_atproto.c
 *
 * NO BACKGROUND TASK: a .SYS ag_driver_init runs in kernel context with no
 * process, so api->task->create refuses it.  The UART is pumped inline, the
 * HostFS/EXTRADIO model - every op that must read drives the wire itself.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/argon.h>
#include <argon/libc.h>

#include "atproto/ag_atproto.h"

AG_DRV("ATRADIO", "0.1", "argon");

#define AT_UART_DEFAULT 2       /* free UART on a real board */
#define AT_BAUD         115200u /* factory ESP-01 default */
#define AT_RING         2048u   /* per-link receive ring */
#define AT_RX           640u    /* line/header accumulator (payload streams) */
#define AT_CMD_MS       8000u   /* a command that never terminates = dead link */

typedef struct {
    bool     used;
    bool     eof;
    uint32_t r, w, fill;
    uint8_t  buf[AT_RING];
} link_t;

typedef struct {
    int        uart;
    ag_mutex_t lock;

    /* receive accumulator + streaming +IPD state */
    uint8_t  rx[AT_RX];
    uint32_t rxlen;
    uint32_t ipd_remaining; /* payload bytes still to route to a ring */
    int      ipd_link;

    /* set by the line handler, read by at_wait */
    ag_at_kind_t term;   /* last terminal reply (OK/ERROR/SEND OK/...) */
    bool         prompt; /* the bare "> " send prompt was seen */
    uint32_t     last_ip;/* +CIPDOMAIN answer */

    bool     ready;
    uint32_t ifaddr;

    link_t link[AG_AT_MAX_LINK];
    bool   nb[AG_AT_MAX_LINK];
} at_t;

static at_t s_at;

static const ag_io_api_t   *IO;
static const ag_task_api_t *TASK;

/* ---------------------------------------------------------------------- */
/* UART + rings                                                            */
/* ---------------------------------------------------------------------- */

static void uart_send(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t         left = len;
    while (left > 0) {
        const int32_t n = IO->uart_write(s_at.uart, p, left);
        if (n <= 0) {
            return;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
}

static void ring_put(link_t *c, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (c->fill < AT_RING) {
            c->buf[c->w] = data[i];
            c->w = (c->w + 1) % AT_RING;
            c->fill++;
        }
    }
}

static void link_open(int i)
{
    link_t *c = &s_at.link[i];
    c->used = true;
    c->eof = false;
    c->r = c->w = c->fill = 0;
    s_at.nb[i] = false;
}

/* ---------------------------------------------------------------------- */
/* One reply line -> state                                                 */
/* ---------------------------------------------------------------------- */

static void handle_line(const char *line)
{
    ag_at_line_t l;
    ag_at_classify(line, &l);
    switch (l.kind) {
    case AG_AT_OK:
    case AG_AT_ERROR:
    case AG_AT_FAIL:
    case AG_AT_SEND_OK:
    case AG_AT_SEND_FAIL:
    case AG_AT_ALREADY:
        s_at.term = l.kind;
        break;
    case AG_AT_PROMPT:
        s_at.prompt = true;
        break;
    case AG_AT_CONNECT:
        if (l.link >= 0 && l.link < AG_AT_MAX_LINK) {
            link_open(l.link);
        }
        break;
    case AG_AT_CLOSED:
        if (l.link >= 0 && l.link < AG_AT_MAX_LINK) {
            s_at.link[l.link].eof = true;
        }
        break;
    case AG_AT_WIFI_GOTIP:
        s_at.ready = true;
        break;
    case AG_AT_WIFI_DOWN:
        s_at.ready = false;
        break;
    case AG_AT_CIPDOMAIN:
        s_at.last_ip = l.addr;
        break;
    case AG_AT_STA_IP:
        s_at.ifaddr = l.addr;
        s_at.ready = true;
        break;
    default:
        break;
    }
}

/* Classify every complete (\n-terminated) line in [p, p+len); return the bytes
 * consumed up to the last newline.  The trailing partial line is left. */
static uint32_t process_lines(char *p, uint32_t len)
{
    uint32_t start = 0, consumed = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (p[i] == '\n') {
            uint32_t end = i;
            if (end > start && p[end - 1] == '\r') {
                end--;
            }
            const char save = p[end];
            p[end] = '\0';
            handle_line(p + start);
            p[end] = save;
            start = i + 1;
            consumed = i + 1;
        }
    }
    return consumed;
}

static void drop_front(uint32_t n)
{
    if (n == 0) {
        return;
    }
    if (n >= s_at.rxlen) {
        s_at.rxlen = 0;
        return;
    }
    memmove(s_at.rx, s_at.rx + n, s_at.rxlen - n);
    s_at.rxlen -= n;
}

/* Drain the accumulator: stream any in-flight +IPD payload to its ring, pull
 * whole +IPD frames, classify the lines between them, and note the "> " prompt
 * that arrives without a newline.  Caller holds the lock. */
static void process_rx(void)
{
    for (;;) {
        if (s_at.ipd_remaining > 0) {
            uint32_t take = s_at.rxlen;
            if (take > s_at.ipd_remaining) {
                take = s_at.ipd_remaining;
            }
            if (s_at.ipd_link >= 0 && s_at.ipd_link < AG_AT_MAX_LINK) {
                ring_put(&s_at.link[s_at.ipd_link], s_at.rx, take);
            }
            drop_front(take);
            s_at.ipd_remaining -= take;
            if (s_at.ipd_remaining > 0) {
                return; /* need more bytes for this payload */
            }
            continue;
        }

        int      off, link, poff;
        uint32_t plen;
        const int r = ag_at_ipd_scan((char *)s_at.rx, s_at.rxlen, &off, &link,
                                     &plen, &poff);

        if (r == 1) {
            /* lines before the frame, then start streaming the payload */
            if (off > 0) {
                (void)process_lines((char *)s_at.rx, (uint32_t)off);
                drop_front((uint32_t)off);
            }
            /* header now at index 0; poff was relative to the old base, so the
             * header length is poff - off. */
            const uint32_t hdr = (uint32_t)(poff - off);
            drop_front(hdr);
            s_at.ipd_link = link;
            s_at.ipd_remaining = plen;
            continue; /* the streaming branch above handles the payload */
        }

        /* No complete +IPD.  Process the lines up to where a partial frame (or
         * a tag prefix) begins, or to the last newline if there is none. */
        uint32_t region = s_at.rxlen;
        if (off >= 0) {
            region = (uint32_t)off; /* keep the partial +IPD from here on */
        }
        const uint32_t took = process_lines((char *)s_at.rx, region);
        uint32_t consume = took;

        /* The send prompt "> " carries no newline, so process_lines never sees
         * it.  Look for it in the unconsumed head of the line region. */
        for (uint32_t i = took; i < region; i++) {
            if (s_at.rx[i] == '>') {
                s_at.prompt = true;
                consume = i + 1;
                break;
            }
        }
        drop_front(consume);

        /* Guard against a wedge: a full buffer with nothing consumable is line
         * noise; drop it and resync rather than spin. */
        if (consume == 0 && off < 0 && s_at.rxlen >= AT_RX) {
            s_at.rxlen = 0;
        }
        return;
    }
}

static void at_pump(uint32_t block_ms)
{
    if (s_at.rxlen < AT_RX) {
        const int32_t n = IO->uart_read(s_at.uart, s_at.rx + s_at.rxlen,
                                        AT_RX - s_at.rxlen, block_ms);
        if (n > 0) {
            s_at.rxlen += (uint32_t)n;
        }
    }
    process_rx();
}

/* ---------------------------------------------------------------------- */
/* Command / reply                                                         */
/* ---------------------------------------------------------------------- */

static void arm(void)
{
    s_at.term = AG_AT_OTHER;
    s_at.prompt = false;
}

/* Send a built command line and pump until a terminal reply (OK/ERROR/...) or
 * timeout.  Returns the terminal kind, or AG_AT_OTHER on timeout. */
static ag_at_kind_t at_run(const char *cmd, uint32_t timeout_ms)
{
    arm();
    uart_send(cmd, strlen(cmd));
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        at_pump(50);
        if (s_at.term != AG_AT_OTHER) {
            return s_at.term;
        }
        waited += 50;
    }
    return AG_AT_OTHER;
}

/* ---------------------------------------------------------------------- */
/* Rings -> caller                                                         */
/* ---------------------------------------------------------------------- */

static int32_t drain_locked(int fd, void *buf, size_t len)
{
    if (fd < 0 || fd >= AG_AT_MAX_LINK) {
        return -AG_EBADF;
    }
    uint8_t *o = (uint8_t *)buf;
    uint32_t n = 0;
    link_t  *c = &s_at.link[fd];
    while (n < len && c->fill > 0) {
        o[n++] = c->buf[c->r];
        c->r = (c->r + 1) % AT_RING;
        c->fill--;
    }
    if (n > 0) {
        return (int32_t)n;
    }
    return c->eof ? 0 : -AG_EAGAIN;
}

static int alloc_link(void)
{
    for (int i = 0; i < AG_AT_MAX_LINK; i++) {
        if (!s_at.link[i].used) {
            return i;
        }
    }
    return -AG_ENFILE;
}

/* ---------------------------------------------------------------------- */
/* The AG_DEV_NET class vtable                                             */
/* ---------------------------------------------------------------------- */

static ag_err_t at_start(ag_device_t *dev)
{
    (void)dev;
    char cmd[128];

    TASK->mutex_lock(s_at.lock, UINT32_MAX);

    ag_at_cmd(cmd, sizeof(cmd), "ATE0");
    (void)at_run(cmd, 2000);
    ag_at_cmd(cmd, sizeof(cmd), "AT+CWMODE=1");
    (void)at_run(cmd, 2000);
    ag_at_cmd(cmd, sizeof(cmd), "AT+CIPMUX=1");
    (void)at_run(cmd, 2000);

    /* Join, if this board was told a network.  Without radio.ssid we assume the
     * modem is already joined (a fixture, or provisioned elsewhere) and only
     * ask it for an address. */
    char ssid[48] = {0};
    char pass[64] = {0};
    if (AG_HAS(ag_api()->cfg, get_str)) {
        (void)ag_api()->cfg->get_str("radio.ssid", ssid, sizeof(ssid));
        (void)ag_api()->cfg->get_str("radio.pass", pass, sizeof(pass));
    }
    if (ssid[0] != '\0') {
        ag_at_cmd_join(cmd, sizeof(cmd), ssid, pass);
        const ag_at_kind_t k = at_run(cmd, 20000);
        if (k != AG_AT_OK) {
            ag_log(AG_LOG_WARN, "atradio", "join failed (%d)", (int)k);
        }
    }

    /* Ask for the address; WIFI GOT IP / +CIFSR set s_at.ready/ifaddr. */
    ag_at_cmd(cmd, sizeof(cmd), "AT+CIFSR");
    (void)at_run(cmd, 3000);

    TASK->mutex_unlock(s_at.lock);
    /* The interface is up; an address may only arrive later (ready() reports
     * it).  netprov polls ready after binding, so start never needs to wait. */
    return AG_OK;
}

static bool at_ready(ag_device_t *dev)
{
    (void)dev;
    TASK->mutex_lock(s_at.lock, UINT32_MAX);
    at_pump(0);
    const bool r = s_at.ready;
    TASK->mutex_unlock(s_at.lock);
    return r;
}

static ag_err_t at_ifaddr(ag_device_t *dev, uint32_t *addr)
{
    (void)dev;
    if (addr == NULL) {
        return -AG_EINVAL;
    }
    TASK->mutex_lock(s_at.lock, UINT32_MAX);
    at_pump(0);
    const bool     ready = s_at.ready;
    const uint32_t a = s_at.ifaddr;
    TASK->mutex_unlock(s_at.lock);
    if (!ready) {
        return -AG_EAGAIN;
    }
    *addr = a;
    return AG_OK;
}

static ag_err_t at_resolve(ag_device_t *dev, const char *host, uint32_t *addr)
{
    (void)dev;
    if (host == NULL || addr == NULL || host[0] == '\0') {
        return -AG_EINVAL;
    }
    char cmd[160];
    if (ag_at_cmd_resolve(cmd, sizeof(cmd), host) < 0) {
        return -AG_EINVAL;
    }
    TASK->mutex_lock(s_at.lock, UINT32_MAX);
    s_at.last_ip = 0;
    const ag_at_kind_t k = at_run(cmd, 10000);
    const uint32_t     ip = s_at.last_ip;
    TASK->mutex_unlock(s_at.lock);
    if (k != AG_AT_OK) {
        return -AG_ENOENT;
    }
    if (ip == 0) {
        return -AG_ENOENT;
    }
    *addr = ip;
    return AG_OK;
}

static int at_connect(ag_device_t *dev, uint32_t addr, uint16_t port,
                      uint32_t timeout_ms)
{
    (void)dev;
    (void)timeout_ms;
    TASK->mutex_lock(s_at.lock, UINT32_MAX);
    const int link = alloc_link();
    if (link < 0) {
        TASK->mutex_unlock(s_at.lock);
        return link;
    }
    char cmd[80];
    ag_at_cmd_connect(cmd, sizeof(cmd), link, addr, port);
    const ag_at_kind_t k = at_run(cmd, 10000);
    /* CONNECT (handled in handle_line -> link_open) then OK; some firmwares
     * only print OK. */
    const bool ok = (k == AG_AT_OK);
    if (ok && !s_at.link[link].used) {
        link_open(link); /* OK without an explicit n,CONNECT line */
    }
    const bool used = s_at.link[link].used;
    TASK->mutex_unlock(s_at.lock);
    if (!ok || !used) {
        return -AG_EIO;
    }
    return link;
}

static int32_t at_send(ag_device_t *dev, int fd, const void *buf, size_t len)
{
    (void)dev;
    if (fd < 0 || fd >= AG_AT_MAX_LINK) {
        return -AG_EBADF;
    }
    if (len == 0) {
        return 0;
    }
    /* AT+CIPSEND takes up to 2048 bytes at a time. */
    uint32_t chunk = (len > 2048u) ? 2048u : (uint32_t)len;

    TASK->mutex_lock(s_at.lock, UINT32_MAX);
    if (!s_at.link[fd].used) {
        TASK->mutex_unlock(s_at.lock);
        return -AG_EBADF;
    }
    char cmd[40];
    ag_at_cmd_send(cmd, sizeof(cmd), fd, chunk);

    /* Phase 1: the command, then wait for the "> " prompt. */
    arm();
    uart_send(cmd, strlen(cmd));
    uint32_t waited = 0;
    bool     prompted = false;
    while (waited < 3000u) {
        at_pump(50);
        if (s_at.prompt) {
            prompted = true;
            break;
        }
        if (s_at.term == AG_AT_ERROR) {
            break;
        }
        waited += 50;
    }
    if (!prompted) {
        TASK->mutex_unlock(s_at.lock);
        return -AG_EIO;
    }

    /* Phase 2: the payload, then wait for SEND OK. */
    arm();
    uart_send(buf, chunk);
    waited = 0;
    ag_at_kind_t k = AG_AT_OTHER;
    while (waited < AT_CMD_MS) {
        at_pump(50);
        if (s_at.term == AG_AT_SEND_OK || s_at.term == AG_AT_SEND_FAIL ||
            s_at.term == AG_AT_ERROR) {
            k = s_at.term;
            break;
        }
        waited += 50;
    }
    TASK->mutex_unlock(s_at.lock);
    if (k != AG_AT_SEND_OK) {
        return -AG_EIO;
    }
    return (int32_t)chunk;
}

static int32_t at_recv(ag_device_t *dev, int fd, void *buf, size_t len)
{
    (void)dev;
    for (;;) {
        TASK->mutex_lock(s_at.lock, UINT32_MAX);
        at_pump(5);
        const int32_t r = drain_locked(fd, buf, len);
        const bool nb = (fd >= 0 && fd < AG_AT_MAX_LINK) ? s_at.nb[fd] : true;
        TASK->mutex_unlock(s_at.lock);
        if (r != -AG_EAGAIN) {
            return r;
        }
        if (nb) {
            return -AG_EAGAIN;
        }
        TASK->sleep_ms(1);
    }
}

static int32_t at_recv_now(ag_device_t *dev, int fd, void *buf, size_t len)
{
    (void)dev;
    TASK->mutex_lock(s_at.lock, UINT32_MAX);
    at_pump(0);
    const int32_t r = drain_locked(fd, buf, len);
    TASK->mutex_unlock(s_at.lock);
    return r;
}

static void at_close(ag_device_t *dev, int fd)
{
    (void)dev;
    if (fd < 0 || fd >= AG_AT_MAX_LINK) {
        return;
    }
    char cmd[32];
    ag_at_cmd_close(cmd, sizeof(cmd), fd);
    TASK->mutex_lock(s_at.lock, UINT32_MAX);
    (void)at_run(cmd, 3000);
    s_at.link[fd].used = false;
    s_at.link[fd].eof = false;
    s_at.link[fd].r = s_at.link[fd].w = s_at.link[fd].fill = 0;
    TASK->mutex_unlock(s_at.lock);
}

static ag_err_t at_nonblock(ag_device_t *dev, int fd, bool on)
{
    (void)dev;
    if (fd < 0 || fd >= AG_AT_MAX_LINK) {
        return -AG_EBADF;
    }
    s_at.nb[fd] = on;
    return AG_OK;
}

static int at_wait_readable(ag_device_t *dev, int fd, uint32_t timeout_ms)
{
    (void)dev;
    if (fd < 0 || fd >= AG_AT_MAX_LINK) {
        return -AG_EBADF;
    }
    uint32_t waited = 0;
    for (;;) {
        TASK->mutex_lock(s_at.lock, UINT32_MAX);
        at_pump(5);
        const bool avail = s_at.link[fd].fill > 0 || s_at.link[fd].eof;
        TASK->mutex_unlock(s_at.lock);
        if (avail) {
            return 1;
        }
        waited += 5;
        if (timeout_ms != UINT32_MAX && waited >= timeout_ms) {
            return 0;
        }
    }
}

/* AT's server model (AT+CIPSERVER) is a different shape from listen/accept and
 * is not needed for the client sockets wget/ftp/ssh use; a Hosted/RLINK radio
 * serves.  Refuse cleanly rather than pretend. */
static int at_listen(ag_device_t *dev, uint16_t port)
{
    (void)dev;
    (void)port;
    return -AG_ENOTSUP;
}

static int at_accept(ag_device_t *dev, int lfd, uint32_t timeout_ms)
{
    (void)dev;
    (void)lfd;
    (void)timeout_ms;
    return -AG_ENOTSUP;
}

static const ag_dev_ops_t k_dev_ops = {0};

static const ag_net_ops_t k_net_ops = {
    .size = sizeof(ag_net_ops_t),
    .start = at_start,
    .ready = at_ready,
    .ifaddr = at_ifaddr,
    .resolve = at_resolve,
    .listen = at_listen,
    .accept = at_accept,
    .connect = at_connect,
    .send = at_send,
    .recv = at_recv,
    .net_close = at_close,
    .nonblock = at_nonblock,
    .wait_readable = at_wait_readable,
    .recv_now = at_recv_now,
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

    memset(&s_at, 0, sizeof(s_at));
    s_at.ipd_link = -1;

    int def_uart = AT_UART_DEFAULT;
    {
        ag_sysinfo_t si;
        memset(&si, 0, sizeof(si));
        ag_sysinfo_get(&si);
        if (ag_strcmp(si.board, "generic") == 0) {
            def_uart = 1; /* QEMU emulates only UART0/UART1 */
        }
    }
    s_at.uart = def_uart;
    if (AG_HAS(ag_api()->cfg, get_int)) {
        s_at.uart = ag_api()->cfg->get_int("radio.uart", def_uart);
    }

    s_at.lock = TASK->mutex_create();
    if (s_at.lock == NULL) {
        return -AG_ENOMEM;
    }

    if (IO->uart_config != NULL) {
        const ag_err_t ue = IO->uart_config(s_at.uart, AT_BAUD, 8, 0, 1);
        if (ue != AG_OK) {
            ag_log(AG_LOG_WARN, "atradio",
                   "uart%d did not come up: %d (set uart%d.tx/rx in BOARD.CFG)",
                   s_at.uart, (int)ue, s_at.uart);
        }
    }

    /* Probe: does anything answer AT?  Not fatal - the device should exist so
     * `net use atradio` can bind even if the modem is plugged in later. */
    {
        char cmd[16];
        ag_at_cmd(cmd, sizeof(cmd), "AT");
        TASK->mutex_lock(s_at.lock, UINT32_MAX);
        const ag_at_kind_t k = at_run(cmd, 1000);
        TASK->mutex_unlock(s_at.lock);
        if (k == AG_AT_OK) {
            ag_log(AG_LOG_INFO, "atradio", "modem answers AT on uart%d",
                   s_at.uart);
        } else {
            ag_log(AG_LOG_WARN, "atradio", "no AT modem on uart%d yet",
                   s_at.uart);
        }
    }

    const ag_dev_add_t desc = {
        .name = "atradio",
        .driver = "ATRADIO",
        .cls = AG_DEV_NET,
        .flags = 0,
        .ops = &k_dev_ops,
        .class_ops = &k_net_ops,
        .priv = &s_at,
    };
    const ag_err_t err = ag_dev_add(&desc);
    if (err != AG_OK) {
        return err;
    }
    ag_log(AG_LOG_INFO, "atradio",
           "/dev/atradio on uart%d - bind with `net use atradio`", s_at.uart);
    return AG_OK;
}
