/*
 * ArgonOS - the plumbing the network commands share (kernel private).
 *
 * A socket delivers whatever happened to arrive, and every protocol above it
 * wants either a line or an exact number of bytes.  Reconciling those two is
 * the same work in the fetch, the server and the file transfer, so it is done
 * once here - along with the progress line, which is the other thing all three
 * need and none of them is about.
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

typedef struct {
    int      fd;
    uint8_t *buf;
    size_t   cap;
    size_t   have; /* bytes in buf                                        */
    size_t   pos;  /* bytes of those already handed out                   */
    bool     eof;
} ag_netio_t;

/*
 * `prefill` bytes at the front of `buf` are treated as already received.  That
 * is how a body starts: the header read almost always pulls in the first of it,
 * and those bytes are part of the file.
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

/* Everything, or an error.  A short send is not a failure by itself. */
ag_err_t ag_netio_send_all(int fd, const void *buf, size_t len);

/* A formatted control line.  Bounded at 288 bytes - these are request lines
 * and FTP commands, not bodies. */
ag_err_t ag_netio_sendf(int fd, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

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
