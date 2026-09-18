/*
 * ArgonOS port contract - a network, when there is one.
 *
 * Deliberately small and deliberately TCP-shaped.  ArgonOS does not want a
 * socket layer of its own: what it wants is a stream to the other end and a way
 * to say who it is.  Everything above that - which process owns a connection,
 * what happens to it when that process dies, how a handle is numbered - is in
 * src/net/net.c and is the same on every port.
 *
 * The port hands back plain descriptors and the kernel wraps them.  That is the
 * whole division: a port never sees an ag_handle_t, and the kernel never sees a
 * struct sockaddr.
 *
 * What a port must supply:
 *
 *   ag_err_t ag_port_net_start(void)
 *   bool     ag_port_net_ready(void)
 *   ag_err_t ag_port_net_ifaddr(uint32_t *addr)
 *   void     ag_port_net_on_ready(ag_port_net_ready_fn fn)
 *
 *   int      ag_port_net_listen(uint16_t port)
 *   int      ag_port_net_accept(int lfd, uint32_t timeout_ms)
 *   int      ag_port_net_connect(uint32_t addr, uint16_t port,
 *                                uint32_t timeout_ms)
 *   int32_t  ag_port_net_send(int fd, const void *buf, size_t len)
 *   int32_t  ag_port_net_recv(int fd, void *buf, size_t len)
 *   void     ag_port_net_close(int fd)
 *   ag_err_t ag_port_net_nonblock(int fd, bool on)
 *   ag_err_t ag_port_net_resolve(const char *host, uint32_t *addr)
 *   int      ag_port_net_wait_readable(int fd, uint32_t timeout_ms)
 *   int32_t  ag_port_net_recv_now(int fd, void *buf, size_t len)
 *
 * Contract, not advice:
 *
 * - start() returns as soon as the interface is up.  It does not wait for an
 *   address: DHCP takes as long as it takes, and boot does not.  ready() is how
 *   anybody finds out, and the kernel does the waiting.
 * - The descriptors are the port's own numbering and mean nothing to the caller
 *   beyond "non-negative is a socket".  A negative return is an -AG_E* code.
 * - accept() with timeout_ms == 0 returns -AG_EAGAIN rather than blocking, and
 *   with UINT32_MAX blocks forever.  Those have to be separate answers: on lwIP
 *   a zero receive timeout means "wait forever", and taking that shortcut hung a
 *   driver until a program on the development machine happened to connect.
 * - ifaddr() gives host-order IPv4.
 * - resolve() is the one thing above sockets that a port has to answer, and it
 *   is here rather than in the kernel because the resolver belongs to the stack
 *   that owns the interface: it is the DHCP lease that says which server to
 *   ask.  A dotted quad never reaches it - the kernel reads those itself, so a
 *   board with no name service can still be told where to connect.
 * - resolve() blocks, and the length of the block is the resolver's, not the
 *   caller's: a few seconds when the server is slow, longer when a stack
 *   retries.  There is no timeout argument because there is nothing honest to
 *   put in it - lwIP's resolver does not take one.
 * - on_ready() exists so the address can be printed the moment it arrives.
 *   That is not decoration: nobody can use the network without knowing what
 *   the board is called, the address arrives long after boot has moved on, and
 *   nothing polls for it.  Registered before start(); called once, from
 *   whatever context the address arrives in, so the kernel keeps it short.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_NET_H
#define ARGON_PORT_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

/* Host-order IPv4, all three. */
typedef void (*ag_port_net_ready_fn)(uint32_t addr, uint32_t mask, uint32_t gw);

ag_err_t ag_port_net_start(void);
bool     ag_port_net_ready(void);
ag_err_t ag_port_net_ifaddr(uint32_t *addr);
void     ag_port_net_on_ready(ag_port_net_ready_fn fn);

/*
 * What the stack is holding, for an operator asking where the memory went.
 *
 * A connection the application has closed is not necessarily a connection the
 * stack has finished with: with the far end's window shut and its program
 * gone, lwIP keeps the block and everything queued on it and probes for ever.
 * Nothing above this layer can see that, and the difference between "nine
 * kilobytes in one connection" and "forty across nine" is the difference
 * between a leak and a stuck peer.
 *
 * Counts are of TCP blocks; `queued` is what is waiting to be sent or
 * acknowledged on them, in bytes.  Zeroed and left alone on a port with no
 * stack to ask.
 */
typedef struct {
    uint16_t active;    /* established, closing, probing - all of it       */
    uint16_t listening;
    uint16_t time_wait; /* finished, waiting out the last packets          */
    uint16_t bound;     /* a port taken and nothing on it                  */
    uint32_t queued;    /* bytes on the send queues of the active ones     */
    uint16_t stalled;   /* active blocks whose peer is advertising nothing */
} ag_port_net_stats_t;

ag_err_t ag_port_net_stats(ag_port_net_stats_t *out);

int     ag_port_net_listen(uint16_t port);
int     ag_port_net_accept(int lfd, uint32_t timeout_ms);
int     ag_port_net_connect(uint32_t addr, uint16_t port, uint32_t timeout_ms);
int32_t ag_port_net_send(int fd, const void *buf, size_t len);
int32_t ag_port_net_recv(int fd, void *buf, size_t len);
void    ag_port_net_close(int fd);

/*
 * The same, for a peer that is not going to take what is queued.
 *
 * A graceful close leaves TCP trying to deliver, which is right until the far
 * end has stopped reading - then the connection, its send queue and every
 * buffer on it are held until something takes the interface down.  This sends
 * a reset instead: the far end is told, the queue is discarded, and the memory
 * comes back now.  Only for a peer already judged gone; anything else should
 * close.
 */
void    ag_port_net_close_hard(int fd);
ag_err_t ag_port_net_nonblock(int fd, bool on);

/* Host-order IPv4 for a name.  -AG_ENOENT when the name does not resolve,
 * -AG_EAGAIN before there is a network to ask over. */
ag_err_t ag_port_net_resolve(const char *host, uint32_t *addr);

/*
 * Is there anything to read yet?  1 = yes, 0 = not within timeout_ms, negative
 * is an -AG_E* code.  UINT32_MAX waits forever, and nothing above this layer
 * asks for that.
 *
 * This is where the kernel's network services do their waiting: ask first, then
 * read what has already arrived.  It is the same call accept() has always used
 * for its timeout, and it is the only wait in the system that can be broken off
 * by the operator, because the caller comes back between slices.
 */
int ag_port_net_wait_readable(int fd, uint32_t timeout_ms);

/*
 * Whatever has already arrived, and never a wait: -AG_EAGAIN when nothing has.
 *
 * Separate from recv() because recv() is allowed to block and this is not.  The
 * kernel's own services use this one with wait_readable, so that no command is
 * ever inside a call it cannot bound and every wait is a place where Ctrl+C is
 * looked at.  recv() stays for the ABI, where an application asked for a
 * blocking read and knows what it asked for.
 *
 * The buffer must be memory that can be written a byte at a time.  That sounds
 * like it goes without saying; on the ESP32 it does not.  A buffer from
 * MALLOC_CAP_INTERNAL alone can land in the instruction-RAM block at
 * 0x4009xxxx, and a stack copying a reply into that does not fail - the calling
 * task simply never comes back.  Ask for AG_MEM_BYTE.
 */
int32_t ag_port_net_recv_now(int fd, void *buf, size_t len);

#endif /* ARGON_PORT_NET_H */
