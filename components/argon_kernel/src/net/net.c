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

#include "net/netsvc.h"

#include <argon/log.h>
#include <argon/net.h>
#include <argon/proc.h>
#include <argon/netprov.h>
#include <argon/power.h>
#include <argon/netmsg.h>

#include <argon/port/net.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/tls.h>
#include <argon/port/sntp.h>

/* 3 virt drivers × (listen+conn) plus brief replace peaks and reload slack. */
#define AG_NET_MAX_SOCK 16
#define AG_NET_HANDLE_BASE 0x71000000

static int             s_fds[AG_NET_MAX_SOCK];
static bool            s_in_use[AG_NET_MAX_SOCK];
/*
 * A slot is either a plain socket (s_fds >= 0, s_tls NULL) or a TLS connection
 * (s_fds == -1, s_tls set).  The handle the ABI hands out is the same either
 * way, so send/recv/close branch here on which one a slot holds - an
 * application that dialled with tls_connect reads and writes it exactly as a
 * plain socket, which is the whole point.
 */
static void           *s_tls[AG_NET_MAX_SOCK];
/*
 * Whose socket it is.  AG_PID_KERNEL for one opened by a driver's own task or
 * by a kernel service, and those are nobody's to reclaim: the phone link's
 * listener is opened from the driver's task and must outlive every process
 * that ever connects to it.
 */
static ag_pid_t        s_owner[AG_NET_MAX_SOCK];
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
            s_owner[i] = ag_proc_self();
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
    ag_netprov_close(fd);
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

#if AG_PORT_HAS_TLS
/*
 * A TLS connection the port handed us becomes a handle, the same table and the
 * same numbering as a plain socket.  No slot: the connection is closed and the
 * caller gets ENFILE, just as for a plain fd that could not be written down.
 */
static ag_handle_t adopt_tls(ag_port_tls_t t)
{
    if (t == NULL) {
        return (ag_handle_t)(-AG_EIO);
    }
    lock();
    for (int i = 0; i < AG_NET_MAX_SOCK; i++) {
        if (!s_in_use[i]) {
            s_in_use[i] = true;
            s_fds[i] = -1;
            s_tls[i] = (void *)t;
            s_owner[i] = ag_proc_self();
            unlock();
            return (ag_handle_t)(AG_NET_HANDLE_BASE + i);
        }
    }
    unlock();
    ag_log(AG_LOG_ERROR, "net", "adopt_tls ENFILE");
    ag_port_tls_close(t);
    return (ag_handle_t)(-AG_ENFILE);
}
#endif

/* The TLS connection a slot holds, or NULL for a plain socket / bad handle. */
static void *tls_of(ag_handle_t h)
{
    const int slot = slot_of(h);
    if (slot < 0 || !s_in_use[slot]) {
        return NULL;
    }
    return s_tls[slot];
}

#if AG_PORT_HAS_SNTP
/*
 * The clock starts at the epoch and nothing moves it, so the first time anything
 * speaks TLS the certificate is "not yet valid" and the handshake fails - which
 * is a trap for an application dialling out over TLS, since it never ran `date
 * sync` by hand.  So the system syncs the clock itself, once, the moment it has
 * an address.  One shot in its own task (the sync blocks for up to ten seconds),
 * best-effort: a board with no reachable time server simply keeps the epoch, as
 * before.
 */
static bool s_time_sync_started;

static void sntp_once(void *arg)
{
    (void)arg;
    const ag_err_t e = ag_port_sntp_sync(NULL, 10000);
    if (e == AG_OK) {
        ag_log(AG_LOG_INFO, "net", "clock synced from the network (sntp)");
    } else {
        ag_log(AG_LOG_WARN, "net",
               "sntp sync failed (%d); TLS will fail until the clock is set",
               (int)e);
    }
    ag_port_task_delete(NULL);
}
#endif

/*
 * The address arrives long after boot has moved on, and nothing polls for it.
 * Printed here because nobody can use the network without knowing what the
 * board is called.
 */
static void on_ready(uint32_t addr, uint32_t mask, uint32_t gw)
{
#if AG_PORT_HAS_SNTP
    /* First address of this boot: set the wall clock, so TLS can verify a
     * certificate's dates.  Once is enough; a reconnect does not redo it. */
    if (!s_time_sync_started) {
        s_time_sync_started = true;
        (void)ag_port_task_create(sntp_once, "ag_sntp", 4096, NULL, 5, 0, 0,
                                  NULL);
    }
#endif
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
    /* Before anything else: a radio on a bus somebody slowed down receives
     * nothing, and looks healthy doing it. */
    ag_powerctl_bus_needed();

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
            s_tls[i] = NULL;
        }
    }

    ag_port_net_on_ready(on_ready);

    const ag_err_t err = ag_netprov_start();
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, "net", "the interface did not start: %d",
               (int)err);
        return err;
    }

    ag_log(AG_LOG_INFO, "net", "interface up (waiting for an address)");
    return AG_OK;
}

/*
 * Drop every socket the handle table holds, through whoever owns them right
 * now.  Called by the provider layer just before it swaps the active provider,
 * while that provider is still the one that can close its own sockets.
 */
void ag_net_reset_sockets(void)
{
    lock();
    for (int i = 0; i < AG_NET_MAX_SOCK; i++) {
        if (s_in_use[i]) {
            const int fd = s_fds[i];
            void     *tls = s_tls[i];
            s_in_use[i] = false;
            s_fds[i] = -1;
            s_tls[i] = NULL;
#if AG_PORT_HAS_TLS
            if (tls != NULL) {
                ag_port_tls_close((ag_port_tls_t)tls);
                continue;
            }
#else
            (void)tls;
#endif
            ag_netprov_close(fd);
        }
    }
    unlock();
}

bool ag_net_ready(void) { return ag_netprov_ready(); }

static bool api_ready(void) { return ag_netprov_ready(); }

static ag_err_t api_ifaddr(uint32_t *addr_out)
{
    return ag_netprov_ifaddr(addr_out);
}

static ag_err_t api_wait_ready(uint32_t timeout_ms)
{
    const ag_port_ticks_t start = ag_port_ticks();
    const ag_port_ticks_t budget =
        (timeout_ms == UINT32_MAX) ? AG_PORT_FOREVER
                                   : ag_port_ms_to_ticks(timeout_ms);
    while (!ag_netprov_ready()) {
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
    return ag_netprov_resolve(host, addr_out);
}

static ag_err_t api_resolve(const char *host, uint32_t *addr_out)
{
    return ag_net_lookup(host, addr_out);
}

static ag_handle_t api_tcp_listen(uint16_t port)
{
    return adopt_fd(ag_netprov_listen(port));
}

static ag_handle_t api_tcp_accept(ag_handle_t listen_h, uint32_t timeout_ms)
{
    const int lfd = fd_of(listen_h);
    if (lfd < 0) {
        return (ag_handle_t)(-AG_EBADF);
    }
    return adopt_fd(ag_netprov_accept(lfd, timeout_ms));
}

static ag_handle_t api_tcp_connect(uint32_t addr, uint16_t port,
                                   uint32_t timeout_ms)
{
    return adopt_fd(ag_netprov_connect(addr, port, timeout_ms));
}

/*
 * A TLS connection to host:port, verified against the root bundle, handed back
 * as an ordinary handle.  By host name, not an address: the certificate is
 * checked against the name, so a name is what the caller must give.  -AG_ENOSYS
 * in a build without TLS.
 */
static ag_handle_t api_tls_connect(const char *host, uint16_t port,
                                   uint32_t timeout_ms)
{
    if (host == NULL) {
        return (ag_handle_t)(-AG_EINVAL);
    }
#if AG_PORT_HAS_TLS
    return adopt_tls(ag_port_tls_connect(host, port, timeout_ms));
#else
    (void)port;
    (void)timeout_ms;
    return (ag_handle_t)(-AG_ENOSYS);
#endif
}

static int32_t api_send(ag_handle_t h, const void *buf, size_t len)
{
    if (buf == NULL && len > 0) {
        return -AG_EINVAL;
    }
#if AG_PORT_HAS_TLS
    void *tls = tls_of(h);
    if (tls != NULL) {
        return ag_port_tls_send((ag_port_tls_t)tls, buf, len);
    }
#endif
    const int fd = fd_of(h);
    if (fd < 0) {
        return -AG_EBADF;
    }
    return ag_netprov_send(fd, buf, len);
}

static int32_t api_recv(ag_handle_t h, void *buf, size_t len)
{
    if (buf == NULL && len > 0) {
        return -AG_EINVAL;
    }
#if AG_PORT_HAS_TLS
    void *tls = tls_of(h);
    if (tls != NULL) {
        return ag_port_tls_recv_now((ag_port_tls_t)tls, buf, len);
    }
#endif
    const int fd = fd_of(h);
    if (fd < 0) {
        return -AG_EBADF;
    }
    return ag_netprov_recv(fd, buf, len);
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
    void     *tls = s_tls[slot];
    s_in_use[slot] = false;
    s_fds[slot] = -1;
    s_tls[slot] = NULL;
    s_owner[slot] = AG_PID_KERNEL;
    unlock();
#if AG_PORT_HAS_TLS
    if (tls != NULL) {
        ag_port_tls_close((ag_port_tls_t)tls);
        return AG_OK;
    }
#else
    (void)tls;
#endif
    ag_netprov_close(fd);
    return AG_OK;
}

static ag_err_t api_reset(ag_handle_t h)
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
    void     *tls = s_tls[slot];
    s_in_use[slot] = false;
    s_fds[slot] = -1;
    s_tls[slot] = NULL;
    s_owner[slot] = AG_PID_KERNEL;
    unlock();
#if AG_PORT_HAS_TLS
    if (tls != NULL) {
        /* TLS has no hard reset of its own; a close is the strongest thing. */
        ag_port_tls_close((ag_port_tls_t)tls);
        return AG_OK;
    }
#else
    (void)tls;
#endif
    ag_netprov_close_hard(fd);
    return AG_OK;
}

uint32_t ag_net_close_owned_by(ag_pid_t pid)
{
    if (pid == AG_PID_KERNEL) {
        return 0u; /* the kernel's own are not anybody's to reclaim */
    }

    int      fds[AG_NET_MAX_SOCK];
    void    *tlss[AG_NET_MAX_SOCK];
    uint32_t n = 0;

    lock();
    for (int i = 0; i < AG_NET_MAX_SOCK; i++) {
        if (s_in_use[i] && s_owner[i] == pid) {
            fds[n] = s_fds[i];
            tlss[n] = s_tls[i];
            n++;
            s_in_use[i] = false;
            s_fds[i] = -1;
            s_tls[i] = NULL;
            s_owner[i] = AG_PID_KERNEL;
        }
    }
    unlock();

    /* Outside the lock: a close goes into the stack, and the stack is
     * entitled to take its time about a connection it is still flushing. */
    for (uint32_t i = 0; i < n; i++) {
#if AG_PORT_HAS_TLS
        if (tlss[i] != NULL) {
            ag_port_tls_close((ag_port_tls_t)tlss[i]);
            continue;
        }
#else
        (void)tlss;
#endif
        ag_netprov_close(fds[i]);
    }
    if (n != 0u) {
        ag_log(AG_LOG_INFO, "net", "closed %u socket(s) left by pid %u",
               (unsigned)n, (unsigned)pid);
    }
    return n;
}

static ag_err_t api_set_nonblock(ag_handle_t h, bool on)
{
#if AG_PORT_HAS_TLS
    if (tls_of(h) != NULL) {
        /* A TLS connection is non-blocking from the handshake on (the port
         * contract), so there is nothing to toggle - accept it either way. */
        (void)on;
        return AG_OK;
    }
#endif
    const int fd = fd_of(h);
    if (fd < 0) {
        return -AG_EBADF;
    }
    return ag_netprov_nonblock(fd, on);
}

/*
 * ABI 0.69: one HTTP(S) exchange through the kernel's own client.  The
 * application gets the status code and, if it asked for one, the body in a
 * file; the console gets nothing, because an application polling a server must
 * not be typing into whatever is on the screen.
 *
 * The slot is filled unconditionally.  There is no build of this table without
 * the engine behind it: without CONFIG_ARGON_ENABLE_NET the whole file is not
 * compiled and api->net is NULL, which is how a caller learns there is no
 * network at all.  A build with the network but without TLS keeps the slot and
 * answers -AG_ENOTSUP for an https URL.
 */
static ag_err_t api_http_request(const char *method, const char *url,
                                 const char *headers, const void *body,
                                 uint32_t body_len, const char *dest,
                                 uint16_t *status)
{
    /* An application asking for a URL is asking for the network: see the note
     * on `net_bring_up`.  Not fatal when it cannot be had - the request then
     * fails the way it always did, with the caller's own message. */
    (void)ag_netprov_bring_up(8000u);
    return ag_http_exchange(method, url, headers, body, body_len, dest, status);
}

const ag_net_api_t ag_net_api_impl = {
    .size = sizeof(ag_net_api_t),
    .ready = api_ready,
    .wait_ready = api_wait_ready,
    .ifaddr = api_ifaddr,
    .tcp_listen = api_tcp_listen,
    .tcp_accept = api_tcp_accept,
    .tcp_connect = api_tcp_connect,
    .reset = api_reset,
    .send = api_send,
    .recv = api_recv,
    .close = api_close,
    .set_nonblock = api_set_nonblock,
    .resolve = api_resolve,
    .http_request = api_http_request,
    .tls_connect = api_tls_connect,
};

const ag_net_api_t *ag_net_api_table(void) { return &ag_net_api_impl; }

#endif /* CONFIG_ARGON_ENABLE_NET */
