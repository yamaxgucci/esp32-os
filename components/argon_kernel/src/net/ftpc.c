/*
 * ArgonOS - an FTP client.
 *
 * FTP is two connections: one where the conversation happens and one per
 * transfer, and everything awkward about it comes from that.  This client is
 * passive only - it never listens - because a board behind a router cannot be
 * connected *to*, and PORT would be a feature that works on the bench and
 * nowhere else.
 *
 * The one deliberate deviation from the letter of RFC 959: the address in a
 * 227 reply is read and then not used.  The data connection goes to the same
 * machine the control connection is talking to, with the port the server named.
 *
 * That is not a workaround for one server, it is the only choice that is right
 * twice: a server behind NAT truthfully reports an address that cannot be
 * reached from here (its own, on the far side), and a server that names some
 * *other* machine's address is asking this board to open a connection to a
 * third party on its behalf - which is the FTP bounce, and is not a feature.
 * Either way the answer is the same: talk to the server we are already talking
 * to.
 *
 * Transfers are binary, always.  ASCII mode exists to translate line endings
 * between systems that disagree about them, and this system has a text editor
 * that reads both - so the mode that can corrupt an executable buys nothing.
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

#include "net/netio.h"

#define FTP_CTL_BUF 512
#define FTP_DATA_BUF 1536
#define FTP_CONNECT_MS 15000
#define FTP_LINE_MAX 224

typedef struct {
    int        ctl;
    uint32_t   host_addr; /* what we connected to; where data goes too    */
    ag_netio_t rdr;
    uint8_t   *ctlbuf;
    uint8_t   *data;
    void      *mem;
    char       reply[FTP_LINE_MAX];
    const char *cwd; /* for local file names                             */
} ftp_t;

/* ---------------------------------------------------------------------- */
/* The control conversation                                               */
/* ---------------------------------------------------------------------- */

/*
 * Reads one reply, which may be many lines, and returns its code.
 *
 * A multi-line reply starts "220-" and ends with a line carrying the same code
 * and a space.  Lines in between may look like anything at all - servers put
 * banners, licence text and drawings in there - so anything that is not a
 * final line is printed and skipped.  Getting this wrong does not fail here:
 * it fails one command later, when the tail of the banner is read as the
 * answer to something else.
 */
static int ftp_reply(ftp_t *f)
{
    for (;;) {
        const ag_err_t err = ag_netio_line(&f->rdr, f->reply, sizeof(f->reply));
        if (err == -AG_EIO) {
            ag_console_puts("the server closed the connection\n");
            return -1;
        }
        if (err != AG_OK && err != -AG_ERANGE) {
            return -1;
        }

        ag_console_printf("%s\n", f->reply);

        bool      final = false;
        const int code = ag_ftp_reply_code(f->reply, &final);
        if (code > 0 && final) {
            return code;
        }
    }
}

static int ftp_command(ftp_t *f, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * Sends one command and echoes it.
 *
 * The echo is not decoration: FTP is a conversation, and a transcript with the
 * server's half of it and not ours is unreadable when something goes wrong -
 * which with FTP is often, and always at the far end.
 */
static ag_err_t ftp_send(ftp_t *f, const char *line, size_t len)
{
    /* Echoed without its terminator - and a password is never echoed at all. */
    size_t shown = len;
    while (shown > 0 && (line[shown - 1] == '\r' || line[shown - 1] == '\n')) {
        shown--;
    }

    const bool is_pass = (shown >= 4) && (line[0] == 'P' || line[0] == 'p') &&
                         (line[1] == 'A' || line[1] == 'a') &&
                         (line[2] == 'S' || line[2] == 's') &&
                         (line[3] == 'S' || line[3] == 's');
    if (is_pass) {
        ag_console_puts("> PASS ****\n");
    } else {
        ag_console_printf("> %.*s\n", (int)shown, line);
    }
    return ag_netio_send_all(f->ctl, line, len);
}

static int ftp_command(ftp_t *f, const char *fmt, ...)
{
    char    line[FTP_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(line)) {
        return -1;
    }
    if (ftp_send(f, line, (size_t)n) != AG_OK) {
        return -1;
    }
    return ftp_reply(f);
}

/*
 * A data connection, ready to read or write.
 *
 * PASV first, then the transfer command: the order matters, because the server
 * starts listening when it answers PASV and expects the connection while the
 * transfer command is in flight.
 */
static int ftp_data_open(ftp_t *f)
{
    if (ftp_send(f, "PASV\r\n", 6) != AG_OK) {
        return -1;
    }
    if (ftp_reply(f) != 227) {
        return -1;
    }

    uint32_t addr = 0;
    uint16_t port = 0;
    if (ag_ftp_pasv_parse(f->reply, &addr, &port) != AG_OK) {
        ag_console_puts("the passive reply had no address in it\n");
        return -1;
    }

    /* See the file header: the port is the server's, the address is ours. */
    if (addr != f->host_addr) {
        char named[16];
        (void)ag_ipv4_str(addr, named, sizeof(named));
        ag_console_printf("(server named %s; using the control address)\n",
                          named);
    }

    const int dfd = ag_port_net_connect(f->host_addr, port, FTP_CONNECT_MS);
    if (dfd < 0) {
        ag_console_printf("data connection refused on port %u\n",
                          (unsigned)port);
    }
    return dfd;
}

/* ---------------------------------------------------------------------- */
/* Transfers                                                              */
/* ---------------------------------------------------------------------- */

/* SIZE, when the server has it: only worth asking so the progress line can
 * show a percentage.  A server that refuses is not a problem. */
static uint64_t ftp_size(ftp_t *f, const char *remote)
{
    char line[FTP_LINE_MAX];
    const int n = snprintf(line, sizeof(line), "SIZE %s\r\n", remote);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        return 0;
    }
    if (ftp_send(f, line, (size_t)n) != AG_OK) {
        return 0;
    }
    /* The reply is printed by ftp_reply like any other; a 550 here is normal. */
    if (ftp_reply(f) != 213) {
        return 0;
    }

    uint64_t size = 0;
    return (ag_ftp_size_parse(f->reply, &size) == AG_OK) ? size : 0;
}

static ag_err_t ftp_get(ftp_t *f, const char *remote, const char *dest)
{
    const uint64_t total = ftp_size(f, remote);

    const int dfd = ftp_data_open(f);
    if (dfd < 0) {
        return -AG_EIO;
    }

    /*
     * RETR before the file is opened, and the file opened only once the server
     * has said 150: a refused transfer must not have truncated the local copy
     * of the same file.
     */
    char cmd[FTP_LINE_MAX];
    const int n = snprintf(cmd, sizeof(cmd), "RETR %s\r\n", remote);
    if (n < 0 || (size_t)n >= sizeof(cmd) ||
        ftp_send(f, cmd, (size_t)n) != AG_OK) {
        ag_port_net_close(dfd);
        return -AG_EIO;
    }

    const int code = ftp_reply(f);
    if (code != 150 && code != 125) {
        ag_port_net_close(dfd);
        return (code == 550) ? -AG_ENOENT : -AG_EIO;
    }

    const ag_handle_t out =
        ag_vfs_open(dest, NULL, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (out < 0) {
        ag_console_printf("%s: cannot be written\n", dest);
        ag_port_net_close(dfd);
        (void)ftp_reply(f);
        return (ag_err_t)out;
    }

    ag_progress_t prog;
    ag_progress_start(&prog, total);

    uint64_t got = 0;
    ag_err_t err = AG_OK;
    for (;;) {
        const int32_t got_now = ag_port_net_recv(dfd, f->data, FTP_DATA_BUF);
        if (got_now < 0) {
            err = (ag_err_t)got_now;
            break;
        }
        if (got_now == 0) {
            break; /* the server closes the data connection at the end */
        }

        size_t off = 0;
        while (off < (size_t)got_now) {
            const int32_t w =
                ag_vfs_write(out, f->data + off, (size_t)got_now - off);
            if (w <= 0) {
                err = (w < 0) ? (ag_err_t)w : -AG_ENOSPC;
                break;
            }
            off += (size_t)w;
        }
        if (err != AG_OK) {
            break;
        }
        got += (uint64_t)got_now;
        ag_progress_tick(&prog, got);

        if (ag_shell_interrupted()) {
            err = -AG_EINTR;
            break;
        }
    }
    ag_progress_done(&prog, got);

    ag_vfs_close(out);
    ag_port_net_close(dfd);

    /* The transfer's own result, which is not the same as the socket's. */
    const int done = ftp_reply(f);
    if (err == AG_OK && done != 226 && done != 250) {
        err = -AG_EIO;
    }
    return err;
}

static ag_err_t ftp_put(ftp_t *f, const char *local, const char *remote)
{
    const ag_handle_t in = ag_vfs_open(local, NULL, AG_O_RDONLY);
    if (in < 0) {
        ag_console_printf("%s: cannot be read\n", local);
        return (ag_err_t)in;
    }

    ag_stat_t st;
    const uint64_t total =
        (ag_vfs_stat(local, NULL, &st) == AG_OK) ? st.size : 0;

    const int dfd = ftp_data_open(f);
    if (dfd < 0) {
        ag_vfs_close(in);
        return -AG_EIO;
    }

    char cmd[FTP_LINE_MAX];
    const int n = snprintf(cmd, sizeof(cmd), "STOR %s\r\n", remote);
    if (n < 0 || (size_t)n >= sizeof(cmd) ||
        ftp_send(f, cmd, (size_t)n) != AG_OK) {
        ag_vfs_close(in);
        ag_port_net_close(dfd);
        return -AG_EIO;
    }

    const int code = ftp_reply(f);
    if (code != 150 && code != 125) {
        ag_vfs_close(in);
        ag_port_net_close(dfd);
        return (code == 550) ? -AG_EACCES : -AG_EIO;
    }

    ag_progress_t prog;
    ag_progress_start(&prog, total);

    uint64_t sent = 0;
    ag_err_t err = AG_OK;
    int32_t  n_read;

    while ((n_read = ag_vfs_read(in, f->data, FTP_DATA_BUF)) > 0) {
        err = ag_netio_send_all(dfd, f->data, (size_t)n_read);
        if (err != AG_OK) {
            break;
        }
        sent += (uint64_t)n_read;
        ag_progress_tick(&prog, sent);
        if (ag_shell_interrupted()) {
            err = -AG_EINTR;
            break;
        }
    }
    if (n_read < 0) {
        err = (ag_err_t)n_read;
    }
    ag_progress_done(&prog, sent);

    ag_vfs_close(in);
    /* Closing the data connection is how the server is told the file ended. */
    ag_port_net_close(dfd);

    const int done = ftp_reply(f);
    if (err == AG_OK && done != 226 && done != 250) {
        err = -AG_EIO;
    }
    return err;
}

static ag_err_t ftp_list(ftp_t *f, const char *path)
{
    const int dfd = ftp_data_open(f);
    if (dfd < 0) {
        return -AG_EIO;
    }

    char cmd[FTP_LINE_MAX];
    const int n = (path != NULL && path[0] != '\0')
                      ? snprintf(cmd, sizeof(cmd), "LIST %s\r\n", path)
                      : snprintf(cmd, sizeof(cmd), "LIST\r\n");
    if (n < 0 || (size_t)n >= sizeof(cmd) ||
        ftp_send(f, cmd, (size_t)n) != AG_OK) {
        ag_port_net_close(dfd);
        return -AG_EIO;
    }

    const int code = ftp_reply(f);
    if (code != 150 && code != 125) {
        ag_port_net_close(dfd);
        return -AG_EIO;
    }

    /*
     * The listing is text, and the data buffer is free while it arrives - the
     * control connection is silent until the transfer ends.
     */
    ag_netio_t dr;
    ag_netio_init(&dr, dfd, f->data, FTP_DATA_BUF, 0);

    char line[FTP_LINE_MAX];
    for (;;) {
        const ag_err_t err = ag_netio_line(&dr, line, sizeof(line));
        if (err == -AG_EIO) {
            break; /* end of the listing */
        }
        if (err != AG_OK && err != -AG_ERANGE) {
            break;
        }
        ag_console_printf("%s\n", line);
        if (ag_shell_interrupted()) {
            break;
        }
    }

    ag_port_net_close(dfd);
    (void)ftp_reply(f);
    return AG_OK;
}

/* ---------------------------------------------------------------------- */
/* Session                                                                */
/* ---------------------------------------------------------------------- */

static void ftp_end(ftp_t *f)
{
    if (f->ctl >= 0) {
        (void)ftp_send(f, "QUIT\r\n", 6);
        (void)ftp_reply(f);
        ag_port_net_close(f->ctl);
        f->ctl = -1;
    }
    ag_port_free(f->mem);
    f->mem = NULL;
}

static ag_err_t ftp_begin(ftp_t *f, const ag_url_t *u, const char *cwd)
{
    memset(f, 0, sizeof(*f));
    f->ctl = -1;
    f->cwd = cwd;

    f->mem = ag_port_alloc(FTP_CTL_BUF + FTP_DATA_BUF, AG_MEM_FAST);
    if (f->mem == NULL) {
        ag_console_puts("not enough memory for a transfer\n");
        return -AG_ENOMEM;
    }
    f->ctlbuf = (uint8_t *)f->mem;
    f->data = (uint8_t *)f->mem + FTP_CTL_BUF;

    ag_err_t err = ag_net_lookup(u->host, &f->host_addr);
    if (err != AG_OK) {
        ag_console_printf("%s: cannot be resolved\n", u->host);
        ag_port_free(f->mem);
        f->mem = NULL;
        return err;
    }

    char ip[16];
    (void)ag_ipv4_str(f->host_addr, ip, sizeof(ip));
    ag_console_printf("%s:%u ... ", ip, (unsigned)u->port);

    f->ctl = ag_port_net_connect(f->host_addr, u->port, FTP_CONNECT_MS);
    if (f->ctl < 0) {
        ag_console_puts("no answer\n");
        err = (ag_err_t)f->ctl;
        f->ctl = -1;
        ag_port_free(f->mem);
        f->mem = NULL;
        return err;
    }
    ag_console_puts("connected\n");

    ag_netio_init(&f->rdr, f->ctl, f->ctlbuf, FTP_CTL_BUF, 0);

    if (ftp_reply(f) != 220) {
        ftp_end(f);
        return -AG_EIO;
    }

    /*
     * Anonymous unless told otherwise, with an address for a password, because
     * that is the convention every public server documents and half of them
     * enforce.
     */
    const char *user = (u->user[0] != '\0') ? u->user : "anonymous";
    const char *pass = (u->user[0] != '\0') ? u->pass : "argon@argonos";

    int code = ftp_command(f, "USER %s\r\n", user);
    if (code == 331 || code == 332) {
        code = ftp_command(f, "PASS %s\r\n", pass);
    }
    if (code != 230 && code != 202) {
        ag_console_puts("that login was not accepted\n");
        ftp_end(f);
        return -AG_EACCES;
    }

    /* Binary for everything: see the file header. */
    (void)ftp_command(f, "TYPE I\r\n");
    return AG_OK;
}

/* The local side of a name typed at the ftp prompt. */
static ag_err_t local_path(ftp_t *f, const char *name, char *out, size_t len)
{
    return ag_path_resolve(name, f->cwd, out, len);
}

static void ftp_help(void)
{
    ag_console_puts(
        "  ls [dir]      list            get <file> [local]  fetch\n"
        "  cd <dir>      change dir      put <local> [file]  send\n"
        "  pwd           where am i      del <file>          delete\n"
        "  md <dir>      make dir        rd <dir>            remove dir\n"
        "  bye           leave           quote <text>        raw command\n");
}

ag_err_t ag_ftp_run(const ag_url_t *u, const char *cwd)
{
    ftp_t          f;
    const ag_err_t err = ftp_begin(&f, u, cwd);
    if (err != AG_OK) {
        return err;
    }

    if (u->path[0] != '\0' && strcmp(u->path, "/") != 0) {
        (void)ftp_command(&f, "CWD %s\r\n", u->path);
    }
    ag_console_puts("ftp: `help` for commands, `bye` to leave\n");

    char line[FTP_LINE_MAX];
    for (;;) {
        ag_shell_clear_interrupted();
        ag_console_puts("ftp> ");

        const int32_t n = ag_console_readline(line, sizeof(line));
        if (n < 0) {
            ag_console_puts("\n");
            break; /* Ctrl+C at the prompt leaves, like every other prompt */
        }

        /* One word, then the rest: FTP names may contain spaces, so only the
         * verb and the first argument are split out. */
        char *verb = line;
        while (*verb == ' ') {
            verb++;
        }
        char *arg = strchr(verb, ' ');
        if (arg != NULL) {
            *arg++ = '\0';
            while (*arg == ' ') {
                arg++;
            }
        }
        char *arg2 = NULL;
        if (arg != NULL) {
            arg2 = strchr(arg, ' ');
            if (arg2 != NULL) {
                *arg2++ = '\0';
                while (*arg2 == ' ') {
                    arg2++;
                }
            }
        }

        if (verb[0] == '\0') {
            continue;
        }
        if (ag_path_icmp(verb, "bye") == 0 ||
            ag_path_icmp(verb, "quit") == 0 ||
            ag_path_icmp(verb, "exit") == 0) {
            break;
        }
        if (ag_path_icmp(verb, "help") == 0 || verb[0] == '?') {
            ftp_help();
            continue;
        }
        if (ag_path_icmp(verb, "ls") == 0 || ag_path_icmp(verb, "dir") == 0) {
            (void)ftp_list(&f, arg);
            continue;
        }
        if (ag_path_icmp(verb, "cd") == 0) {
            if (arg == NULL) {
                ag_console_puts("cd <directory>\n");
                continue;
            }
            (void)ftp_command(&f, "CWD %s\r\n", arg);
            continue;
        }
        if (ag_path_icmp(verb, "pwd") == 0) {
            (void)ftp_command(&f, "PWD\r\n");
            continue;
        }
        if (ag_path_icmp(verb, "get") == 0) {
            if (arg == NULL) {
                ag_console_puts("get <remote> [local]\n");
                continue;
            }
            char dest[AG_PATH_MAX];
            const char *want = (arg2 != NULL) ? arg2 : ag_path_basename(arg);
            if (local_path(&f, want, dest, sizeof(dest)) != AG_OK) {
                ag_console_puts("that local name will not do\n");
                continue;
            }
            (void)ftp_get(&f, arg, dest);
            continue;
        }
        if (ag_path_icmp(verb, "put") == 0) {
            if (arg == NULL) {
                ag_console_puts("put <local> [remote]\n");
                continue;
            }
            char src[AG_PATH_MAX];
            if (local_path(&f, arg, src, sizeof(src)) != AG_OK) {
                ag_console_puts("that local name will not do\n");
                continue;
            }
            (void)ftp_put(&f, src,
                          (arg2 != NULL) ? arg2 : ag_path_basename(src));
            continue;
        }
        if (ag_path_icmp(verb, "del") == 0) {
            if (arg == NULL) {
                ag_console_puts("del <file>\n");
                continue;
            }
            (void)ftp_command(&f, "DELE %s\r\n", arg);
            continue;
        }
        if (ag_path_icmp(verb, "md") == 0) {
            if (arg == NULL) {
                ag_console_puts("md <directory>\n");
                continue;
            }
            (void)ftp_command(&f, "MKD %s\r\n", arg);
            continue;
        }
        if (ag_path_icmp(verb, "rd") == 0) {
            if (arg == NULL) {
                ag_console_puts("rd <directory>\n");
                continue;
            }
            (void)ftp_command(&f, "RMD %s\r\n", arg);
            continue;
        }
        if (ag_path_icmp(verb, "quote") == 0) {
            /* Whatever the operator knows that this client does not. */
            if (arg == NULL) {
                ag_console_puts("quote <command>\n");
                continue;
            }
            if (arg2 != NULL) {
                (void)ftp_command(&f, "%s %s\r\n", arg, arg2);
            } else {
                (void)ftp_command(&f, "%s\r\n", arg);
            }
            continue;
        }

        ag_console_printf("%s? `help` lists what there is\n", verb);
    }

    ftp_end(&f);
    ag_shell_clear_interrupted();
    return AG_OK;
}

ag_err_t ag_ftp_fetch(const ag_url_t *u, const char *dest)
{
    if (u->path[0] == '\0' || strcmp(u->path, "/") == 0) {
        ag_console_puts("that ftp address names no file\n");
        return -AG_EINVAL;
    }

    ftp_t          f;
    const ag_err_t err = ftp_begin(&f, u, NULL);
    if (err != AG_OK) {
        return err;
    }

    const ag_err_t gerr = ftp_get(&f, u->path, dest);
    ftp_end(&f);
    return gerr;
}

#endif /* CONFIG_ARGON_ENABLE_NET */
