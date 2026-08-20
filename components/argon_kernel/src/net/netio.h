/*
 * ArgonOS - the plumbing the network commands share (kernel private).
 *
 * A socket delivers whatever happened to arrive, and every protocol above it
 * wants either a line or an exact number of bytes.  Reconciling those two is
 * the same work in the fetch, the server and the file transfer, so it is done
 * once here - along with the progress line, which is the other thing all three
 * need and none of them is about.
 *
 * Nothing here ever waits inside a read.  Waiting is done by asking the port
 * whether anything has arrived (ag_port_net_wait_readable, a select), in slices,
 * and then taking what is there with a read that cannot block.  Two reasons,
 * both learned the hard way:
 *
 * - A timeout that belongs to the stack is not ours to rely on.  SO_RCVTIMEO
 *   does fire under QEMU; a socket wedged for any other reason does not care
 *   about it at all, and the board then sits in a call that never returns until
 *   somebody resets it.
 * - Ctrl+C.  An operator must be able to abandon a transfer that has stalled,
 *   and that is only possible if the waiting happens where the flag is looked
 *   at - here, between slices.
 *
 * The buffer belongs to the caller.  Nothing in this file allocates, because
 * the commands above it already know how long their one allocation lives, and
 * on a chip with this little memory that is the only way to be sure it is
 * released.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NETIO_H
#define ARGON_NETIO_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

/* How long a read or a write waits before it says so.  Long enough for a slow
 * server on a slow link, short enough that a person is still watching. */
#define AG_NETIO_TIMEOUT_MS 15000

/*
 * How long one wait inside select lasts.  The whole timeout is made of these,
 * and between them Ctrl+C is looked at - so this is also how quickly a stalled
 * transfer can be abandoned.
 */
#define AG_NETIO_SLICE_MS 50

/*
 * One read from a non-blocking socket, waited for properly.
 *
 * Returns the bytes read, 0 at end of stream, -AG_ETIMEDOUT when nothing
 * arrived in time, -AG_EINTR when the operator pressed Ctrl+C, or another
 * -AG_E* from the port.
 */
int32_t ag_netio_recv(int fd, void *buf, size_t len, uint32_t timeout_ms);

/* All of it or an error, by the same rules. */
ag_err_t ag_netio_send(int fd, const void *buf, size_t len,
                       uint32_t timeout_ms);

/* ag_netio_send with AG_NETIO_TIMEOUT_MS, which is what the clients want. */
ag_err_t ag_netio_send_all(int fd, const void *buf, size_t len);

/* A formatted control line.  Bounded at 288 bytes - these are request lines
 * and FTP commands, not bodies. */
ag_err_t ag_netio_sendf(int fd, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* ---------------------------------------------------------------------- */

typedef struct {
    int      fd;
    uint8_t *buf;
    size_t   cap;
    size_t   have; /* bytes in buf                                        */
    size_t   pos;  /* bytes of those already handed out                   */
    bool     eof;
    uint32_t timeout_ms;
} ag_netio_t;

/*
 * `prefill` bytes at the front of `buf` are treated as already received - that
 * is how a body starts when the header read pulled some of it in.  The socket
 * must already be non-blocking; see the file header.
 */
void ag_netio_init(ag_netio_t *r, int fd, uint8_t *buf, size_t cap,
                   size_t prefill);

/* Up to `n` bytes.  0 at end of stream, negative on error. */
int32_t ag_netio_read(ag_netio_t *r, void *dst, size_t n);

/*
 * One line, CR and LF removed.  -AG_ERANGE when the line does not fit (the
 * line is then discarded up to its end, so the stream stays usable), -AG_EIO
 * when the connection ended before the line did.
 */
ag_err_t ag_netio_line(ag_netio_t *r, char *out, size_t len);

/* ---------------------------------------------------------------------- */

/*
 * The one line that says a transfer is alive.
 *
 * Rate limited rather than printed per block: at 300 KB/s over Ethernet a line
 * per 1.5 KB block is two hundred lines a second, which on a serial console is
 * most of the transfer's time.
 */
typedef struct {
    uint64_t total; /* 0 when the server did not say                      */
    int64_t  start_us;
    int64_t  last_us;
} ag_progress_t;

void ag_progress_start(ag_progress_t *p, uint64_t total);
void ag_progress_tick(ag_progress_t *p, uint64_t done);

/* Clears the progress line and prints how long it took and how fast. */
void ag_progress_done(ag_progress_t *p, uint64_t done);

#endif /* ARGON_NETIO_H */
