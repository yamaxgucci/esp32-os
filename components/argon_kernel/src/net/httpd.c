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
                              bool head_only)
{
    const ag_handle_t dir = ag_vfs_opendir(path, NULL);
    if (dir < 0) {
        reply_status(fd, 403, true);
        return (ag_err_t)dir;
    }

    ag_err_t err = ag_netio_sendf(fd,
                                  "HTTP/1.1 200 OK\r\nServer: ArgonOS\r\n"
                                  "Content-Type: text/html\r\n"
                                  "Connection: close\r\n\r\n");
    if (err == AG_OK && !head_only) {
        err = ag_netio_sendf(fd,
                             "<html><head><title>%s</title></head><body>"
                             "<h2>%s</h2><pre>\r\n",
                             target, target);
    }
    if (err != AG_OK || head_only) {
        ag_vfs_closedir(dir);
        return err;
    }

    if (strcmp(target, "/") != 0) {
        err = ag_netio_sendf(fd, "<a href=\"../\">../</a>\r\n");
    }

    ag_dirent_t ent;
    while (err == AG_OK && ag_vfs_readdir(dir, &ent) == AG_OK) {
        const bool is_dir = (ent.st.attr & AG_A_DIR) != 0;

        char href[512];
        if (ag_pct_encode(ent.name, href, sizeof(href)) == 0) {
            continue; /* a name that cannot be linked to is not listed */
        }

        if (is_dir) {
            err = ag_netio_sendf(fd, "<a href=\"%s/\">%s/</a>\r\n", href,
                                 ent.name);
        } else {
            err = ag_netio_sendf(fd, "<a href=\"%s\">%s</a>  %u\r\n", href,
                                 ent.name, (unsigned)ent.st.size);
        }
        if (ag_shell_interrupted()) {
            break;
        }
    }
    ag_vfs_closedir(dir);

    if (err == AG_OK) {
        err = ag_netio_sendf(fd, "</pre></body></html>\r\n");
    }
    return err;
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
static void serve_one(int fd, const char *root, httpd_bufs_t *b)
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
    if (!is_get && !is_head) {
        reply_status(fd, 405, true);
        ag_console_printf("  %s -> 405\n", req.method);
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
                err = serve_listing(fd, path, req.target, is_head);
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

ag_err_t ag_httpd_run(uint16_t port, const char *root)
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
        serve_one(cfd, root, &b);
        ag_port_net_close(cfd);
    }

    ag_port_net_close(lfd);
    ag_port_free(b.mem);
    ag_console_puts("stopped\n");
    ag_shell_clear_interrupted();
    return AG_OK;
}

#endif /* CONFIG_ARGON_ENABLE_NET */
