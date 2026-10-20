/*
 * ArgonOS - one HTTP(S) exchange: request out, body to a file or nowhere.
 *
 * The smallest thing that is honestly a client: one connection, one request,
 * the body straight to a file.  No keep-alive, no cache, no compression.
 * https is supported when the build has TLS (CONFIG_ARGON_NET_TLS): the
 * connection then comes from the TLS port instead of a plain socket, and the
 * same buffered reader drives it by branching in one place (net/netio.c).
 * mbedTLS is not free - tens of kilobytes of RAM for its record buffers - so
 * TLS is a build option, and without it an https URL is refused by the URL
 * parser rather than fetched in clear.
 *
 * It serves two callers, which is why the method, the extra headers, the body
 * and the throw-the-answer-away case are all here rather than in a second
 * file: `wget` at the console wants GET and a progress line, and an
 * application (ABI 0.69's net->http_request) wants POST, an Authorization
 * header, and silence.  Two clients would be two sets of timeouts and two
 * chances to forget that the destination file must not be truncated until the
 * server has answered.
 *
 * What it does insist on:
 *
 * - The destination file is opened after the server has answered 2xx, not
 *   before.  A failed fetch that has already truncated the file it was going
 *   to replace has destroyed something to gain nothing.
 * - A body with no length is read until the connection closes, which is what
 *   Connection: close is for, and a chunked body is decoded rather than
 *   written out with its framing.  A file that is 5% chunk headers is not the
 *   file that was asked for, and nothing downstream would notice.
 * - Redirects are followed, up to a few, because half the servers on a network
 *   answer with one - and only to http or https, so a redirect to a scheme this
 *   build cannot speak stops rather than fetching something else.
 * - A POST that is redirected stops being one unless the server said 307 or
 *   308, which is what a browser does and what every framework's
 *   answer-a-form-with-a-redirect expects.  Sending the body twice is how a
 *   button gets pressed twice.
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
#include <argon/netprov.h>
#include <argon/port/net.h>
#include <argon/port/task.h>
#include <argon/port/time.h>
#include <argon/port/tls.h>

#include "net/netio.h"

#define HTTP_HDR_MAX 1024
#define HTTP_REQ_MAX 1024
#define HTTP_BODY_BUF 1536
#define HTTP_CONNECT_MS 15000
#define HTTP_MAX_REDIRECTS 4

/* What this system calls itself to a server.  Servers log it, and a log that
 * says which machine asked is worth the twenty bytes. */
#define HTTP_AGENT "ArgonOS/1.0"

/* Console output, unless the caller asked for silence.  An application that
 * polls a server every ten seconds must not be typing into the console of
 * whatever is on the screen at the time. */
#define SAY(q, ...)                                                          \
    do {                                                                     \
        if (!(q)) {                                                          \
            ag_console_printf(__VA_ARGS__);                                  \
        }                                                                    \
    } while (0)
#define SAYS(q, s)                                                           \
    do {                                                                     \
        if (!(q)) {                                                          \
            ag_console_puts(s);                                              \
        }                                                                    \
    } while (0)

/* One exchange, as the caller described it. */
typedef struct {
    const char *method;  /* "GET" or "POST"                               */
    const char *headers; /* extra "Name: value\r\n" lines, or NULL        */
    const void *body;    /* request body, POST only                       */
    uint32_t    body_len;
    const char *dest;    /* NULL: read the answer and throw it away       */
    uint16_t   *status;  /* out: the last status code seen, 0 if none     */
    bool        quiet;
} http_req_t;

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
    /*
     * AG_MEM_BYTE is not decoration.  Without it the allocator may answer from
     * memory that is only reachable as instructions - on this chip that is a
     * six-kilobyte block at 0x4009xxxx - and a socket copying a reply into it
     * does not fail, it stops: the task never comes back out of recv.  Two
     * hours of the board's first afternoon went into that one, and the fix is
     * this flag.  Anything a driver or a stack writes bytes into needs it.
     */
    b->mem = ag_port_alloc(HTTP_HDR_MAX + HTTP_BODY_BUF,
                           AG_MEM_FAST | AG_MEM_BYTE);
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
/* Building the request                                                     */
/* ---------------------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t len;
    size_t at;
    bool   overflow;
} writer_t;

static void w_str(writer_t *w, const char *s)
{
    for (size_t i = 0; s[i] != '\0'; i++) {
        if (w->at + 1 >= w->len) {
            w->overflow = true;
            return;
        }
        w->buf[w->at++] = s[i];
    }
    w->buf[w->at] = '\0';
}

static void w_u32(writer_t *w, uint32_t v)
{
    char tmp[10];
    char out[11];
    int  n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    for (int i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    out[n] = '\0';
    w_str(w, out);
}

/* Writes the request line and the headers into `req`. */
static ag_err_t build_request(const ag_url_t *u, const http_req_t *r,
                              char *req, size_t req_len)
{
    writer_t w = {req, req_len, 0, false};

    w_str(&w, r->method);
    w_str(&w, " ");
    w_str(&w, u->path);
    w_str(&w, " HTTP/1.1\r\nHost: ");
    w_str(&w, u->host);
    /* Port in Host: a server behind a name-based virtual host answers a bare
     * name with the wrong site. */
    if (u->port != 80 && u->port != 443) {
        w_str(&w, ":");
        w_u32(&w, (uint32_t)u->port);
    }
    w_str(&w, "\r\nUser-Agent: " HTTP_AGENT "\r\nAccept: */*\r\n");
    /* Connection: close is what makes a body with no length finite. */
    w_str(&w, "Connection: close\r\n");
    if (r->body_len != 0u) {
        w_str(&w, "Content-Length: ");
        w_u32(&w, r->body_len);
        w_str(&w, "\r\n");
    }
    if (r->headers != NULL) {
        w_str(&w, r->headers);
    }
    w_str(&w, "\r\n");

    return w.overflow ? -AG_ERANGE : AG_OK;
}

/* ---------------------------------------------------------------------- */

/*
 * Everything the file system does with the bytes, in one place, so that the
 * chunked path and the plain path cannot disagree about what a short write
 * means.  `out < 0` means the caller wants the answer counted and thrown away.
 */
static ag_err_t write_all(ag_handle_t out, const uint8_t *data, size_t len)
{
    if (out < 0) {
        return AG_OK;
    }
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
 * The connection request_once talks over is either a plain fd or a TLS handle.
 * These two helpers are the only places that care which; everywhere else the
 * buffered reader (with rdr.tls set) hides the difference.
 */
static ag_err_t conn_send_all(int fd, void *tls, const void *buf, size_t len)
{
#if AG_PORT_HAS_TLS
    if (tls != NULL) {
        const uint8_t *p = (const uint8_t *)buf;
        size_t         left = len;
        const int64_t  deadline =
            ag_port_us() + (int64_t)AG_NETIO_TIMEOUT_MS * 1000;
        while (left > 0) {
            const int32_t n = ag_port_tls_send((ag_port_tls_t)tls, p, left);
            if (n == -AG_EAGAIN) {
                if (ag_shell_interrupted()) {
                    return -AG_EINTR;
                }
                if (ag_port_us() > deadline) {
                    return -AG_ETIMEDOUT;
                }
                ag_port_task_delay(ag_port_ms_to_ticks(AG_NETIO_SLICE_MS));
                continue;
            }
            if (n < 0) {
                return (ag_err_t)n;
            }
            p += (size_t)n;
            left -= (size_t)n;
        }
        return AG_OK;
    }
#else
    (void)tls;
#endif
    return ag_netio_send_all(fd, buf, len);
}

static void conn_close(int fd, void *tls)
{
#if AG_PORT_HAS_TLS
    if (tls != NULL) {
        ag_port_tls_close((ag_port_tls_t)tls);
        return;
    }
#else
    (void)tls;
#endif
    if (fd >= 0) {
        ag_netprov_close(fd);
    }
}

/*
 * One request.  Returns AG_OK with `next` empty when the exchange succeeded,
 * or AG_OK with `next` set when the server redirected.
 */
static ag_err_t request_once(const ag_url_t *u, const http_req_t *r,
                             http_bufs_t *b, char *next, size_t next_len)
{
    next[0] = '\0';
    const bool quiet = r->quiet;

    uint32_t addr = 0;
    ag_err_t err = ag_net_lookup(u->host, &addr);
    if (err != AG_OK) {
        SAY(quiet, "%s: cannot be resolved\n", u->host);
        return err;
    }

    char ip[16];
    (void)ag_ipv4_str(addr, ip, sizeof(ip));
    if (strcmp(ip, u->host) != 0) {
        SAY(quiet, "%s is %s\n", u->host, ip);
    }
    SAY(quiet, "%s:%u ... ", ip, (unsigned)u->port);

    int        fd = -1;
    void      *tls = NULL;
    const bool https = (strcmp(u->scheme, "https") == 0);
    if (https) {
#if AG_PORT_HAS_TLS
        /* esp_tls resolves, connects and does the handshake, verifying the peer
         * against the certificate bundle. */
        tls = ag_port_tls_connect(u->host, u->port, HTTP_CONNECT_MS);
        if (tls == NULL) {
            SAYS(quiet, "no answer (or the certificate did not check out)\n");
            return -AG_EIO;
        }
#else
        SAYS(quiet, "no TLS in this build\n");
        return -AG_ENOTSUP;
#endif
    } else {
        fd = ag_netprov_connect(addr, u->port, HTTP_CONNECT_MS);
        if (fd < 0) {
            SAYS(quiet, "no answer\n");
            return (ag_err_t)fd;
        }
        /*
         * From here the socket never blocks and netio does the waiting.  On this
         * hardware a blocking read of a fresh connection does not return at all
         * - see net/netio.h - and even where it does, a command that cannot be
         * interrupted while it waits is a board that has to be reset.  (The TLS
         * connection is set non-blocking by the port after its handshake.)
         */
        (void)ag_netprov_nonblock(fd, true);
    }
    SAYS(quiet, "connected\n");

    char req[HTTP_REQ_MAX];
    err = build_request(u, r, req, sizeof(req));
    if (err != AG_OK) {
        conn_close(fd, tls);
        return err;
    }
    err = conn_send_all(fd, tls, req, strlen(req));
    if (err == AG_OK && r->body_len != 0u && r->body != NULL) {
        err = conn_send_all(fd, tls, r->body, r->body_len);
    }
    if (err != AG_OK) {
        conn_close(fd, tls);
        return err;
    }

    /*
     * The header, read through the same buffered reader the body will use.
     *
     * A byte at a time, which sounds worse than it is: the reader is doing the
     * receiving, so this is a copy out of its buffer, not a syscall each time.
     * The point is that the first byte of the body stays in the reader instead
     * of being read into the header buffer and copied back - and that is where
     * a fetch loses the first kilobyte of a file if it gets it wrong.
     */
    ag_netio_t rdr;
    ag_netio_init(&rdr, fd, b->body, HTTP_BODY_BUF, 0);
    rdr.tls = tls; /* NULL for http; the reader then branches to TLS on refill */

    size_t have = 0;
    while (ag_http_header_end(b->hdr, have) == 0) {
        if (have == HTTP_HDR_MAX) {
            SAYS(quiet, "the reply header is too long for this system\n");
            conn_close(fd, tls);
            return -AG_ERANGE;
        }
        const int32_t n = ag_netio_read(&rdr, b->hdr + have, 1);
        if (n < 0) {
            /*
             * Named, because the commonest version of this is a server that
             * accepted the connection and then said nothing at all - and a
             * command that comes back fifteen seconds later with no message
             * looks like a command that did nothing.
             */
            if (n == -AG_ETIMEDOUT) {
                SAYS(quiet, "no reply within fifteen seconds\n");
            } else if (n == -AG_EINTR) {
                SAYS(quiet, "^C\n");
            } else {
                SAYS(quiet,
                     "the connection failed while waiting for the reply\n");
            }
            conn_close(fd, tls);
            return (ag_err_t)n;
        }
        if (n == 0) {
            SAYS(quiet, "the connection closed before the reply\n");
            conn_close(fd, tls);
            return -AG_EIO;
        }
        have += (size_t)n;
    }
    const size_t end = have;

    ag_http_resp_t resp;
    err = ag_http_parse_response(b->hdr, end, &resp);
    if (err != AG_OK) {
        SAYS(quiet, "this is not an HTTP server\n");
        conn_close(fd, tls);
        return err;
    }
    if (r->status != NULL) {
        *r->status = resp.status;
    }

    if (resp.status >= 300 && resp.status < 400) {
        SAY(quiet, "%d %s\n", resp.status, ag_http_status_text(resp.status));
        err = redirect_url(u, resp.location, next, next_len);
        if (err != AG_OK) {
            SAYS(quiet, "...to somewhere this system cannot express\n");
        }
        conn_close(fd, tls);
        return err;
    }

    if (resp.status < 200 || resp.status >= 300) {
        const char *text = ag_http_status_text(resp.status);
        SAY(quiet, "%d%s%s\n", resp.status, (text[0] != '\0') ? " " : "",
            text);
        conn_close(fd, tls);
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
     * is the difference between a failed fetch and a lost file.  A NULL
     * destination means the caller wants the status and not the answer, so
     * nothing is opened and the body is counted and dropped.
     */
    ag_handle_t out = -1;
    if (r->dest != NULL) {
        out = ag_vfs_open(r->dest, NULL,
                          AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
        if (out < 0) {
            SAY(quiet, "%s: cannot be written\n", r->dest);
            conn_close(fd, tls);
            return (ag_err_t)out;
        }
    }

    if (!quiet) {
        if (resp.have_length) {
            ag_console_printf("%d OK, %u bytes", resp.status,
                              (unsigned)resp.length);
        } else {
            ag_console_printf("%d OK, length unknown", resp.status);
        }
        if (resp.type[0] != '\0') {
            ag_console_printf(", %s", resp.type);
        }
        ag_console_puts("\n");
    }

    ag_progress_t prog;
    ag_progress_start(&prog, resp.have_length ? resp.length : 0);

    uint64_t total = 0;
    if (resp.chunked) {
        err = pump_chunked(&rdr, out, &prog, &total);
    } else {
        err = pump(&rdr, out, resp.have_length ? resp.length : UINT64_MAX,
                   &prog, &total);
    }
    if (!quiet) {
        ag_progress_done(&prog, total);
    }

    if (out >= 0) {
        ag_vfs_close(out);
    }
    conn_close(fd, tls);

    if (err == -AG_EINTR) {
        /* Named the way the operator typed it, not the way the VFS spells it. */
        char shown[AG_PATH_MAX];
        if (r->dest != NULL) {
            ag_shell_dos_path(r->dest, shown, sizeof(shown));
        } else {
            shown[0] = '\0';
        }
        SAY(quiet, "^C - %s holds the %u bytes that arrived\n", shown,
            (unsigned)total);
        return err;
    }
    if (err != AG_OK) {
        SAY(quiet, "the transfer stopped after %u bytes\n", (unsigned)total);
        return err;
    }
    if (resp.have_length && total != resp.length) {
        SAY(quiet, "short: %u of %u bytes\n", (unsigned)total,
            (unsigned)resp.length);
        return -AG_EIO;
    }
    return AG_OK;
}

/* GET or POST, and nothing else: a typo here would go out on the wire. */
static bool method_ok(const char *method)
{
    return method != NULL &&
           (strcmp(method, "GET") == 0 || strcmp(method, "POST") == 0);
}

static ag_err_t http_run(const char *method, const char *url_text,
                         const char *headers, const void *body,
                         uint32_t body_len, const char *dest, uint16_t *status,
                         bool quiet)
{
    if (url_text == NULL || !method_ok(method)) {
        return -AG_EINVAL;
    }
    if (strcmp(method, "GET") == 0 && body_len != 0u) {
        return -AG_EINVAL;
    }
    if (body_len != 0u && body == NULL) {
        return -AG_EINVAL;
    }
    if (status != NULL) {
        *status = 0;
    }

    http_req_t r;
    memset(&r, 0, sizeof(r));
    r.method = method;
    r.headers = headers;
    r.body = body;
    r.body_len = body_len;
    r.dest = dest;
    r.status = status;
    r.quiet = quiet;

    http_bufs_t bufs;
    if (!bufs_alloc(&bufs)) {
        SAYS(quiet, "not enough memory for a transfer\n");
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
         * http here has no login, so a URL that carries a name and a password
         * is not going to use them - and a fetch that answers 401 after being
         * given a password looks like a wrong password.
         */
        if (u.user[0] != '\0') {
            SAYS(quiet, "(the name and password in that URL are ignored: "
                        "http here has no login)\n");
        }
        /*
         * A redirect that changes scheme lands here: ftp:// is a different
         * conversation.  https:// is fine when this build has TLS, and is the
         * common case now that servers redirect http to it.
         */
        bool ok_scheme = (strcmp(u.scheme, "http") == 0);
#if AG_PORT_HAS_TLS
        ok_scheme = ok_scheme || (strcmp(u.scheme, "https") == 0);
#endif
        if (!ok_scheme) {
            SAY(quiet, "%s: not http%s\n", current,
                AG_PORT_HAS_TLS ? " or https" : "");
            err = -AG_ENOTSUP;
            break;
        }

        char next[288];
        err = request_once(&u, &r, &bufs, next, sizeof(next));
        if (err != AG_OK || next[0] == '\0') {
            break;
        }
        if (hop == HTTP_MAX_REDIRECTS) {
            SAYS(quiet, "too many redirects\n");
            err = -AG_EIO;
            break;
        }
        /*
         * A redirected POST stops being one unless the server said 307 or 308:
         * 303 means "look over there instead", and 301/302 are what frameworks
         * answer after a form post.  Sending the body a second time is how a
         * button gets pressed twice.
         */
        if (strcmp(r.method, "POST") == 0 && r.status != NULL &&
            *r.status != 307 && *r.status != 308) {
            r.method = "GET";
            r.body = NULL;
            r.body_len = 0;
        }
        SAY(quiet, "-> %s\n", next);
        memcpy(current, next, sizeof(current));
    }

    bufs_free(&bufs);
    return err;
}

ag_err_t ag_http_fetch(const char *url_text, const char *dest)
{
    if (url_text == NULL || dest == NULL) {
        return -AG_EINVAL;
    }
    return http_run("GET", url_text, NULL, NULL, 0, dest, NULL, false);
}

ag_err_t ag_http_exchange(const char *method, const char *url_text,
                          const char *headers, const void *body,
                          uint32_t body_len, const char *dest,
                          uint16_t *status)
{
    return http_run(method, url_text, headers, body, body_len, dest, status,
                    true);
}

#endif /* CONFIG_ARGON_ENABLE_NET */
