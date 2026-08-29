/*
 * ArgonOS port contract - a TLS client, so a socket can be encrypted.
 *
 * The one thing above here needs is https: connect to a host, verify it against
 * the built-in root certificates, then read and write as if it were a plain
 * socket.  The recv/send shape deliberately matches the plain net port
 * (recv_now returns bytes / 0 at end / -AG_EAGAIN when nothing is ready yet),
 * so the same buffered reader (net/netio.c) drives either one by branching in
 * one place instead of learning two protocols.
 *
 * What a port must supply, when AG_PORT_HAS_TLS is 1:
 *
 *   ag_port_tls_t ag_port_tls_connect(const char *host, uint16_t port,
 *                                     uint32_t timeout_ms)
 *   int32_t  ag_port_tls_recv_now(ag_port_tls_t, void *buf, size_t len)
 *   int32_t  ag_port_tls_send(ag_port_tls_t, const void *buf, size_t len)
 *   int      ag_port_tls_wait_readable(ag_port_tls_t, uint32_t timeout_ms)
 *   size_t   ag_port_tls_pending(ag_port_tls_t)
 *   void     ag_port_tls_close(ag_port_tls_t)
 *
 * Contract:
 * - connect() does the TCP connect and the TLS handshake, verifying the peer
 *   against the root bundle; NULL on any failure (name, connect, or a
 *   certificate that does not check out).  After it returns the connection is
 *   non-blocking, like the plain port's sockets.
 * - recv_now(): >0 bytes, 0 at end of stream, -AG_EAGAIN when nothing is ready
 *   yet, other negative on error.  pending() is the decrypted bytes already
 *   buffered inside TLS - check it before waiting on the socket, since a whole
 *   record can arrive in one packet and leave the socket not readable while
 *   there is still data to hand out.
 * - wait_readable() waits on the underlying socket, exactly as the plain port.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_TLS_H
#define ARGON_PORT_TLS_H

#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

#include <argon/port/impl/tls.h>

#if AG_PORT_HAS_TLS

typedef struct ag_port_tls *ag_port_tls_t; /* opaque connection handle */

ag_port_tls_t ag_port_tls_connect(const char *host, uint16_t port,
                                  uint32_t timeout_ms);
int32_t ag_port_tls_recv_now(ag_port_tls_t tls, void *buf, size_t len);
int32_t ag_port_tls_send(ag_port_tls_t tls, const void *buf, size_t len);
int     ag_port_tls_wait_readable(ag_port_tls_t tls, uint32_t timeout_ms);
size_t  ag_port_tls_pending(ag_port_tls_t tls);
void    ag_port_tls_close(ag_port_tls_t tls);

#endif /* AG_PORT_HAS_TLS */

#endif /* ARGON_PORT_TLS_H */
