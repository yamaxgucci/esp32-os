/*
 * ArgonOS - the network provider indirection.  See argon/netprov.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/config.h>

#if CONFIG_ARGON_ENABLE_NET

#include <argon/netprov.h>

#include <string.h>

#include <argon/device.h>
#include <argon/log.h>
#include <argon/net.h>

#include <argon/port/net.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

/* ------------------------------------------------------------------------ */
/* The built-in provider: the port, wrapped so it has the ag_net_ops_t shape. */
/* The `dev` argument is ignored - the port keeps its own singleton state.    */
/* ------------------------------------------------------------------------ */

static ag_err_t bi_start(ag_device_t *dev)
{
    (void)dev;
    return ag_port_net_start();
}
static bool bi_ready(ag_device_t *dev)
{
    (void)dev;
    return ag_port_net_ready();
}
static ag_err_t bi_ifaddr(ag_device_t *dev, uint32_t *addr)
{
    (void)dev;
    return ag_port_net_ifaddr(addr);
}
static ag_err_t bi_resolve(ag_device_t *dev, const char *host, uint32_t *addr)
{
    (void)dev;
    return ag_port_net_resolve(host, addr);
}
static int bi_listen(ag_device_t *dev, uint16_t port)
{
    (void)dev;
    return ag_port_net_listen(port);
}
static int bi_accept(ag_device_t *dev, int lfd, uint32_t timeout_ms)
{
    (void)dev;
    return ag_port_net_accept(lfd, timeout_ms);
}
static int bi_connect(ag_device_t *dev, uint32_t addr, uint16_t port,
                      uint32_t timeout_ms)
{
    (void)dev;
    return ag_port_net_connect(addr, port, timeout_ms);
}
static int32_t bi_send(ag_device_t *dev, int fd, const void *buf, size_t len)
{
    (void)dev;
    return ag_port_net_send(fd, buf, len);
}
static int32_t bi_recv(ag_device_t *dev, int fd, void *buf, size_t len)
{
    (void)dev;
    return ag_port_net_recv(fd, buf, len);
}
static void bi_close(ag_device_t *dev, int fd)
{
    (void)dev;
    ag_port_net_close(fd);
}
static ag_err_t bi_nonblock(ag_device_t *dev, int fd, bool on)
{
    (void)dev;
    return ag_port_net_nonblock(fd, on);
}
static int bi_wait_readable(ag_device_t *dev, int fd, uint32_t timeout_ms)
{
    (void)dev;
    return ag_port_net_wait_readable(fd, timeout_ms);
}
static int32_t bi_recv_now(ag_device_t *dev, int fd, void *buf, size_t len)
{
    (void)dev;
    return ag_port_net_recv_now(fd, buf, len);
}

static const ag_net_ops_t s_builtin = {
    .size = sizeof(ag_net_ops_t),
    .start = bi_start,
    .ready = bi_ready,
    .ifaddr = bi_ifaddr,
    .resolve = bi_resolve,
    .listen = bi_listen,
    .accept = bi_accept,
    .connect = bi_connect,
    .send = bi_send,
    .recv = bi_recv,
    .net_close = bi_close,
    .nonblock = bi_nonblock,
    .wait_readable = bi_wait_readable,
    .recv_now = bi_recv_now,
};

/* ------------------------------------------------------------------------ */
/* Active provider.  NULL device means "the built-in one".                   */
/* ------------------------------------------------------------------------ */

static const ag_net_ops_t *s_ops = &s_builtin;
static ag_device_t        *s_dev; /* NULL for built-in */
static char                s_name[AG_DEV_NAME_MAX] = "builtin";

/* ------------------------------------------------------------------------ */
/* Dispatch.  One indirect call each; the client above cannot tell which     */
/* provider it reached.                                                       */
/* ------------------------------------------------------------------------ */

ag_err_t ag_netprov_start(void) { return s_ops->start(s_dev); }
bool     ag_netprov_ready(void) { return s_ops->ready(s_dev); }
ag_err_t ag_netprov_ifaddr(uint32_t *addr) { return s_ops->ifaddr(s_dev, addr); }
ag_err_t ag_netprov_resolve(const char *host, uint32_t *addr)
{
    return s_ops->resolve(s_dev, host, addr);
}
int ag_netprov_listen(uint16_t port) { return s_ops->listen(s_dev, port); }
int ag_netprov_accept(int lfd, uint32_t timeout_ms)
{
    return s_ops->accept(s_dev, lfd, timeout_ms);
}
int ag_netprov_connect(uint32_t addr, uint16_t port, uint32_t timeout_ms)
{
    return s_ops->connect(s_dev, addr, port, timeout_ms);
}
int32_t ag_netprov_send(int fd, const void *buf, size_t len)
{
    return s_ops->send(s_dev, fd, buf, len);
}
int32_t ag_netprov_recv(int fd, void *buf, size_t len)
{
    return s_ops->recv(s_dev, fd, buf, len);
}
void     ag_netprov_close(int fd) { s_ops->net_close(s_dev, fd); }
void     ag_netprov_close_hard(int fd)
{
    if (ag_netprov_is_builtin()) {
        ag_port_net_close_hard(fd);
        return;
    }
    s_ops->net_close(s_dev, fd);
}
ag_err_t ag_netprov_nonblock(int fd, bool on)
{
    return s_ops->nonblock(s_dev, fd, on);
}
int ag_netprov_wait_readable(int fd, uint32_t timeout_ms)
{
    return s_ops->wait_readable(s_dev, fd, timeout_ms);
}
int32_t ag_netprov_recv_now(int fd, void *buf, size_t len)
{
    return s_ops->recv_now(s_dev, fd, buf, len);
}

/* ------------------------------------------------------------------------ */
/* Control.                                                                   */
/* ------------------------------------------------------------------------ */

const char *ag_netprov_active(void) { return s_name; }
bool ag_netprov_is_builtin(void) { return s_dev == NULL; }

/* Every function pointer the kernel actually dials must be present; a NULL one
 * would fault the first time a socket was opened, and the operator should be
 * told at bind time instead. */
static bool ops_complete(const ag_net_ops_t *o)
{
    return o != NULL && o->size >= sizeof(ag_net_ops_t) && o->start &&
           o->ready && o->ifaddr && o->resolve && o->listen && o->accept &&
           o->connect && o->send && o->recv && o->net_close && o->nonblock &&
           o->wait_readable && o->recv_now;
}

/* Print the address once it is up, so `net use` reads like a link coming on.
 * Bounded: a provider that never gets an address returns after the budget and
 * the operator can query later with `net`. */
static void announce_when_ready(void)
{
    const ag_port_ticks_t start = ag_port_ticks();
    const ag_port_ticks_t budget = ag_port_ms_to_ticks(4000);
    while (!s_ops->ready(s_dev)) {
        if ((ag_port_ticks() - start) >= budget) {
            ag_log(AG_LOG_INFO, "net", "%s: no address yet", s_name);
            return;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(50));
    }
    uint32_t addr = 0;
    if (s_ops->ifaddr(s_dev, &addr) == AG_OK) {
        ag_log(AG_LOG_INFO, "net", "%s ip %u.%u.%u.%u", s_name,
               (unsigned)(addr >> 24), (unsigned)((addr >> 16) & 0xffu),
               (unsigned)((addr >> 8) & 0xffu), (unsigned)(addr & 0xffu));
    }
}

ag_err_t ag_netprov_use_builtin(void)
{
    if (s_dev == NULL) {
        return AG_OK; /* already the built-in one */
    }
    ag_net_reset_sockets();
    s_ops = &s_builtin;
    s_dev = NULL;
    strcpy(s_name, "builtin");
    return ag_port_net_start();
}

ag_err_t ag_netprov_use_device(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return -AG_EINVAL;
    }
    if (strcmp(name, "builtin") == 0) {
        return ag_netprov_use_builtin();
    }

    ag_device_t *dev = ag_dev_find(name);
    if (dev == NULL) {
        return -AG_ENODEV;
    }
    if (dev->cls != AG_DEV_NET) {
        ag_log(AG_LOG_ERROR, "net", "%s is not a network device", name);
        return -AG_EINVAL;
    }
    const ag_net_ops_t *ops = (const ag_net_ops_t *)dev->class_ops;
    if (!ops_complete(ops)) {
        ag_log(AG_LOG_ERROR, "net", "%s: incomplete net ops", name);
        return -AG_ENOTSUP;
    }

    ag_net_reset_sockets();
    s_ops = ops;
    s_dev = dev;
    strncpy(s_name, dev->name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';

    const ag_err_t e = s_ops->start(s_dev);
    if (e != AG_OK) {
        ag_log(AG_LOG_ERROR, "net", "%s did not start: %d", s_name, (int)e);
        /* Fall back so the system is never left with a dead provider. */
        (void)ag_netprov_use_builtin();
        return e;
    }
    announce_when_ready();
    return AG_OK;
}

#endif /* CONFIG_ARGON_ENABLE_NET */
