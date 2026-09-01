/*
 * ArgonOS - telnet console endpoint.
 *
 * A TCP listener on port 23.  Each accepted connection becomes a console
 * endpoint (ag_console_attach), so everything the console already does - the
 * shell, the editor, the file manager, the VT100 screen - appears over the
 * network with not one line of change above here.  This file is only the
 * transport: turn a socket into the console's read/write/close hooks, and strip
 * the telnet IAC negotiation so a clean VT stream reaches the input decoder.
 *
 * The console output is UTF-8 plus ASCII escapes, and 0xFF is never a byte of
 * either, so nothing on the way out needs telnet's FF->FF FF escaping; only the
 * way in is filtered.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "console/telnet_console.h"

#if defined(CONFIG_ARGON_NET_TELNET) && CONFIG_ARGON_NET_TELNET

#include <string.h>

#include <argon/console.h>
#include <argon/log.h>

#include <argon/port/net.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

#define TELNET_MAX_CONN 2      /* console has 4 endpoint slots; UART keeps one */
#define TELNET_DEFAULT_PORT 23
#define TELNET_ACCEPT_MS 400   /* how often the listener wakes to check `stop` */

/* Telnet commands (RFC 854). */
#define TN_IAC  255
#define TN_WILL 251 /* ..254 are WILL/WONT/DO/DONT, each with one option byte */
#define TN_DONT 254
#define TN_ECHO 1
#define TN_SGA  3   /* suppress go-ahead: character-at-a-time mode */

/* IAC filter state, per connection. */
enum { IAC_NORMAL = 0, IAC_CMD, IAC_OPT };

typedef struct {
    volatile bool used;
    int           fd;
    uint8_t       iac; /* IAC filter state */
} telnet_conn_t;

static telnet_conn_t   s_conns[TELNET_MAX_CONN];
static volatile bool   s_running;
static volatile bool   s_stop;
static int             s_listen_fd = -1;
static uint16_t        s_port;
static ag_port_task_t  s_task;

/* ---- the console transport -------------------------------------------- */

static int32_t telnet_write(void *ctx, const char *data, size_t len)
{
    telnet_conn_t *c = (telnet_conn_t *)ctx;
    if (!c->used || c->fd < 0) {
        return 0;
    }
    const int32_t n = ag_port_net_send(c->fd, data, len);
    /* A send error does not detach here - the read side sees the same broken
     * socket and returns negative, which is the one place that detaches. */
    return (n < 0) ? 0 : n;
}

/*
 * Read what is waiting, strip telnet negotiation, hand up a clean VT stream.
 * Returns negative when the peer has gone, so the console detaches us.
 */
static int32_t telnet_read(void *ctx, uint8_t *buf, size_t len)
{
    telnet_conn_t *c = (telnet_conn_t *)ctx;
    if (!c->used || c->fd < 0) {
        return -1;
    }

    uint8_t raw[128];
    size_t  want = (len < sizeof(raw)) ? len : sizeof(raw);
    if (want == 0) {
        return 0;
    }

    const int32_t n = ag_port_net_recv_now(c->fd, raw, want);
    if (n == 0) {
        return -1; /* orderly close: peer hung up */
    }
    if (n == -AG_EAGAIN) {
        return 0; /* nothing right now, connection still alive */
    }
    if (n < 0) {
        return -1; /* any other error: treat as gone */
    }

    size_t out = 0;
    for (int32_t i = 0; i < n; i++) {
        const uint8_t b = raw[i];
        switch (c->iac) {
        case IAC_NORMAL:
            if (b == TN_IAC) {
                c->iac = IAC_CMD;
            } else {
                buf[out++] = b;
            }
            break;
        case IAC_CMD:
            if (b == TN_IAC) {
                buf[out++] = TN_IAC; /* escaped literal 0xFF */
                c->iac = IAC_NORMAL;
            } else if (b >= TN_WILL && b <= TN_DONT) {
                c->iac = IAC_OPT; /* WILL/WONT/DO/DONT: one option byte follows */
            } else {
                c->iac = IAC_NORMAL; /* other two-byte command: consumed */
            }
            break;
        case IAC_OPT:
        default:
            c->iac = IAC_NORMAL; /* consume the option byte */
            break;
        }
    }
    return (int32_t)out; /* may be 0 if the whole read was negotiation */
}

static void telnet_close(void *ctx)
{
    telnet_conn_t *c = (telnet_conn_t *)ctx;
    if (c->fd >= 0) {
        (void)ag_port_net_close(c->fd);
        c->fd = -1;
    }
    c->used = false;
}

static const ag_con_transport_t k_telnet_transport = {
    .name = "telnet",
    .write = telnet_write,
    .read = telnet_read,
    .close = telnet_close,
};

/* ---- the listener ------------------------------------------------------ */

static telnet_conn_t *conn_alloc(void)
{
    for (int i = 0; i < TELNET_MAX_CONN; i++) {
        if (!s_conns[i].used) {
            s_conns[i].used = true;
            s_conns[i].fd = -1;
            s_conns[i].iac = IAC_NORMAL;
            return &s_conns[i];
        }
    }
    return NULL;
}

static void accept_one(void)
{
    const int fd = ag_port_net_accept(s_listen_fd, TELNET_ACCEPT_MS);
    if (fd < 0) {
        return; /* timeout (nothing waiting) or a transient error */
    }

    telnet_conn_t *c = conn_alloc();
    if (c == NULL) {
        (void)ag_port_net_close(fd); /* too many sessions already */
        return;
    }
    c->fd = fd;
    (void)ag_port_net_nonblock(fd, true);

    /* Offer to echo and to run without go-ahead, so a line-mode client switches
     * to character-at-a-time - which is what the console's input decoder wants. */
    static const uint8_t hello[] = {
        TN_IAC, TN_WILL, TN_ECHO, TN_IAC, TN_WILL, TN_SGA,
    };
    (void)ag_port_net_send(fd, hello, sizeof(hello));

    if (ag_console_attach(&k_telnet_transport, c) != AG_OK) {
        (void)ag_port_net_close(fd);
        c->fd = -1;
        c->used = false;
    }
}

static void telnet_task(void *arg)
{
    (void)arg;
    while (!s_stop) {
        accept_one();
    }

    /* Tearing down: drop the sessions, then the listener. */
    for (int i = 0; i < TELNET_MAX_CONN; i++) {
        if (s_conns[i].used) {
            ag_console_detach(&s_conns[i]); /* calls telnet_close */
        }
    }
    if (s_listen_fd >= 0) {
        (void)ag_port_net_close(s_listen_fd);
        s_listen_fd = -1;
    }
    s_running = false;
    ag_port_task_delete(NULL);
}

ag_err_t ag_telnet_start(uint16_t port)
{
    if (s_running) {
        return -AG_EBUSY;
    }
    if (port == 0) {
        port = TELNET_DEFAULT_PORT;
    }

    const int lfd = ag_port_net_listen(port);
    if (lfd < 0) {
        return (lfd < 0) ? (ag_err_t)lfd : -AG_EIO;
    }
    s_listen_fd = lfd;
    s_port = port;
    s_stop = false;
    s_running = true;

    if (!ag_port_task_create(telnet_task, "ag_telnet", 4096, NULL, 6, 0, 0,
                             &s_task)) {
        (void)ag_port_net_close(s_listen_fd);
        s_listen_fd = -1;
        s_running = false;
        return -AG_ENOMEM;
    }
    ag_log(AG_LOG_INFO, "telnet", "listening on port %u", (unsigned)port);
    return AG_OK;
}

void ag_telnet_stop(void)
{
    if (!s_running) {
        return;
    }
    s_stop = true;
    /* The task wakes within one accept window, cleans up and deletes itself. */
    for (int i = 0; i < 50 && s_running; i++) {
        ag_port_task_delay(ag_port_ms_to_ticks(20));
    }
}

bool ag_telnet_running(void) { return s_running; }

uint16_t ag_telnet_port(void) { return s_running ? s_port : 0; }

#endif /* CONFIG_ARGON_NET_TELNET */
