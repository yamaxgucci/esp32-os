/*
 * ArgonOS - buffered socket reads and the progress line.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/config.h>

#if CONFIG_ARGON_ENABLE_NET

#include "net/netio.h"

#include <stdio.h>
#include <string.h>

#include <argon/console.h>
#include <argon/port/net.h>
#include <argon/port/time.h>

void ag_netio_init(ag_netio_t *r, int fd, uint8_t *buf, size_t cap,
                   size_t prefill)
{
    r->fd = fd;
    r->buf = buf;
    r->cap = cap;
    r->have = (prefill > cap) ? cap : prefill;
    r->pos = 0;
    r->eof = false;
}

/* Refills only when empty: a half-full buffer still has bytes to hand out, and
 * asking the socket for more of them would block for no reason. */
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
    const int32_t n = ag_port_net_recv(r->fd, r->buf, r->cap);
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

ag_err_t ag_netio_send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t         left = len;

    while (left > 0) {
        const int32_t n = ag_port_net_send(fd, p, left);
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

ag_err_t ag_netio_sendf(int fd, const char *fmt, ...)
{
    char    line[288];
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

    if (p->total > 0) {
        const unsigned pct = (unsigned)((done * 100u) / p->total);
        ag_console_printf("\r  %u/%u KB  %u%%   ", kb(done), kb(p->total), pct);
    } else {
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
