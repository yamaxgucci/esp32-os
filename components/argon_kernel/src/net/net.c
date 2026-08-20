/*
 * ArgonOS - the api->net table: who owns a connection, and what it is called.
 *
 * The sockets themselves are the port's (argon/port/net.h).  What is here is
 * the part that is the same wherever the system runs: a bounded table of
 * connections with handles of our own numbering, so that a handle the ABI gave
 * out cannot be confused with a file, and so that there is one place that knows
 * how many connections exist.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/config.h>

#if CONFIG_ARGON_ENABLE_NET

#include <argon/log.h>
#include <argon/net.h>
#include <argon/netmsg.h>

#include <argon/port/net.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>

/* 3 virt drivers × (listen+conn) plus brief replace peaks and reload slack. */
#define AG_NET_MAX_SOCK 16
#define AG_NET_HANDLE_BASE 0x71000000

static int             s_fds[AG_NET_MAX_SOCK];
static bool            s_in_use[AG_NET_MAX_SOCK];
static ag_port_mutex_t s_lock;

static void lock(void)
{
    if (s_lock != NULL) {
        ag_port_mutex_take(s_lock, AG_PORT_FOREVER);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        ag_port_mutex_give(s_lock);
    }
}

static int slot_of(ag_handle_t h)
{
    if (h < AG_NET_HANDLE_BASE ||
        h >= AG_NET_HANDLE_BASE + AG_NET_MAX_SOCK) {
        return -1;
    }
    return (int)(h - AG_NET_HANDLE_BASE);
}

/*
 * A descriptor from the port becomes a handle of ours, or is closed again.  A
 * socket that could not be written down is a socket nobody owns, and one of
 * those outlives the process that made it.
 */
static ag_handle_t adopt_fd(int fd)
{
    if (fd < 0) {
        return (ag_handle_t)fd; /* already an -AG_E* code */
    }
    lock();
    for (int i = 0; i < AG_NET_MAX_SOCK; i++) {
        if (!s_in_use[i]) {
            s_in_use[i] = true;
            s_fds[i] = fd;
            unlock();
            return (ag_handle_t)(AG_NET_HANDLE_BASE + i);
        }
    }
    {
        int used = 0;
        for (int i = 0; i < AG_NET_MAX_SOCK; i++) {
            if (s_in_use[i]) {
                used++;
            }
        }
        unlock();
        ag_log(AG_LOG_ERROR, "net", "adopt_fd ENFILE used=%d/%d", used,
               AG_NET_MAX_SOCK);
    }
    ag_port_net_close(fd);
    return (ag_handle_t)(-AG_ENFILE);
}

static int fd_of(ag_handle_t h)
{
    const int slot = slot_of(h);
    if (slot < 0 || !s_in_use[slot]) {
        return -1;
    }
    return s_fds[slot];
}

/*
 * The address arrives long after boot has moved on, and nothing polls for it.
 * Printed here because nobody can use the network without knowing what the
 * board is called.
 */
static void on_ready(uint32_t addr, uint32_t mask, uint32_t gw)
{
    ag_log(AG_LOG_INFO, "net", "ip %u.%u.%u.%u mask %u.%u.%u.%u gw %u.%u.%u.%u",
           (unsigned)(addr >> 24), (unsigned)((addr >> 16) & 0xffu),
           (unsigned)((addr >> 8) & 0xffu), (unsigned)(addr & 0xffu),
           (unsigned)(mask >> 24), (unsigned)((mask >> 16) & 0xffu),
           (unsigned)((mask >> 8) & 0xffu), (unsigned)(mask & 0xffu),
           (unsigned)(gw >> 24), (unsigned)((gw >> 16) & 0xffu),
           (unsigned)((gw >> 8) & 0xffu), (unsigned)(gw & 0xffu));
}

ag_err_t ag_net_init(void)
{
    /*
     * Callable more than once, because on a board with a radio the network is
     * not only started at boot: it is turned on and off from the shell, since
     * a radio that is running costs tens of kilobytes and a radio that is
     * merely linked costs none.  The socket table is only rebuilt the first
     * time; doing it again would forget sockets somebody still holds.
     */
    if (s_lock == NULL) {
        s_lock = ag_port_mutex_new();
        if (s_lock == NULL) {
            return -AG_ENOMEM;
        }
        for (int i = 0; i < AG_NET_MAX_SOCK; i++) {
            s_fds[i] = -1;
            s_in_use[i] = false;
        }
    }

    ag_port_net_on_ready(on_ready);

    const ag_err_t err = ag_port_net_start();
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, "net", "the interface did not start: %d",
               (int)err);
        return err;
    }

    ag_log(AG_LOG_INFO, "net", "interface up (waiting for an address)");
    return AG_OK;
}

bool ag_net_ready(void) { return ag_port_net_ready(); }

static bool api_ready(void) { return ag_port_net_ready(); }

static ag_err_t api_ifaddr(uint32_t *addr_out)
{
    return ag_port_net_ifaddr(addr_out);
}

static ag_err_t api_wait_ready(uint32_t timeout_ms)
{
    const ag_port_ticks_t start = ag_port_ticks();
    const ag_port_ticks_t budget =
        (timeout_ms == UINT32_MAX) ? AG_PORT_FOREVER
                                   : ag_port_ms_to_ticks(timeout_ms);
    while (!ag_port_net_ready()) {
        if (budget != AG_PORT_FOREVER &&
            (ag_port_ticks() - start) >= budget) {
            return -AG_ETIMEDOUT;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(20));
    }
    return AG_OK;
}

/*
 * A dotted quad is read here and a name is passed down.
 *
 * Not a shortcut: a board on a network with no name service, or none yet, can
 * still be told exactly where to connect, and the answer does not depend on a
 * lease.  It also means every caller in the system - the fetch, the file
 * transfer, an application - takes "the host" in one form and never has to ask
 * which kind it has.
 */
ag_err_t ag_net_lookup(const char *host, uint32_t *addr_out)
{
    if (host == NULL || addr_out == NULL) {
        return -AG_EINVAL;
    }
    if (ag_ipv4_parse(host, addr_out)) {
        return AG_OK;
    }
    return ag_port_net_resolve(host, addr_out);
}

static ag_err_t api_resolve(const char *host, uint32_t *addr_out)
{
    return ag_net_lookup(host, addr_out);
}

static ag_handle_t api_tcp_listen(uint16_t port)
{
    return adopt_fd(ag_port_net_listen(port));
}

static ag_handle_t api_tcp_accept(ag_handle_t listen_h, uint32_t timeout_ms)
{
    const int lfd = fd_of(listen_h);
    if (lfd < 0) {
        return (ag_handle_t)(-AG_EBADF);
    }
    return adopt_fd(ag_port_net_accept(lfd, timeout_ms));
}

static ag_handle_t api_tcp_connect(uint32_t addr, uint16_t port,
                                   uint32_t timeout_ms)
{
    return adopt_fd(ag_port_net_connect(addr, port, timeout_ms));
}

static int32_t api_send(ag_handle_t h, const void *buf, size_t len)
{
    const int fd = fd_of(h);
    if (fd < 0) {
        return -AG_EBADF;
    }
    if (buf == NULL && len > 0) {
        return -AG_EINVAL;
    }
    return ag_port_net_send(fd, buf, len);
}

static int32_t api_recv(ag_handle_t h, void *buf, size_t len)
{
    const int fd = fd_of(h);
    if (fd < 0) {
        return -AG_EBADF;
    }
    if (buf == NULL && len > 0) {
        return -AG_EINVAL;
    }
    return ag_port_net_recv(fd, buf, len);
}

static ag_err_t api_close(ag_handle_t h)
{
    const int slot = slot_of(h);
    if (slot < 0) {
        return -AG_EBADF;
    }
    lock();
    if (!s_in_use[slot]) {
        unlock();
        return -AG_EBADF;
    }
    const int fd = s_fds[slot];
    s_in_use[slot] = false;
    s_fds[slot] = -1;
    unlock();
    ag_port_net_close(fd);
    return AG_OK;
}

static ag_err_t api_set_nonblock(ag_handle_t h, bool on)
{
    const int fd = fd_of(h);
    if (fd < 0) {
        return -AG_EBADF;
    }
    return ag_port_net_nonblock(fd, on);
}

const ag_net_api_t ag_net_api_impl = {
    .size = sizeof(ag_net_api_t),
    .ready = api_ready,
    .wait_ready = api_wait_ready,
    .ifaddr = api_ifaddr,
    .tcp_listen = api_tcp_listen,
    .tcp_accept = api_tcp_accept,
    .tcp_connect = api_tcp_connect,
    .send = api_send,
    .recv = api_recv,
    .close = api_close,
    .set_nonblock = api_set_nonblock,
    .resolve = api_resolve,
};

const ag_net_api_t *ag_net_api_table(void) { return &ag_net_api_impl; }

#endif /* CONFIG_ARGON_ENABLE_NET */
