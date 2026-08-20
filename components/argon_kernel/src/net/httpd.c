/*
 * ArgonOS - serving the card over HTTP.
 *
 * A file server for a machine that has one processor, no threads to spare and
 * about fifty kilobytes of memory free.  So: one connection at a time, one
 * request per connection, and no keep-alive.  That is not a shortcut to be
 * fixed later - with a single accept loop, a kept-open connection is a client
 * holding the whole server while it thinks, and a browser holds six of them.
 * Closing after each reply is what makes the second visitor's page arrive.
 *
 * It runs in the foreground and stops on Ctrl+C, like every other command
 * here.  A daemon would need a task, a stack, and a policy for what happens to
 * it when an application takes the machine; a command needs none of that, and
 * this is a system where the operator is present.
 *
 * Two things it will not do:
 *
 * - Serve anything outside the directory it was given.  The target is decoded,
 *   then judged (ag_http_target_safe), then joined - in that order, because
 *   "%2e%2e" is ".." and only the decoded form can be judged.
 * - Accept a request larger than one kilobyte of headers.  A request that big
 *   is not from a person, and answering it would mean holding it.
 *
 * Writing is off unless it is asked for (`httpd 80 a:\ /w`).  With it on the
 * page carries a file picker and a delete button per row, and that is the whole
 * feature: a phone and a laptop that cannot see each other's disks can both see
 * this card.  Off is the default because a server that takes files from anyone
 * who can reach it is a decision, not a convenience - there is no password
 * here, and the network is whatever network the board is on.
 *
 * Both are ordinary HTML forms.  No JavaScript: a file picker and a submit
 * button are older than this project and work in every browser a phone has ever
 * had.  The cost is that the server has to read multipart/form-data, which is
 * done streaming - the body is as large as the file, and the file is the reason
 * somebody bought a card.
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
#include <argon/path.h>
#include <argon/shell.h>
#include <argon/vfs.h>

#include <argon/port/mem.h>
#include <argon/port/net.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

#include "net/netio.h"

#define HTTPD_HDR_MAX 1024
#define HTTPD_FILE_BUF 1536

/* How long a client is given to send its request and to take a reply.  Short
 * on purpose: this server can only talk to one of them at a time. */
#define HTTPD_REQUEST_MS 5000
#define HTTPD_SEND_MS 10000
/* A body comes from a phone over a radio; it is allowed to take longer than a
 * request header, and a pause in the middle of a photo is not a failure. */
#define HTTPD_UPLOAD_MS 20000

/* Long enough that Ctrl+C is felt at once, short enough that the loop is idle
 * between visitors. */
#define HTTPD_ACCEPT_MS 200

typedef struct {
    char    *hdr;  /* HTTPD_HDR_MAX                                       */
    uint8_t *file; /* HTTPD_FILE_BUF                                      */
    void    *mem;
} httpd_bufs_t;

/* ---------------------------------------------------------------------- */

/*
 * Send and receive that cannot wedge the board: the socket is non-blocking and
 * netio waits against a deadline, so a client that stops reading halfway
 * through a file costs this server a timeout rather than a reset.  The port's
 * own timeouts would not do anyway - they are set at connect time, and this
 * socket arrived from accept.
 */
static ag_err_t send_all(int fd, const void *buf, size_t len)
{
    return ag_netio_send(fd, buf, len, HTTPD_SEND_MS);
}

/* The request header block, or nothing.  Returns its length. */
static int32_t recv_request(int fd, char *hdr, size_t cap, size_t *end_out)
{
    size_t have = 0;

    for (;;) {
        const int32_t n =
            ag_netio_recv(fd, hdr + have, cap - have, HTTPD_REQUEST_MS);
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

static void reply_status(int fd, int status, bool with_body)
{
    const char *text = ag_http_status_text(status);
    char        body[128];

    const int blen =
        snprintf(body, sizeof(body),
                 "<html><body><h2>%d %s</h2></body></html>\r\n", status, text);

    (void)ag_netio_sendf(fd,
                         "HTTP/1.1 %d %s\r\nServer: ArgonOS\r\n"
                         "Content-Type: text/html\r\nContent-Length: %d\r\n"
                         "Connection: close\r\n\r\n",
                         status, text, with_body ? blen : 0);
    if (with_body && blen > 0) {
        (void)send_all(fd, body, (size_t)blen);
    }
}

/* ---------------------------------------------------------------------- */

/*
 * A directory as a page.
 *
 * No Content-Length: the listing is written as it is read, and buffering it to
 * count the bytes would mean holding a directory of unknown size in memory on
 * a machine that has none to spare.  Connection: close is what tells the
 * client where the body ends - which is exactly what HTTP/1.0 did.
 */
static ag_err_t serve_listing(int fd, const char *path, const char *target,
                              bool head_only, bool writable)
{
    const ag_handle_t dir = ag_vfs_opendir(path, NULL);
    if (dir < 0) {
        reply_status(fd, 403, true);
        return (ag_err_t)dir;
    }

    ag_err_t err = ag_netio_sendf(fd,
                                  "HTTP/1.1 200 OK\r\nServer: ArgonOS\r\n"
                                  "Content-Type: text/html; charset=utf-8\r\n"
                                  "Connection: close\r\n\r\n");
    if (err == AG_OK && !head_only) {
        /*
         * Sent as a constant rather than formatted: it is longer than one
         * formatted line may be, and there is nothing in it to fill in.  The
         * viewport line is the difference between a page a phone can use and
         * one it lays out at desktop width and then shrinks to nothing.
         */
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
        err = send_all(fd, k_head, sizeof(k_head) - 1);
        if (err == AG_OK) {
            err = ag_netio_sendf(fd, "%s", target);
        }
        if (err == AG_OK) {
            err = ag_netio_sendf(fd, "</title></head><body><h2>%s</h2>",
                                 target);
        }
    }
    if (err != AG_OK || head_only) {
        ag_vfs_closedir(dir);
        return err;
    }

    if (writable) {
        err = ag_netio_sendf(fd,
                             "<form class=\"u\" method=\"post\" "
                             "enctype=\"multipart/form-data\" action=\"\">"
                             "<input type=\"file\" name=\"f\" multiple> "
                             "<button>send</button></form>");
    }
    if (err == AG_OK) {
        err = ag_netio_sendf(fd, "<table>");
    }
    if (err == AG_OK && strcmp(target, "/") != 0) {
        err = ag_netio_sendf(fd, "<tr><td><a href=\"../\">../</a></td>"
                                 "<td class=\"s\"></td><td></td></tr>");
    }

    ag_dirent_t ent;
    while (err == AG_OK && ag_vfs_readdir(dir, &ent) == AG_OK) {
        const bool is_dir = (ent.st.attr & AG_A_DIR) != 0;

        char href[512];
        if (ag_pct_encode(ent.name, href, sizeof(href)) == 0) {
            continue; /* a name that cannot be linked to is not listed */
        }

        if (is_dir) {
            err = ag_netio_sendf(fd,
                                 "<tr><td><a href=\"%s/\">%s/</a></td>"
                                 "<td class=\"s\"></td><td></td></tr>",
                                 href, ent.name);
            continue;
        }

        err = ag_netio_sendf(fd,
                             "<tr><td><a href=\"%s\">%s</a></td>"
                             "<td class=\"s\">%u</td><td>",
                             href, ent.name, (unsigned)ent.st.size);
        if (err == AG_OK && writable) {
            /*
             * A form rather than a link, and that is not decoration: a link is
             * something a browser may follow by itself - to preview it, to
             * warm its cache - and this one deletes a file.
             */
            err = ag_netio_sendf(fd,
                                 "<form class=\"d\" method=\"post\" "
                                 "action=\"%s\"><input type=\"hidden\" "
                                 "name=\"delete\" value=\"1\">"
                                 "<button>delete</button></form>",
                                 href);
        }
        if (err == AG_OK) {
            err = ag_netio_sendf(fd, "</td></tr>");
        }
        if (ag_shell_interrupted()) {
            break;
        }
    }
    ag_vfs_closedir(dir);

    if (err == AG_OK) {
        err = ag_netio_sendf(fd, "</table>%s</body></html>",
                             writable ? "" : "<p>read only</p>");
    }
    return err;
}

/* ---------------------------------------------------------------------- */
/* Taking a file in                                                       */
/* ---------------------------------------------------------------------- */

/* Where in `hay` (n bytes) `needle` (m bytes) starts, or -1. */
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
 * One part's body, to a file or to nowhere.
 *
 * Everything up to the next delimiter is the file.  Written with a sliding
 * window rather than by reading the body first: the body is as big as the file.
 * The window keeps the last delimiter-length bytes back before flushing,
 * because a delimiter can be split across two reads, and a file that lost four
 * bytes at a block boundary is a file that is quietly wrong.
 *
 * `have` is what is already in the window on entry and what is left over on
 * exit - the next part starts there.
 */
static int64_t recv_part(ag_netio_t *r, const char *path, const uint8_t *delim,
                         size_t dlen, uint8_t *win, size_t cap, size_t *have,
                         bool *last)
{
    ag_handle_t out = -1;
    if (path != NULL) {
        out = ag_vfs_open(path, NULL, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
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
                ag_vfs_write(out, win, (size_t)at) != (int32_t)at) {
                err = -AG_EIO;
                break;
            }
            written += at;

            /* Two bytes after the delimiter say whether the body ends here:
             * "--" is the last one, CRLF means another part follows. */
            size_t after = (size_t)at + dlen;
            while ((*have - after) < 2 && *have < cap) {
                const int32_t n = ag_netio_read(r, win + *have, cap - *have);
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

        /* No delimiter in sight: all but the tail is certainly file. */
        if (*have > dlen) {
            const size_t flush = *have - (dlen - 1);
            if (out >= 0 &&
                ag_vfs_write(out, win, flush) != (int32_t)flush) {
                err = -AG_EIO;
                break;
            }
            written += (int64_t)flush;
            memmove(win, win + flush, *have - flush);
            *have -= flush;
        }

        const int32_t n = ag_netio_read(r, win + *have, cap - *have);
        if (n < 0) {
            err = (ag_err_t)n;
            break;
        }
        if (n == 0) {
            /* The client stopped mid-file.  What arrived is written, and the
             * caller says so rather than pretending the file is complete. */
            if (*have > 0 && out >= 0) {
                (void)ag_vfs_write(out, win, *have);
                written += (int64_t)*have;
                *have = 0;
            }
            err = -AG_EIO;
            break;
        }
        *have += (size_t)n;
    }

    if (out >= 0) {
        ag_vfs_close(out);
    }
    return (err != AG_OK) ? (int64_t)err : written;
}

/*
 * A whole multipart body: every part with a filename becomes a file in `dir`,
 * everything else is read and thrown away (a form field this server has no use
 * for is not a reason to refuse the upload).
 */
static int serve_upload(int fd, const char *dir, const char *target,
                        const ag_http_req_t *req, httpd_bufs_t *b,
                        const uint8_t *body, size_t body_len)
{
    char boundary[AG_HTTP_BOUNDARY_MAX + 1];
    if (!ag_http_boundary(req->content_type, boundary, sizeof(boundary))) {
        reply_status(fd, 400, true);
        ag_console_puts("  upload without a boundary -> 400\n");
        return 400;
    }

    /*
     * The delimiter carries the CRLF that ends the part before it.  The very
     * first one has no part before it, so the window is seeded with a CRLF and
     * every delimiter then looks the same - which is one loop instead of two.
     */
    uint8_t delim[4 + AG_HTTP_BOUNDARY_MAX + 1];
    delim[0] = '\r';
    delim[1] = '\n';
    delim[2] = '-';
    delim[3] = '-';
    const size_t blen = strlen(boundary);
    memcpy(delim + 4, boundary, blen);
    const size_t dlen = 4 + blen;

    /* The reader owns the file buffer and starts with whatever arrived stuck
     * to the back of the request header. */
    memcpy(b->file, body, body_len);
    ag_netio_t rdr;
    ag_netio_init(&rdr, fd, b->file, HTTPD_FILE_BUF, body_len);
    rdr.timeout_ms = HTTPD_UPLOAD_MS;

    /* The window is the header buffer, which has done its job by now. */
    uint8_t     *win = (uint8_t *)b->hdr;
    const size_t cap = HTTPD_HDR_MAX;
    size_t       have = 2;
    win[0] = '\r';
    win[1] = '\n';

    int      files = 0;
    uint64_t bytes = 0;
    bool     last = false;

    /* Everything before the first delimiter is preamble, and goes nowhere. */
    int64_t skipped = recv_part(&rdr, NULL, delim, dlen, win, cap, &have,
                                &last);
    if (skipped < 0) {
        reply_status(fd, 400, true);
        ag_console_puts("  upload: no first boundary -> 400\n");
        return 400;
    }

    while (!last) {
        /* This part's headers, up to the blank line. */
        size_t hend = 0;
        for (;;) {
            hend = ag_http_header_end((const char *)win, have);
            if (hend != 0 || have == cap) {
                break;
            }
            const int32_t n = ag_netio_read(&rdr, win + have, cap - have);
            if (n <= 0) {
                break;
            }
            have += (size_t)n;
        }
        if (hend == 0) {
            reply_status(fd, 400, true);
            ag_console_puts("  upload: a part without headers -> 400\n");
            return 400;
        }

        char       name[AG_NAME_MAX + 1];
        const bool named =
            ag_http_part_filename((const char *)win, hend, name, sizeof(name));

        memmove(win, win + hend, have - hend);
        have -= hend;

        char path[AG_PATH_MAX];
        if (named && ag_path_join(dir, name, path, sizeof(path)) != AG_OK) {
            reply_status(fd, 414, true);
            return 414;
        }

        const int64_t n = recv_part(&rdr, named ? path : NULL, delim, dlen, win,
                                    cap, &have, &last);
        if (n < 0) {
            /*
             * Half a file is on the card and the operator should hear it - and
             * hear *why*, because "out of memory" is the one that means the
             * board is being asked for more connections than it has, and no
             * amount of retrying will change that.
             */
            if (n == -AG_ENOMEM) {
                ag_console_printf("  upload %s: out of memory\n",
                                  named ? name : "(field)");
                reply_status(fd, 503, true);
                return 503;
            }
            ag_console_printf("  upload %s stopped short (%d)\n",
                              named ? name : "(field)", (int)n);
            reply_status(fd, 400, true);
            return 400;
        }
        if (named) {
            files++;
            bytes += (uint64_t)n;
            ag_console_printf("  + %s, %u bytes\n", name, (unsigned)n);
        }
    }

    if (files == 0) {
        reply_status(fd, 400, true);
        ag_console_puts("  upload with no file in it -> 400\n");
        return 400;
    }

    /*
     * See Other, so that the browser asks for the listing with a GET.  A page
     * that answered the POST directly would be re-posted by every reload, and
     * the reload button is the first thing anybody presses.
     */
    (void)ag_netio_sendf(fd,
                         "HTTP/1.1 303 See Other\r\nServer: ArgonOS\r\n"
                         "Location: %s\r\nContent-Length: 0\r\n"
                         "Connection: close\r\n\r\n",
                         target);
    ag_console_printf("  %s -> 303, %d file(s), %u bytes\n", target, files,
                      (unsigned)bytes);
    return 303;
}

static ag_err_t serve_file(int fd, const char *path, httpd_bufs_t *b,
                           bool head_only, uint64_t *sent_out)
{
    ag_stat_t st;
    if (ag_vfs_stat(path, NULL, &st) != AG_OK) {
        reply_status(fd, 404, true);
        return -AG_ENOENT;
    }

    const ag_handle_t in = ag_vfs_open(path, NULL, AG_O_RDONLY);
    if (in < 0) {
        reply_status(fd, 403, true);
        return (ag_err_t)in;
    }

    ag_err_t err = ag_netio_sendf(fd,
                                  "HTTP/1.1 200 OK\r\nServer: ArgonOS\r\n"
                                  "Content-Type: %s\r\nContent-Length: %u\r\n"
                                  "Connection: close\r\n\r\n",
                                  ag_http_mime(path), (unsigned)st.size);
    if (err != AG_OK || head_only) {
        ag_vfs_close(in);
        return err;
    }

    int32_t n;
    while ((n = ag_vfs_read(in, b->file, HTTPD_FILE_BUF)) > 0) {
        err = send_all(fd, b->file, (size_t)n);
        if (err != AG_OK) {
            break;
        }
        *sent_out += (uint64_t)n;
        if (ag_shell_interrupted()) {
            err = -AG_EINTR;
            break;
        }
    }
    if (n < 0) {
        err = (ag_err_t)n;
    }
    ag_vfs_close(in);
    return err;
}

/* ---------------------------------------------------------------------- */

/* One request, then the connection is closed.  See the file's header. */
static void serve_one(int fd, const char *root, httpd_bufs_t *b, bool writable)
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
        ag_console_puts("  400 (not a request)\n");
        return;
    }

    const bool is_get = strcmp(req.method, "GET") == 0;
    const bool is_head = strcmp(req.method, "HEAD") == 0;
    const bool is_post = strcmp(req.method, "POST") == 0;
    if (!is_get && !is_head && !is_post) {
        reply_status(fd, 405, true);
        ag_console_printf("  %s -> 405\n", req.method);
        return;
    }
    if (is_post && !writable) {
        /*
         * Not 403: nothing about this request was wrong, the server was simply
         * not started with `/w`.  405 with the methods it does have is what
         * tells a person which of the two to change.
         */
        (void)ag_netio_sendf(
            fd,
            "HTTP/1.1 405 Method Not Allowed\r\nServer: ArgonOS\r\n"
            "Allow: GET, HEAD\r\nContent-Type: text/html\r\n"
            "Content-Length: 46\r\nConnection: close\r\n\r\n"
            "<html><body>read only (httpd /w)</body></html>");
        ag_console_printf("  %s -> 405 (read only)\n", req.target);
        return;
    }

    if (!ag_http_target_safe(req.target)) {
        reply_status(fd, 403, true);
        ag_console_printf("  %s -> 403 (refused)\n", req.target);
        return;
    }

    /* The target, joined to the root it is not allowed to leave. */
    char path[AG_PATH_MAX];
    const bool wants_dir =
        (req.target[strlen(req.target) - 1] == '/'); /* target is never "" */
    const int pn = (strcmp(root, "/") == 0)
                       ? snprintf(path, sizeof(path), "%s", req.target)
                       : snprintf(path, sizeof(path), "%s%s", root,
                                  req.target);
    if (pn <= 0 || (size_t)pn >= sizeof(path)) {
        reply_status(fd, 414, true);
        return;
    }
    /* "/dir/" and "/dir" are the same directory to the filesystem. */
    size_t plen = strlen(path);
    while (plen > 1 && path[plen - 1] == '/') {
        path[--plen] = '\0';
    }

    ag_stat_t st;
    if (ag_vfs_stat(path, NULL, &st) != AG_OK) {
        reply_status(fd, 404, true);
        ag_console_printf("  %s -> 404\n", req.target);
        return;
    }

    uint64_t sent = 0;
    ag_err_t err = AG_OK;
    int      status = 200;

    if (is_post) {
        const uint8_t *body = (const uint8_t *)b->hdr + end;
        const size_t   body_len = (size_t)have - end;

        if ((st.attr & AG_A_DIR) != 0) {
            /* A file arriving into the directory it was posted to. */
            (void)serve_upload(fd, path, req.target, &req, b, body, body_len);
            return;
        }

        /*
         * A file, posted to with "delete=1", goes away.  The body is a form
         * field and fits in what already arrived; a POST to a file that says
         * anything else is not something this server does.
         */
        /*
         * Bounded, because the buffer is not a string: it holds whatever the
         * client sent and nothing put a terminator after it.  strstr here read
         * past the end of the allocation for as long as it took to find a zero
         * byte - which is a fault waiting for the wrong heap layout.
         */
        if (body_len > 0 && body_len < 64 &&
            find_bytes(body, body_len, (const uint8_t *)"delete", 6) >= 0) {
            const ag_err_t derr = ag_vfs_unlink(path, NULL);
            if (derr != AG_OK) {
                reply_status(fd, 403, true);
                ag_console_printf("  delete %s -> 403 (%d)\n", req.target,
                                  (int)derr);
                return;
            }

            /* Back to the directory the file was in, by See Other for the
             * same reason the upload uses it: a reload must not repeat this. */
            char up[AG_URL_PATH_MAX + 1];
            snprintf(up, sizeof(up), "%s", req.target);
            char *slash = strrchr(up, '/');
            if (slash != NULL) {
                slash[1] = '\0';
            }
            (void)ag_netio_sendf(fd,
                                 "HTTP/1.1 303 See Other\r\n"
                                 "Server: ArgonOS\r\nLocation: %s\r\n"
                                 "Content-Length: 0\r\nConnection: close"
                                 "\r\n\r\n",
                                 up);
            ag_console_printf("  - %s\n", req.target);
            return;
        }

        reply_status(fd, 400, true);
        ag_console_printf("  %s -> 400 (post of nothing)\n", req.target);
        return;
    }

    if ((st.attr & AG_A_DIR) != 0) {
        /*
         * A directory answers with its index page when it has one, because
         * that is what a browser asking for "/" expects, and with a listing
         * otherwise, because that is what makes the card browsable at all.
         */
        char index[AG_PATH_MAX];
        bool served = false;
        static const char *const names[] = {"index.htm", "index.html"};

        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (ag_path_join(path, names[i], index, sizeof(index)) != AG_OK) {
                continue;
            }
            if (ag_vfs_stat(index, NULL, &st) == AG_OK &&
                (st.attr & AG_A_DIR) == 0) {
                err = serve_file(fd, index, b, is_head, &sent);
                served = true;
                break;
            }
        }
        if (!served) {
            /*
             * Without the trailing slash, every link on the page would be
             * resolved against the parent - so the client is sent round once
             * to ask again with it.
             */
            if (!wants_dir) {
                (void)ag_netio_sendf(
                    fd,
                    "HTTP/1.1 301 Moved Permanently\r\nServer: ArgonOS\r\n"
                    "Location: %s/\r\nContent-Length: 0\r\n"
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
        status = 0; /* the reply was cut short; there is no status to claim */
    }

    if (status == 0) {
        ag_console_printf("  %s -> broke off after %u bytes\n", req.target,
                          (unsigned)sent);
    } else if (sent > 0) {
        ag_console_printf("  %s -> %d, %u bytes\n", req.target, status,
                          (unsigned)sent);
    } else {
        ag_console_printf("  %s -> %d\n", req.target, status);
    }
}

ag_err_t ag_httpd_run(uint16_t port, const char *root, bool writable)
{
    httpd_bufs_t b;
    b.mem = ag_port_alloc(HTTPD_HDR_MAX + HTTPD_FILE_BUF,
                          AG_MEM_FAST | AG_MEM_BYTE);
    if (b.mem == NULL) {
        ag_console_puts("not enough memory to serve\n");
        return -AG_ENOMEM;
    }
    b.hdr = (char *)b.mem;
    b.file = (uint8_t *)b.mem + HTTPD_HDR_MAX;

    const int lfd = ag_port_net_listen(port);
    if (lfd < 0) {
        ag_port_free(b.mem);
        ag_console_printf("port %u is not available (%d)\n", (unsigned)port,
                          (int)lfd);
        return (ag_err_t)lfd;
    }

    uint32_t addr = 0;
    char     ip[16] = "0.0.0.0";
    if (ag_port_net_ifaddr(&addr) == AG_OK) {
        (void)ag_ipv4_str(addr, ip, sizeof(ip));
    }

    char shown[AG_PATH_MAX];
    ag_shell_dos_path(root, shown, sizeof(shown));
    ag_console_printf("serving %s at http://%s:%u/\n", shown, ip,
                      (unsigned)port);
    ag_console_printf("%s\n", writable ? "browsers may send and delete files"
                                        : "read only (add /w to accept files)");
    ag_console_puts("Ctrl+C to stop\n");

    ag_shell_clear_interrupted();

    while (!ag_shell_interrupted()) {
        const int cfd = ag_port_net_accept(lfd, HTTPD_ACCEPT_MS);
        if (cfd == -AG_EAGAIN || cfd == -AG_ETIMEDOUT) {
            continue;
        }
        if (cfd < 0) {
            ag_console_printf("accept: %d\n", cfd);
            break;
        }

        /* Non-blocking for the reasons in send_all above. */
        (void)ag_port_net_nonblock(cfd, true);
        serve_one(cfd, root, &b, writable);
        ag_port_net_close(cfd);
    }

    ag_port_net_close(lfd);
    ag_port_free(b.mem);
    ag_console_puts("stopped\n");
    ag_shell_clear_interrupted();
    return AG_OK;
}

#endif /* CONFIG_ARGON_ENABLE_NET */
