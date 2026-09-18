/*
 * ArgonOS - the network provider indirection.
 *
 * The kernel used to call ag_port_net_* directly, from two places at once: the
 * ABI handle table in net.c, and every kernel service that holds a raw fd
 * (netio.c, httpc.c, ftpc.c, ssh.c, mqtt.c, modbus.c, telnet_console.c).  All
 * of them now call ag_netprov_* instead, and this one layer decides who answers:
 * the built-in stack (the port, over lwIP) or a loaded .SYS bound as the active
 * provider (an external radio on a wire).
 *
 * One provider is active at a time.  That is the whole simplification: an fd is
 * unambiguous because only one id space exists, "is the network ready" and
 * "what is my address" have one answer, and switching providers is an operator
 * action of the same weight as `wifi off` - the sockets that were open are torn
 * down, because they belonged to the interface that is going away.
 *
 * The signatures are, deliberately, the port's signatures (argon/port/net.h)
 * with ag_port_ renamed to ag_netprov_.  The clients above could not tell the
 * difference before and must not be able to now; the rename is the only change
 * a client saw.  ag_port_net_on_ready has no ag_netprov_ twin: it is a built-in
 * concept (a port callback), and net.c keeps calling it directly.  With an
 * external provider active the address is announced when the provider is bound
 * instead.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NETPROV_H
#define ARGON_NETPROV_H

#include <argon/port/config.h>

#if CONFIG_ARGON_ENABLE_NET

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

/* The dispatchers - one per port net function, same signature, called by every
 * kernel network client in place of ag_port_net_*. */
ag_err_t ag_netprov_start(void);
bool     ag_netprov_ready(void);
ag_err_t ag_netprov_ifaddr(uint32_t *addr);
ag_err_t ag_netprov_resolve(const char *host, uint32_t *addr);

int      ag_netprov_listen(uint16_t port);
int      ag_netprov_accept(int lfd, uint32_t timeout_ms);
int      ag_netprov_connect(uint32_t addr, uint16_t port, uint32_t timeout_ms);
int32_t  ag_netprov_send(int fd, const void *buf, size_t len);
int32_t  ag_netprov_recv(int fd, void *buf, size_t len);
void     ag_netprov_close(int fd);

/*
 * Throw the connection away: a reset to the far end, the queue discarded, the
 * memory back at once.  For a peer already judged gone - see api->net->reset.
 *
 * Only the built-in stack can do it; a radio on the other end of a wire has
 * one close and that is the one it gets.  Falling back is right rather than
 * refusing: the caller has decided this connection is finished either way.
 */
void     ag_netprov_close_hard(int fd);
ag_err_t ag_netprov_nonblock(int fd, bool on);
int      ag_netprov_wait_readable(int fd, uint32_t timeout_ms);
int32_t  ag_netprov_recv_now(int fd, void *buf, size_t len);

/*
 * Control.  Binding a device looks it up in the registry, checks it is an
 * AG_DEV_NET with a well-formed ag_net_ops_t, tears down the sockets the old
 * provider owned, makes the new one active and starts it.  use_builtin puts the
 * port back.  active() names whoever is bound ("builtin" or the device name),
 * for the `net` command and the shell prompt.
 */
ag_err_t    ag_netprov_use_builtin(void);
ag_err_t    ag_netprov_use_device(const char *name);
const char *ag_netprov_active(void);
bool        ag_netprov_is_builtin(void);

#endif /* CONFIG_ARGON_ENABLE_NET */

#endif /* ARGON_NETPROV_H */
