/*
 * ArgonOS - fetching a file over HTTP.
 *
 * The smallest thing that is honestly a client: one connection, one request,
 * the body straight to a file.  No keep-alive, no cache, no compression, and
 * no TLS - that last one is not an omission to be fixed cheaply.  mbedTLS
 * wants tens of kilobytes of RAM for its record buffers alone, and this chip
 * has under a hundred free with the radio up, so an https URL is refused by
 * the URL parser rather than half-supported here.
 *
 * What it does insist on:
 *
 * - The destination file is opened after the server has answered 200, not
 *   before.  A failed fetch that has already truncated the file it was going
 *   to replace has destroyed something to gain nothing.
 * - A body with no length is read until the connection closes, which is what
 *   Connection: close is for, and a chunked body is decoded rather than
 *   written out with its framing.  A file that is 5% chunk headers is not the
 *   file that was asked for, and nothing downstream would notice.
 * - Redirects are followed, up to a few, because half the servers on a network
 *   answer with one - and only to http, so a redirect to https stops rather
 *   than silently fetching over a different scheme.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/config.h>

#if CONFIG_ARGON_ENABLE_NET

#include "net/netsvc.h"

#include <stdio.h>
#include <string.h>

#include <argon/console.h>
#include <argon/net.h>
#include <argon/netmsg.h>
#include <argon/shell.h>
#include <argon/vfs.h>

#include <argon/port/mem.h>
#include <argon/port/net.h>

#include "net/netio.h"

#define HTTP_HDR_MAX 1024
#define HTTP_BODY_BUF 1536
#define HTTP_CONNECT_MS 15000
#define HTTP_MAX_REDIRECTS 4

/* What this system calls itself to a server.  Servers log it, and a log that
 * says which machine asked is worth the twenty bytes. */
#define HTTP_AGENT "ArgonOS/1.0"

/*
 * The bytes the transfer needs, in one allocation and only while it runs.
 *
 * Not static: two and a half kilobytes of permanent data segment is a large
 * fraction of what is left on this chip once the radio is up, and a command
 * that is not running should cost nothing.  The header block and the body
 * buffer are one block because they are wanted at the same time and freed at
 * the same time.
 */
typedef struct {
    char    *hdr;  /* HTTP_HDR_MAX                                        */
    uint8_t *body; /* HTTP_BODY_BUF                                       */
    void    *mem;
} http_bufs_t;

static bool bufs_alloc(http_bufs_t *b)
{
    b->mem = ag_port_alloc(HTTP_HDR_MAX + HTTP_BODY_BUF, AG_MEM_FAST);
    if (b->mem == NULL) {
        return false;
    }
    b->hdr = (char *)b->mem;
    b->body = (uint8_t *)b->mem + HTTP_HDR_MAX;
    return true;
}

static void bufs_free(http_bufs_t *b)
{
    ag_port_free(b->mem);
    b->mem = NULL;
}

/* ---------------------------------------------------------------------- */

/*
 * Everything the file system does with the bytes, in one place, so that the
 * chunked path and the plain path cannot disagree about what a short write
 * means.
 */
static ag_err_t write_all(ag_handle_t out, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        const int32_t n = ag_vfs_write(out, data + off, len - off);
        if (n < 0) {
            return (ag_err_t)n;
        }
        if (n == 0) {
            return -AG_ENOSPC;
        }
        off += (size_t)n;
    }
    return AG_OK;
}

/*
 * Moves `want` bytes (or everything until the stream ends, when want is
 * UINT64_MAX) from the socket into the file.
 */
static ag_err_t pump(ag_netio_t *r, ag_handle_t out, uint64_t want,
                     ag_progress_t *prog, uint64_t *total)
{
    uint8_t  block[256];
    uint64_t moved = 0;

    /* `want` is this call's business; `total` is the transfer's, and the two
     * differ once a chunked body arrives in pieces. */
    while (want == UINT64_MAX || moved < want) {
        size_t ask = sizeof(block);
        if (want != UINT64_MAX && (want - moved) < ask) {
            ask = (size_t)(want - moved);
        }

        const int32_t n = ag_netio_read(r, block, ask);
        if (n < 0) {
            return (ag_err_t)n;
        }
        if (n == 0) {
            /* The end of the stream is the end of the body only when the
             * server never said how long the body was. */
            return (want == UINT64_MAX) ? AG_OK : -AG_EIO;
        }

        const ag_err_t err = write_all(out, block, (size_t)n);
        if (err != AG_OK) {
            return err;
        }
        moved += (uint64_t)n;
        *total += (uint64_t)n;
        ag_progress_tick(prog, *total);

        if (ag_shell_interrupted()) {
            return -AG_EINTR;
        }
    }
    return AG_OK;
}

/* A chunked body, decoded.  The framing is not part of the file. */
static ag_err_t pump_chunked(ag_netio_t *r, ag_handle_t out,
                             ag_progress_t *prog, uint64_t *total)
{
    char line[64];

    for (;;) {
        ag_err_t err = ag_netio_line(r, line, sizeof(line));
        if (err == -AG_ERANGE) {
            return -AG_EFORMAT; /* a chunk size is never 63 characters */
        }
        if (err != AG_OK) {
            return err;
        }
        if (line[0] == '\0') {
            continue; /* the CRLF that ends the previous chunk */
        }

        uint64_t size = 0;
        err = ag_http_chunk_size(line, &size);
        if (err != AG_OK) {
            return -AG_EFORMAT;
        }
        if (size == 0) {
            return AG_OK; /* the last chunk; trailers, if any, are ignored */
        }

        err = pump(r, out, size, prog, total);
        if (err != AG_OK) {
            return err;
        }
    }
}

/* ---------------------------------------------------------------------- */

/*
 * A Location, which may be a whole URL or just a path, turned into a whole URL
 * against the request that produced it.
 */
static ag_err_t redirect_url(const ag_url_t *from, const char *location,
                             char *out, size_t len)
{
    if (location[0] == '\0') {
        return -AG_EFORMAT;
    }
    if (strstr(location, "://") != NULL) {
        return ((size_t)snprintf(out, len, "%s", location) < len)
                   ? AG_OK
                   : -AG_ERANGE;
    }

    if (location[0] == '/') {
        const int n = snprintf(out, len, "%s://%s:%u%s", from->scheme,
                               from->host, (unsigned)from->port, location);
        return (n > 0 && (size_t)n < len) ? AG_OK : -AG_ERANGE;
    }

    /* Relative: the last segment of the old path is replaced, not the path. */
    const char  *slash = strrchr(from->path, '/');
    const size_t keep = (slash != NULL) ? (size_t)(slash - from->path + 1) : 1;
    const int    n = snprintf(out, len, "%s://%s:%u%.*s%s", from->scheme,
                              from->host, (unsigned)from->port, (int)keep,
                              from->path, location);
    return (n > 0 && (size_t)n < len) ? AG_OK : -AG_ERANGE;
}

/*
 * One request.  Returns AG_OK with `next` empty when the file has been saved,
 * or AG_OK with `next` set when the server redirected.
 */
static ag_err_t request_once(const ag_url_t *u, const char *dest,
                             http_bufs_t *b, char *next, size_t next_len)
{
    next[0] = '\0';

    uint32_t addr = 0;
    ag_err_t err = ag_net_lookup(u->host, &addr);
    if (err != AG_OK) {
        ag_console_printf("%s: cannot be resolved\n", u->host);
        return err;
    }

    char ip[16];
    (void)ag_ipv4_str(addr, ip, sizeof(ip));
    if (strcmp(ip, u->host) != 0) {
        ag_console_printf("%s is %s\n", u->host, ip);
    }
    ag_console_printf("%s:%u ... ", ip, (unsigned)u->port);

    const int fd = ag_port_net_connect(addr, u->port, HTTP_CONNECT_MS);
    if (fd < 0) {
        ag_console_puts("no answer\n");
        return (ag_err_t)fd;
    }
    ag_console_puts("connected\n");

    /*
     * Host: carries the port when it is not the default, because a server
     * behind a name-based virtual host answers a bare name with the wrong
     * site.  Connection: close is what makes a body with no length finite.
     */
    if (u->port == 80) {
        err = ag_netio_sendf(fd,
                             "GET %s HTTP/1.1\r\nHost: %s\r\n"
                             "User-Agent: " HTTP_AGENT "\r\n"
                             "Accept: */*\r\nConnection: close\r\n\r\n",
                             u->path, u->host);
    } else {
        err = ag_netio_sendf(fd,
                             "GET %s HTTP/1.1\r\nHost: %s:%u\r\n"
                             "User-Agent: " HTTP_AGENT "\r\n"
                             "Accept: */*\r\nConnection: close\r\n\r\n",
                             u->path, u->host, (unsigned)u->port);
    }
    if (err != AG_OK) {
        ag_port_net_close(fd);
        return err;
    }

    /* The header block, and whatever body arrived stuck to the back of it. */
    size_t have = 0;
    size_t end = 0;
    while (end == 0) {
        if (have == HTTP_HDR_MAX) {
            ag_console_puts("the reply header is too long for this system\n");
            ag_port_net_close(fd);
            return -AG_ERANGE;
        }
        const int32_t n =
            ag_port_net_recv(fd, b->hdr + have, HTTP_HDR_MAX - have);
        if (n < 0) {
            /*
             * Named, because the commonest version of this is a server that
             * accepted the connection and then said nothing at all - and a
             * command that comes back after fifteen seconds with no message
             * looks like a command that did nothing.
             */
            ag_console_puts(
                (n == -AG_EAGAIN || n == -AG_ETIMEDOUT)
                    ? "no reply within fifteen seconds\n"
                    : "the connection failed while waiting for the reply\n");
            ag_port_net_close(fd);
            return (ag_err_t)n;
        }
        if (n == 0) {
            ag_console_puts("the connection closed before the reply\n");
            ag_port_net_close(fd);
            return -AG_EIO;
        }
        have += (size_t)n;
        end = ag_http_header_end(b->hdr, have);
    }

    ag_http_resp_t resp;
    err = ag_http_parse_response(b->hdr, end, &resp);
    if (err != AG_OK) {
        ag_console_puts("this is not an HTTP server\n");
        ag_port_net_close(fd);
        return err;
    }

    if (resp.status >= 300 && resp.status < 400) {
        ag_console_printf("%d %s\n", resp.status,
                          ag_http_status_text(resp.status));
        err = redirect_url(u, resp.location, next, next_len);
        if (err != AG_OK) {
            ag_console_puts("...to somewhere this system cannot express\n");
        }
        ag_port_net_close(fd);
        return err;
    }

    if (resp.status != 200) {
        const char *text = ag_http_status_text(resp.status);
        ag_console_printf("%d%s%s\n", resp.status, (text[0] != '\0') ? " " : "",
                          text);
        ag_port_net_close(fd);
        /* Distinguishable, so that a script can tell "no such file" from "the
         * server is broken". */
        if (resp.status == 404 || resp.status == 410) {
            return -AG_ENOENT;
        }
        if (resp.status == 401 || resp.status == 403) {
            return -AG_EACCES;
        }
        return -AG_EIO;
    }

    /*
     * Answered, so now the file may be replaced.  Not one line earlier: this
     * is the difference between a failed fetch and a lost file.
     */
    const ag_handle_t out =
        ag_vfs_open(dest, NULL, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (out < 0) {
        ag_console_printf("%s: cannot be written\n", dest);
        ag_port_net_close(fd);
        return (ag_err_t)out;
    }

    if (resp.have_length) {
        ag_console_printf("200 OK, %u bytes", (unsigned)resp.length);
    } else {
        ag_console_puts("200 OK, length unknown");
    }
    if (resp.type[0] != '\0') {
        ag_console_printf(", %s", resp.type);
    }
    ag_console_puts("\n");

    /* The body bytes that came with the header start the file. */
    const size_t carried = have - end;
    memcpy(b->body, b->hdr + end, carried);

    ag_netio_t rdr;
    ag_netio_init(&rdr, fd, b->body, HTTP_BODY_BUF, carried);

    ag_progress_t prog;
    ag_progress_start(&prog, resp.have_length ? resp.length : 0);

    uint64_t total = 0;
    if (resp.chunked) {
        err = pump_chunked(&rdr, out, &prog, &total);
    } else {
        err = pump(&rdr, out, resp.have_length ? resp.length : UINT64_MAX,
                   &prog, &total);
    }
    ag_progress_done(&prog, total);

    ag_vfs_close(out);
    ag_port_net_close(fd);

    if (err == -AG_EINTR) {
        /* Named the way the operator typed it, not the way the VFS spells it. */
        char shown[AG_PATH_MAX];
        ag_shell_dos_path(dest, shown, sizeof(shown));
        ag_console_printf("^C - %s holds the %u bytes that arrived\n", shown,
                          (unsigned)total);
        return err;
    }
    if (err != AG_OK) {
        ag_console_printf("the transfer stopped after %u bytes\n",
                          (unsigned)total);
        return err;
    }
    if (resp.have_length && total != resp.length) {
        ag_console_printf("short: %u of %u bytes\n", (unsigned)total,
                          (unsigned)resp.length);
        return -AG_EIO;
    }
    return AG_OK;
}

ag_err_t ag_http_fetch(const char *url_text, const char *dest)
{
    if (url_text == NULL || dest == NULL) {
        return -AG_EINVAL;
    }

    http_bufs_t bufs;
    if (!bufs_alloc(&bufs)) {
        ag_console_puts("not enough memory for a transfer\n");
        return -AG_ENOMEM;
    }

    char     current[288];
    ag_err_t err = AG_OK;
    if ((size_t)snprintf(current, sizeof(current), "%s", url_text) >=
        sizeof(current)) {
        bufs_free(&bufs);
        return -AG_ERANGE;
    }

    for (int hop = 0; hop <= HTTP_MAX_REDIRECTS; hop++) {
        ag_url_t u;
        err = ag_url_parse(current, &u);
        if (err != AG_OK) {
            break;
        }
        /*
         * There is no Basic authentication here, so a URL that carries a name
         * and a password is not going to use them - and a fetch that answers
         * 401 after being given a password looks like a wrong password.
         */
        if (u.user[0] != '\0') {
            ag_console_puts("(the name and password in that URL are ignored: "
                            "http here has no login)\n");
        }
        /*
         * A redirect that changes scheme lands here: ftp:// is a different
         * conversation and https:// is one this system does not have.
         */
        if (strcmp(u.scheme, "http") != 0) {
            ag_console_printf("%s: not http\n", current);
            err = -AG_ENOTSUP;
            break;
        }

        char next[288];
        err = request_once(&u, dest, &bufs, next, sizeof(next));
        if (err != AG_OK || next[0] == '\0') {
            break;
        }
        if (hop == HTTP_MAX_REDIRECTS) {
            ag_console_puts("too many redirects\n");
            err = -AG_EIO;
            break;
        }
        ag_console_printf("-> %s\n", next);
        memcpy(current, next, sizeof(current));
    }

    bufs_free(&bufs);
    return err;
}

#endif /* CONFIG_ARGON_ENABLE_NET */
