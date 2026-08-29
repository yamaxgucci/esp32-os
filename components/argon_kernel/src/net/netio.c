/*
 * ArgonOS - waiting on a socket without trusting it, and the progress line.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/config.h>

#if CONFIG_ARGON_ENABLE_NET

#include "net/netio.h"

#include <stdio.h>
#include <string.h>

#include <argon/console.h>
#include <argon/shell.h>

#include <argon/port/net.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

/* Everything in this file waits the same way; this is that way. */
static bool wait_a_moment(int64_t deadline, ag_err_t *why)
{
    if (ag_shell_interrupted()) {
        *why = -AG_EINTR;
        return false;
    }
    if (ag_port_us() > deadline) {
        *why = -AG_ETIMEDOUT;
        return false;
    }
    return true;
}

int32_t ag_netio_recv(int fd, void *buf, size_t len, uint32_t timeout_ms)
{
    const int64_t deadline = ag_port_us() + (int64_t)timeout_ms * 1000;

    for (;;) {
        /*
         * Ask before reading.  A read that arrives before the data does may
         * never come back on this hardware (see port/net.h), so the only
         * thing anybody here waits inside is select.
         */
        const int ready = ag_port_net_wait_readable(fd, AG_NETIO_SLICE_MS);
        if (ready < 0) {
            return (int32_t)ready;
        }
        if (ready > 0) {
            const int32_t n = ag_port_net_recv_now(fd, buf, len);
            if (n != -AG_EAGAIN) {
                return n;
            }
        }
        ag_err_t why = AG_OK;
        if (!wait_a_moment(deadline, &why)) {
            return (int32_t)why;
        }
    }
}

ag_err_t ag_netio_send(int fd, const void *buf, size_t len,
                       uint32_t timeout_ms)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t         left = len;
    const int64_t  deadline = ag_port_us() + (int64_t)timeout_ms * 1000;

    while (left > 0) {
        const int32_t n = ag_port_net_send(fd, p, left);
        if (n == -AG_EAGAIN) {
            ag_err_t why = AG_OK;
            if (!wait_a_moment(deadline, &why)) {
                return why;
            }
            continue;
        }
        if (n < 0) {
            return (ag_err_t)n;
        }
        if (n == 0) {
            return -AG_EIO;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    return AG_OK;
}

ag_err_t ag_netio_send_all(int fd, const void *buf, size_t len)
{
    return ag_netio_send(fd, buf, len, AG_NETIO_TIMEOUT_MS);
}

ag_err_t ag_netio_sendf(int fd, const char *fmt, ...)
{
    char    line[512];
    va_list ap;

    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (n < 0) {
        return -AG_EINVAL;
    }
    /* A truncated request line asks for the wrong thing; refuse to send it. */
    if ((size_t)n >= sizeof(line)) {
        return -AG_ERANGE;
    }
    return ag_netio_send_all(fd, line, (size_t)n);
}

/* ---------------------------------------------------------------------- */

void ag_netio_init(ag_netio_t *r, int fd, uint8_t *buf, size_t cap,
                   size_t prefill)
{
    r->fd = fd;
    r->buf = buf;
    r->cap = cap;
    r->have = (prefill > cap) ? cap : prefill;
    r->pos = 0;
    r->eof = false;
    r->timeout_ms = AG_NETIO_TIMEOUT_MS;
}

/* Refills only when empty: a half-full buffer still has bytes to hand out, and
 * asking the socket for more of them would wait for no reason. */
static int32_t fill(ag_netio_t *r)
{
    if (r->pos < r->have) {
        return (int32_t)(r->have - r->pos);
    }
    if (r->eof) {
        return 0;
    }

    r->pos = 0;
    r->have = 0;
    const int32_t n = ag_netio_recv(r->fd, r->buf, r->cap, r->timeout_ms);
    if (n < 0) {
        return n;
    }
    if (n == 0) {
        r->eof = true;
        return 0;
    }
    r->have = (size_t)n;
    return n;
}

int32_t ag_netio_read(ag_netio_t *r, void *dst, size_t n)
{
    if (r == NULL || dst == NULL) {
        return -AG_EINVAL;
    }
    if (n == 0) {
        return 0;
    }

    const int32_t avail = fill(r);
    if (avail <= 0) {
        return avail;
    }

    size_t take = (size_t)avail;
    if (take > n) {
        take = n;
    }
    memcpy(dst, r->buf + r->pos, take);
    r->pos += take;
    return (int32_t)take;
}

ag_err_t ag_netio_line(ag_netio_t *r, char *out, size_t len)
{
    if (r == NULL || out == NULL || len == 0) {
        return -AG_EINVAL;
    }

    size_t written = 0;
    bool   overflowed = false;

    for (;;) {
        const int32_t avail = fill(r);
        if (avail < 0) {
            return (ag_err_t)avail;
        }
        if (avail == 0) {
            /* End of stream: a last line without a terminator still counts. */
            if (written == 0) {
                out[0] = '\0';
                return -AG_EIO;
            }
            break;
        }

        const char c = (char)r->buf[r->pos++];
        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (written + 1 < len) {
            out[written++] = c;
        } else {
            /* Keep consuming to the end of the line: the stream after it is
             * still a protocol, and abandoning it mid-line loses that. */
            overflowed = true;
        }
    }

    out[written] = '\0';
    return overflowed ? -AG_ERANGE : AG_OK;
}

/* ---------------------------------------------------------------------- */

void ag_progress_start(ag_progress_t *p, uint64_t total)
{
    p->total = total;
    p->start_us = ag_port_us();
    p->last_us = p->start_us;
}

/* Kilobytes, because a byte count on a forty-column screen is mostly digits. */
static unsigned kb(uint64_t bytes) { return (unsigned)(bytes / 1024u); }

void ag_progress_tick(ag_progress_t *p, uint64_t done)
{
    const int64_t now = ag_port_us();
    if (now - p->last_us < 500000 && done != p->total) {
        return;
    }
    p->last_us = now;

    /* Under a few kilobytes there is nothing to watch, and "0/0 KB 100%" is
     * worse than nothing: the line that says how many bytes arrived is along
     * in a moment. */
    if (p->total > 0 && p->total < 4096) {
        return;
    }

    if (p->total > 0) {
        const unsigned pct = (unsigned)((done * 100u) / p->total);
        ag_console_printf("\r  %u/%u KB  %u%%   ", kb(done), kb(p->total), pct);
    } else if (done >= 4096) {
        ag_console_printf("\r  %u KB   ", kb(done));
    }
}

void ag_progress_done(ag_progress_t *p, uint64_t done)
{
    const int64_t  us = ag_port_us() - p->start_us;
    const unsigned ms = (unsigned)(us / 1000);

    const unsigned rate =
        (ms > 0) ? (unsigned)((done * 1000u) / ((uint64_t)ms * 1024u)) : 0;

    ag_console_printf("\r%u bytes in %u.%us", (unsigned)done, ms / 1000,
                      (ms % 1000) / 100);
    if (rate > 0) {
        ag_console_printf(" (%u KB/s)", rate);
    }
    ag_console_puts("        \n");
}

#endif /* CONFIG_ARGON_ENABLE_NET */
