/*
 * ArgonOS - HTTPD.AXE: serve a directory over HTTP, as a loadable application.
 *
 * This used to be a built-in shell command (net/httpd.c).  It is out of the
 * firmware now and lives on disk: a file server is a thing you run when you want
 * it, on a board that has a radio, and nothing about it needs to be in the image
 * the way `dir` and `copy` do.  Same server, same behaviour, same words on the
 * console - it just arrives through the loader instead of the command table, and
 * so it can be replaced without reflashing the kernel.
 *
 *   httpd            serve the current directory on port 80, read only
 *   httpd 8080 c:\   a port and a directory
 *   httpd 80 a:\ /w  and let browsers send and delete files
 *
 * The HTTP parsing/formatting is the kernel's own netmsg, compiled in here too
 * (it is pure string work and wget/ftp still use it in the firmware); the
 * socket timeouts are a small layer over the net ABI, since the kernel's netio
 * reaches the port directly and an application cannot.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/argon.h>
#include <argon/netmsg.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

AG_APP("HTTPD", "1.0", "argon", AG_AXE_NEEDS_NET);

#define HTTPD_HDR_MAX 1024
#define HTTPD_FILE_BUF 1536

#define HTTPD_REQUEST_MS 5000
#define HTTPD_SEND_MS 10000
#define HTTPD_UPLOAD_MS 20000
#define HTTPD_ACCEPT_MS 200
#define NIO_SLICE_MS 20

/*
 * Below this much free memory, the next visitor is turned away at the door.
 * See the long note in the firmware's original: a connection accepted when
 * lwIP or the Wi-Fi driver is out of memory does not fail, it aborts the board.
 * Sixteen kilobytes is measured, not guessed.
 */
#define HTTPD_MEM_FLOOR (16u * 1024u)

typedef struct {
    char    *hdr;  /* HTTPD_HDR_MAX  */
    uint8_t *file; /* HTTPD_FILE_BUF */
    void    *mem;
} httpd_bufs_t;

static size_t sys_free(void)
{
    ag_meminfo_t mi;
    ag_meminfo(&mi);
    return mi.system_free;
}

/* ---------------------------------------------------------------------- */
/* Socket IO, over the net ABI, that cannot wedge the board.              */
/*                                                                        */
/* The kernel's netio waits with a select in the port; an application has  */
/* only send/recv on a non-blocking socket, so the wait is a bounded poll  */
/* here, looked at against a deadline and against Ctrl+C between slices.    */
/* ---------------------------------------------------------------------- */

static ag_err_t send_all(ag_handle_t fd, const void *buf, size_t len,
                         uint32_t timeout_ms)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t         left = len;
    uint32_t       start = ag_millis();

    while (left > 0) {
        const int32_t w = ag_net_send(fd, p, left);
        if (w > 0) {
            p += w;
            left -= (size_t)w;
            start = ag_millis();
            continue;
        }
        if (w == -AG_EAGAIN) {
            if (ag_interrupted()) {
                return -AG_EINTR;
            }
            if (ag_millis() - start >= timeout_ms) {
                return -AG_ETIMEDOUT;
            }
            ag_delay(NIO_SLICE_MS);
            continue;
        }
        return (w == 0) ? -AG_EIO : (ag_err_t)w;
    }
    return AG_OK;
}

/* Up to `len` bytes.  0 at end of stream, negative on error. */
static int32_t recv_some(ag_handle_t fd, void *buf, size_t len,
                         uint32_t timeout_ms)
{
    uint32_t start = ag_millis();
    for (;;) {
        const int32_t n = ag_net_recv(fd, buf, len);
        if (n >= 0) {
            return n;
        }
        if (n == -AG_EAGAIN) {
            if (ag_interrupted()) {
                return -AG_EINTR;
            }
            if (ag_millis() - start >= timeout_ms) {
                return -AG_ETIMEDOUT;
            }
            ag_delay(NIO_SLICE_MS);
            continue;
        }
        return n;
    }
}

/* A formatted control line.  Bounded, and the bound is a real limit: half a
 * table row is a broken page, so a line that does not fit is refused. */
static ag_err_t sendf(ag_handle_t fd, const char *fmt, ...)
{
    char    line[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        return -AG_ERANGE;
    }
    return send_all(fd, line, (size_t)n, HTTPD_SEND_MS);
}

/* A buffered reader: prefill bytes at the front of buf are already received
 * (the body that arrived stuck to the request header); after those, straight
 * from the socket. */
typedef struct {
    ag_handle_t fd;
    uint8_t    *buf;
    size_t      cap;
    size_t      have;
    size_t      pos;
    uint32_t    timeout_ms;
} nio_t;

static void nio_init(nio_t *r, ag_handle_t fd, uint8_t *buf, size_t cap,
                     size_t prefill)
{
    r->fd = fd;
    r->buf = buf;
    r->cap = cap;
    r->have = prefill;
    r->pos = 0;
    r->timeout_ms = HTTPD_UPLOAD_MS;
}

static int32_t nio_read(nio_t *r, void *dst, size_t n)
{
    if (r->pos < r->have) {
        size_t k = r->have - r->pos;
        if (k > n) {
            k = n;
        }
        memcpy(dst, r->buf + r->pos, k);
        r->pos += k;
        return (int32_t)k;
    }
    return recv_some(r->fd, dst, n, r->timeout_ms);
}

/* The request header block, or nothing.  Returns its length. */
static int32_t recv_request(ag_handle_t fd, char *hdr, size_t cap,
                            size_t *end_out)
{
    size_t have = 0;
    for (;;) {
        const int32_t n =
            recv_some(fd, hdr + have, cap - have, HTTPD_REQUEST_MS);
        if (n < 0) {
            return n;
        }
        if (n == 0) {
            return -AG_EIO; /* gone before it asked for anything */
        }
        have += (size_t)n;

        const size_t end = ag_http_header_end(hdr, have);
        if (end != 0) {
            *end_out = end;
            return (int32_t)have;
        }
        if (have == cap) {
            return -AG_ERANGE;
        }
    }
}

static void reply_status(ag_handle_t fd, int status, bool with_body)
{
    const char *text = ag_http_status_text(status);
    char        body[128];

    const int blen =
        snprintf(body, sizeof(body),
                 "<html><body><h2>%d %s</h2></body></html>\r\n", status, text);

    (void)sendf(fd,
                "HTTP/1.1 %d %s\r\nServer: ArgonOS\r\n"
                "Content-Type: text/html\r\nContent-Length: %d\r\n"
                "Connection: close\r\n\r\n",
                status, text, with_body ? blen : 0);
    if (with_body && blen > 0) {
        (void)send_all(fd, body, (size_t)blen, HTTPD_SEND_MS);
    }
}

/* ---------------------------------------------------------------------- */

/* dir + name, joined with a single separator, into out.  -AG_ERANGE if it
 * would not fit.  A trailing slash on dir (a drive root like "a:\" seen as
 * "/") is not doubled. */
static ag_err_t path_join(const char *dir, const char *name, char *out,
                          size_t cap)
{
    const size_t dl = strlen(dir);
    const bool   slash = (dl > 0 && (dir[dl - 1] == '/'));
    const int    n = snprintf(out, cap, "%s%s%s", dir, slash ? "" : "/", name);
    if (n < 0 || (size_t)n >= cap) {
        return -AG_ERANGE;
    }
    return AG_OK;
}

static ag_err_t serve_listing(ag_handle_t fd, const char *path,
                              const char *target, bool head_only, bool writable)
{
    const ag_handle_t dir = ag_opendir(path);
    if (dir < 0) {
        reply_status(fd, 403, true);
        return (ag_err_t)dir;
    }

    ag_err_t err = sendf(fd,
                         "HTTP/1.1 200 OK\r\nServer: ArgonOS\r\n"
                         "Content-Type: text/html; charset=utf-8\r\n"
                         "Connection: close\r\n\r\n");
    if (err == AG_OK && !head_only) {
        static const char k_head[] =
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,"
            "initial-scale=1\"><style>"
            "body{font:16px sans-serif;margin:1em;max-width:44em}"
            "table{border-collapse:collapse;width:100%}"
            "td{padding:.45em .6em;border-bottom:1px solid #ddd}"
            "td.s{text-align:right;color:#666;white-space:nowrap}"
            "button{font:inherit;padding:.2em .7em}"
            "form.u{margin:1em 0;padding:.8em;background:#f2f2f2}"
            "form.d{display:inline;margin:0}"
            "</style><title>";
        err = send_all(fd, k_head, sizeof(k_head) - 1, HTTPD_SEND_MS);
        if (err == AG_OK) {
            err = sendf(fd, "%s", target);
        }
        if (err == AG_OK) {
            err = sendf(fd, "</title></head><body><h2>%s</h2>", target);
        }
    }
    if (err != AG_OK || head_only) {
        ag_closedir(dir);
        return err;
    }

    if (writable) {
        err = sendf(fd,
                    "<form class=\"u\" method=\"post\" "
                    "enctype=\"multipart/form-data\" action=\"\">"
                    "<input type=\"file\" name=\"f\" multiple> "
                    "<button>send</button></form>");
    }
    if (err == AG_OK) {
        err = sendf(fd, "<table>");
    }
    if (err == AG_OK && strcmp(target, "/") != 0) {
        err = sendf(fd, "<tr><td><a href=\"../\">../</a></td>"
                        "<td class=\"s\"></td><td></td></tr>");
    }

    ag_dirent_t ent;
    while (err == AG_OK && ag_readdir(dir, &ent) == AG_OK) {
        const bool is_dir = (ent.st.attr & AG_A_DIR) != 0;

        char href[512];
        if (ag_pct_encode(ent.name, href, sizeof(href)) == 0) {
            continue; /* a name that cannot be linked to is not listed */
        }

        if (is_dir) {
            err = sendf(fd,
                        "<tr><td><a href=\"%s/\">%s/</a></td>"
                        "<td class=\"s\"></td><td></td></tr>",
                        href, ent.name);
            continue;
        }

        err = sendf(fd,
                    "<tr><td><a href=\"%s\">%s</a></td>"
                    "<td class=\"s\">%u</td><td>",
                    href, ent.name, (unsigned)ent.st.size);
        if (err == AG_OK && writable) {
            err = sendf(fd,
                        "<form class=\"d\" method=\"post\" "
                        "action=\"%s\"><input type=\"hidden\" "
                        "name=\"delete\" value=\"1\">"
                        "<button>delete</button></form>",
                        href);
        }
        if (err == AG_OK) {
            err = sendf(fd, "</td></tr>");
        }
        if (ag_interrupted()) {
            break;
        }
    }
    ag_closedir(dir);

    if (err == AG_OK) {
        err = sendf(fd, "</table>%s</body></html>",
                    writable ? "" : "<p>read only</p>");
    }
    return err;
}

/* ---------------------------------------------------------------------- */
/* Taking a file in                                                       */
/* ---------------------------------------------------------------------- */

static int find_bytes(const uint8_t *hay, size_t n, const uint8_t *needle,
                      size_t m)
{
    if (m == 0 || n < m) {
        return -1;
    }
    for (size_t i = 0; i + m <= n; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, m) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * One part's body, to a file or to nowhere.  A sliding window keeps the last
 * delimiter-length bytes back before flushing, because a delimiter can be split
 * across two reads.  `have` is what is in the window on entry and what is left
 * over on exit.
 */
static int64_t recv_part(nio_t *r, const char *path, const uint8_t *delim,
                         size_t dlen, uint8_t *win, size_t cap, size_t *have,
                         bool *last)
{
    ag_handle_t out = -1;
    if (path != NULL) {
        out = ag_open(path, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
        if (out < 0) {
            return (int64_t)out;
        }
    }

    int64_t  written = 0;
    ag_err_t err = AG_OK;
    *last = true;

    for (;;) {
        const int at = find_bytes(win, *have, delim, dlen);
        if (at >= 0) {
            if (at > 0 && out >= 0 &&
                ag_write(out, win, (size_t)at) != (int32_t)at) {
                err = -AG_EIO;
                break;
            }
            written += at;

            size_t after = (size_t)at + dlen;
            while ((*have - after) < 2 && *have < cap) {
                const int32_t n = nio_read(r, win + *have, cap - *have);
                if (n <= 0) {
                    break;
                }
                *have += (size_t)n;
            }
            if ((*have - after) >= 2) {
                *last = (win[after] == '-' && win[after + 1] == '-');
                after += 2;
            }
            memmove(win, win + after, *have - after);
            *have -= after;
            break;
        }

        if (*have > dlen) {
            const size_t flush = *have - (dlen - 1);
            if (out >= 0 && ag_write(out, win, flush) != (int32_t)flush) {
                err = -AG_EIO;
                break;
            }
            written += (int64_t)flush;
            memmove(win, win + flush, *have - flush);
            *have -= flush;
        }

        const int32_t n = nio_read(r, win + *have, cap - *have);
        if (n < 0) {
            err = (ag_err_t)n;
            break;
        }
        if (n == 0) {
            if (*have > 0 && out >= 0) {
                (void)ag_write(out, win, *have);
                written += (int64_t)*have;
                *have = 0;
            }
            err = -AG_EIO;
            break;
        }
        *have += (size_t)n;
    }

    if (out >= 0) {
        ag_close(out);
    }
    return (err != AG_OK) ? (int64_t)err : written;
}

static int serve_upload(ag_handle_t fd, const char *dir, const char *target,
                        const ag_http_req_t *req, httpd_bufs_t *b,
                        const uint8_t *body, size_t body_len)
{
    char boundary[AG_HTTP_BOUNDARY_MAX + 1];
    if (!ag_http_boundary(req->content_type, boundary, sizeof(boundary))) {
        reply_status(fd, 400, true);
        ag_printf("  upload without a boundary -> 400\n");
        return 400;
    }

    uint8_t delim[4 + AG_HTTP_BOUNDARY_MAX + 1];
    delim[0] = '\r';
    delim[1] = '\n';
    delim[2] = '-';
    delim[3] = '-';
    const size_t blen = strlen(boundary);
    memcpy(delim + 4, boundary, blen);
    const size_t dlen = 4 + blen;

    memcpy(b->file, body, body_len);
    nio_t rdr;
    nio_init(&rdr, fd, b->file, HTTPD_FILE_BUF, body_len);

    uint8_t     *win = (uint8_t *)b->hdr;
    const size_t cap = HTTPD_HDR_MAX;
    size_t       have = 2;
    win[0] = '\r';
    win[1] = '\n';

    int      files = 0;
    uint64_t bytes = 0;
    bool     last = false;

    int64_t skipped = recv_part(&rdr, NULL, delim, dlen, win, cap, &have, &last);
    if (skipped < 0) {
        reply_status(fd, 400, true);
        ag_printf("  upload: no first boundary -> 400\n");
        return 400;
    }

    while (!last) {
        size_t hend = 0;
        for (;;) {
            hend = ag_http_header_end((const char *)win, have);
            if (hend != 0 || have == cap) {
                break;
            }
            const int32_t n = nio_read(&rdr, win + have, cap - have);
            if (n <= 0) {
                break;
            }
            have += (size_t)n;
        }
        if (hend == 0) {
            reply_status(fd, 400, true);
            ag_printf("  upload: a part without headers -> 400\n");
            return 400;
        }

        char       name[AG_NAME_MAX + 1];
        const bool named =
            ag_http_part_filename((const char *)win, hend, name, sizeof(name));

        memmove(win, win + hend, have - hend);
        have -= hend;

        char path[AG_PATH_MAX];
        if (named && path_join(dir, name, path, sizeof(path)) != AG_OK) {
            reply_status(fd, 414, true);
            return 414;
        }

        const int64_t n = recv_part(&rdr, named ? path : NULL, delim, dlen, win,
                                    cap, &have, &last);
        if (n < 0) {
            if (n == -AG_ENOMEM) {
                ag_printf("  upload %s: out of memory\n",
                          named ? name : "(field)");
                reply_status(fd, 503, true);
                return 503;
            }
            ag_printf("  upload %s stopped short (%d)\n",
                      named ? name : "(field)", (int)n);
            reply_status(fd, 400, true);
            return 400;
        }
        if (named) {
            files++;
            bytes += (uint64_t)n;
            ag_printf("  + %s, %u bytes\n", name, (unsigned)n);
        }
    }

    if (files == 0) {
        reply_status(fd, 400, true);
        ag_printf("  upload with no file in it -> 400\n");
        return 400;
    }

    (void)sendf(fd,
                "HTTP/1.1 303 See Other\r\nServer: ArgonOS\r\n"
                "Location: %s\r\nContent-Length: 0\r\n"
                "Connection: close\r\n\r\n",
                target);
    ag_printf("  %s -> 303, %d file(s), %u bytes\n", target, files,
              (unsigned)bytes);
    return 303;
}

static ag_err_t serve_file(ag_handle_t fd, const char *path, httpd_bufs_t *b,
                           bool head_only, uint64_t *sent_out)
{
    ag_stat_t st;
    if (ag_stat(path, &st) != AG_OK) {
        reply_status(fd, 404, true);
        return -AG_ENOENT;
    }

    const ag_handle_t in = ag_open(path, AG_O_RDONLY);
    if (in < 0) {
        reply_status(fd, 403, true);
        return (ag_err_t)in;
    }

    ag_err_t err = sendf(fd,
                         "HTTP/1.1 200 OK\r\nServer: ArgonOS\r\n"
                         "Content-Type: %s\r\nContent-Length: %u\r\n"
                         "Connection: close\r\n\r\n",
                         ag_http_mime(path), (unsigned)st.size);
    if (err != AG_OK || head_only) {
        ag_close(in);
        return err;
    }

    int32_t n;
    while ((n = ag_read(in, b->file, HTTPD_FILE_BUF)) > 0) {
        err = send_all(fd, b->file, (size_t)n, HTTPD_SEND_MS);
        if (err != AG_OK) {
            break;
        }
        *sent_out += (uint64_t)n;
        if (ag_interrupted()) {
            err = -AG_EINTR;
            break;
        }
    }
    if (n < 0) {
        err = (ag_err_t)n;
    }
    ag_close(in);
    return err;
}

/* ---------------------------------------------------------------------- */

static void serve_one(ag_handle_t fd, const char *root, httpd_bufs_t *b,
                      bool writable)
{
    size_t        end = 0;
    const int32_t have = recv_request(fd, b->hdr, HTTPD_HDR_MAX, &end);
    if (have < 0) {
        if (have == -AG_ERANGE) {
            reply_status(fd, 414, true);
        }
        return;
    }

    ag_http_req_t req;
    if (ag_http_parse_request(b->hdr, end, &req) != AG_OK) {
        reply_status(fd, 400, true);
        ag_printf("  400 (not a request)\n");
        return;
    }

    const bool is_get = strcmp(req.method, "GET") == 0;
    const bool is_head = strcmp(req.method, "HEAD") == 0;
    const bool is_post = strcmp(req.method, "POST") == 0;
    if (!is_get && !is_head && !is_post) {
        reply_status(fd, 405, true);
        ag_printf("  %s -> 405\n", req.method);
        return;
    }
    if (is_post && !writable) {
        (void)sendf(fd,
                    "HTTP/1.1 405 Method Not Allowed\r\nServer: ArgonOS\r\n"
                    "Allow: GET, HEAD\r\nContent-Type: text/html\r\n"
                    "Content-Length: 46\r\nConnection: close\r\n\r\n"
                    "<html><body>read only (httpd /w)</body></html>");
        ag_printf("  %s -> 405 (read only)\n", req.target);
        return;
    }

    if (!ag_http_target_safe(req.target)) {
        reply_status(fd, 403, true);
        ag_printf("  %s -> 403 (refused)\n", req.target);
        return;
    }

    char       path[AG_PATH_MAX];
    const bool wants_dir = (req.target[strlen(req.target) - 1] == '/');
    const int  pn = (strcmp(root, "/") == 0)
                        ? snprintf(path, sizeof(path), "%s", req.target)
                        : snprintf(path, sizeof(path), "%s%s", root,
                                   req.target);
    if (pn <= 0 || (size_t)pn >= sizeof(path)) {
        reply_status(fd, 414, true);
        return;
    }
    size_t plen = strlen(path);
    while (plen > 1 && path[plen - 1] == '/') {
        path[--plen] = '\0';
    }

    ag_stat_t st;
    if (ag_stat(path, &st) != AG_OK) {
        reply_status(fd, 404, true);
        ag_printf("  %s -> 404\n", req.target);
        return;
    }

    uint64_t sent = 0;
    ag_err_t err = AG_OK;
    int      status = 200;

    if (is_post) {
        const uint8_t *body = (const uint8_t *)b->hdr + end;
        const size_t   body_len = (size_t)have - end;

        if ((st.attr & AG_A_DIR) != 0) {
            (void)serve_upload(fd, path, req.target, &req, b, body, body_len);
            return;
        }

        if (body_len > 0 && body_len < 64 &&
            find_bytes(body, body_len, (const uint8_t *)"delete", 6) >= 0) {
            const ag_err_t derr = ag_unlink(path);
            if (derr != AG_OK) {
                reply_status(fd, 403, true);
                ag_printf("  delete %s -> 403 (%d)\n", req.target, (int)derr);
                return;
            }

            char  up[AG_URL_PATH_MAX + 1];
            snprintf(up, sizeof(up), "%s", req.target);
            char *slash = strrchr(up, '/');
            if (slash != NULL) {
                slash[1] = '\0';
            }
            (void)sendf(fd,
                        "HTTP/1.1 303 See Other\r\n"
                        "Server: ArgonOS\r\nLocation: %s\r\n"
                        "Content-Length: 0\r\nConnection: close\r\n\r\n",
                        up);
            ag_printf("  - %s\n", req.target);
            return;
        }

        reply_status(fd, 400, true);
        ag_printf("  %s -> 400 (post of nothing)\n", req.target);
        return;
    }

    if ((st.attr & AG_A_DIR) != 0) {
        char index[AG_PATH_MAX];
        bool served = false;
        static const char *const names[] = {"index.htm", "index.html"};

        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (path_join(path, names[i], index, sizeof(index)) != AG_OK) {
                continue;
            }
            if (ag_stat(index, &st) == AG_OK && (st.attr & AG_A_DIR) == 0) {
                err = serve_file(fd, index, b, is_head, &sent);
                served = true;
                break;
            }
        }
        if (!served) {
            if (!wants_dir) {
                (void)sendf(fd,
                            "HTTP/1.1 301 Moved Permanently\r\nServer: "
                            "ArgonOS\r\nLocation: %s/\r\nContent-Length: 0\r\n"
                            "Connection: close\r\n\r\n",
                            req.target);
                status = 301;
            } else {
                err = serve_listing(fd, path, req.target, is_head, writable);
            }
        }
    } else {
        err = serve_file(fd, path, b, is_head, &sent);
    }

    if (err == -AG_ENOENT) {
        status = 404;
    } else if (err != AG_OK) {
        status = 0;
    }

    if (status == 0) {
        ag_printf("  %s -> broke off after %u bytes\n", req.target,
                  (unsigned)sent);
    } else if (sent > 0) {
        ag_printf("  %s -> %d, %u bytes\n", req.target, status, (unsigned)sent);
    } else {
        ag_printf("  %s -> %d\n", req.target, status);
    }
}

/* ---------------------------------------------------------------------- */

int ag_main(int argc, char **argv)
{
    uint16_t    port = 80;
    const char *root = "/";
    bool        writable = false;

    /* httpd [port] [directory] [/w] - the same words the built-in took. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "/w") == 0 || strcmp(argv[i], "/W") == 0) {
            writable = true;
        } else if (argv[i][0] >= '0' && argv[i][0] <= '9') {
            long p = 0;
            for (const char *c = argv[i]; *c >= '0' && *c <= '9'; c++) {
                p = p * 10 + (*c - '0');
            }
            if (p <= 0 || p > 65535) {
                ag_printf("usage: httpd [port] [directory] [/w]\n");
                return 1;
            }
            port = (uint16_t)p;
        } else {
            root = argv[i];
        }
    }

    if (!ag_net_is_ready() && ag_net_wait_ready(8000) != AG_OK) {
        ag_printf("no network (is the radio up and joined?)\n");
        return 1;
    }

    httpd_bufs_t b;
    b.mem = ag_malloc(HTTPD_HDR_MAX + HTTPD_FILE_BUF);
    if (b.mem == NULL) {
        ag_printf("not enough memory to serve\n");
        return 1;
    }
    b.hdr = (char *)b.mem;
    b.file = (uint8_t *)b.mem + HTTPD_HDR_MAX;

    const ag_handle_t lfd = ag_tcp_listen(port);
    if (lfd < 0) {
        ag_free(b.mem);
        ag_printf("port %u is not available (%d)\n", (unsigned)port, (int)lfd);
        return 1;
    }

    uint32_t addr = 0;
    char     ip[16] = "0.0.0.0";
    if (ag_net_ifaddr(&addr) == AG_OK) {
        (void)ag_ipv4_str(addr, ip, sizeof(ip));
    }

    ag_printf("serving %s at http://%s:%u/\n", root, ip, (unsigned)port);
    ag_printf("%s\n", writable ? "browsers may send and delete files"
                               : "read only (add /w to accept files)");
    ag_printf("%u KB free; visitors are refused below %u KB\n",
              (unsigned)(sys_free() / 1024u),
              (unsigned)(HTTPD_MEM_FLOOR / 1024u));
    ag_printf("Ctrl+C to stop\n");

    while (!ag_interrupted()) {
        const ag_handle_t cfd = ag_tcp_accept(lfd, HTTPD_ACCEPT_MS);
        if (cfd == -AG_EAGAIN || cfd == -AG_ETIMEDOUT) {
            continue;
        }
        if (cfd < 0) {
            if (ag_interrupted()) {
                break;
            }
            ag_printf("accept: %d\n", (int)cfd);
            break;
        }

        const size_t free_now = sys_free();
        if (free_now < HTTPD_MEM_FLOOR) {
            ag_net_close(cfd);
            ag_printf("  busy: %u KB free, connection refused\n",
                      (unsigned)(free_now / 1024u));
            continue;
        }

        (void)ag_net_set_nonblock(cfd, true);
        serve_one(cfd, root, &b, writable);
        ag_net_close(cfd);
    }

    ag_net_close(lfd);
    ag_free(b.mem);
    ag_printf("stopped\n");
    return 0;
}
