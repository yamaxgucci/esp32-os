/*
 * ArgonOS - net, wget, httpd, ftp.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/config.h>

#if CONFIG_ARGON_ENABLE_NET

#include "shell/cmd_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <argon/console.h>
#include <argon/loader.h>
#include <argon/net.h>
#include <argon/netmsg.h>
#include <argon/path.h>
#include <argon/shell.h>
#include <argon/vfs.h>

#include <argon/port/net.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

#include "net/netsvc.h"

static const char *strerr(ag_err_t err)
{
    return ag_loader_api()->sys->strerror(err);
}

/* ---------------------------------------------------------------------- */
/* net                                                                    */
/* ---------------------------------------------------------------------- */

/*
 * Exists even though `wifi` prints an address too: a board on a cable has no
 * `wifi` command at all (the radio is not in that image), and every network
 * question starts with "does this machine have an address".
 */
static int net_status(void)
{
    if (!ag_net_ready()) {
        ag_console_puts("no address\n");
        ag_console_puts(
            "  the interface may still be starting, or DHCP may have no "
            "answer\n");
        return 1;
    }

    uint32_t addr = 0;
    const ag_err_t err = ag_port_net_ifaddr(&addr);
    if (err != AG_OK) {
        ag_console_printf("address: %s\n", strerr(err));
        return 1;
    }

    char ip[16];
    (void)ag_ipv4_str(addr, ip, sizeof(ip));
    ag_console_printf("address %s\n", ip);
    return 0;
}

int ag_cmd_net(int argc, char **argv)
{
    if (argc < 2) {
        return net_status();
    }

    if (ag_path_icmp(argv[1], "resolve") == 0) {
        if (argc < 3) {
            ag_console_puts("usage: net resolve <name>\n");
            return 1;
        }
        uint32_t       addr = 0;
        const ag_err_t err = ag_net_lookup(argv[2], &addr);
        if (err != AG_OK) {
            ag_console_printf("%s: %s\n", argv[2], strerr(err));
            return 1;
        }
        char ip[16];
        (void)ag_ipv4_str(addr, ip, sizeof(ip));
        ag_console_printf("%s is %s\n", argv[2], ip);
        return 0;
    }

    if (ag_path_icmp(argv[1], "wait") == 0) {
        /*
         * For a script.  A board that comes up and fetches something cannot
         * do it in the second after boot: the interface is up long before
         * DHCP has answered, and every command that needs an address would
         * otherwise fail once and work when typed by hand.
         */
        const unsigned secs =
            (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 10) : 30u;
        const int64_t deadline = ag_port_us() + (int64_t)secs * 1000000;

        while (!ag_net_ready()) {
            if (ag_shell_interrupted()) {
                ag_console_puts("^C\n");
                return 1;
            }
            if (ag_port_us() > deadline) {
                ag_console_printf("no address after %us\n", secs);
                return 1;
            }
            ag_port_task_delay(ag_port_ms_to_ticks(100));
        }
        return net_status();
    }

    ag_console_puts("usage: net [wait [seconds] | resolve <name>]\n");
    return 1;
}

/* ---------------------------------------------------------------------- */
/* wget                                                                   */
/* ---------------------------------------------------------------------- */

/*
 * The name a URL implies, when nobody said where to put the file.
 *
 * The last segment of the path, decoded, with the query cut off first - and
 * "index.htm" when the URL names a directory, because a file has to be called
 * something and that is the name the same page has everywhere else.
 */
static void name_from_url(const ag_url_t *u, char *out, size_t len)
{
    char path[AG_URL_PATH_MAX + 1];
    snprintf(path, sizeof(path), "%s", u->path);

    char *q = strchr(path, '?');
    if (q != NULL) {
        *q = '\0';
    }
    (void)ag_pct_decode(path);

    const char *slash = strrchr(path, '/');
    const char *base = (slash != NULL) ? slash + 1 : path;

    if (base[0] == '\0') {
        snprintf(out, len, "index.htm");
        return;
    }
    snprintf(out, len, "%s", base);
}

int ag_cmd_wget(int argc, char **argv)
{
    if (argc < 2) {
        ag_console_puts("usage: wget <url> [file]\n");
        ag_console_puts("  http:// and ftp:// - there is no TLS in this "
                        "system, so no https\n");
        return 1;
    }

    ag_url_t       u;
    const ag_err_t uerr = ag_url_parse(argv[1], &u);
    if (uerr == -AG_ENOTSUP) {
        ag_console_printf("%s: this system speaks http and ftp only\n",
                          argv[1]);
        return 1;
    }
    if (uerr != AG_OK) {
        ag_console_printf("%s: %s\n", argv[1], strerr(uerr));
        return 1;
    }

    /* Where the bytes go.  A directory as the destination means "in there
     * under the name the URL implies", which is what copy does too. */
    char name[AG_URL_PATH_MAX + 1];
    name_from_url(&u, name, sizeof(name));

    char     dest[AG_PATH_MAX];
    ag_err_t err = ag_path_resolve((argc > 2) ? argv[2] : name,
                                   ag_shell_cwd(), dest, sizeof(dest));
    if (err != AG_OK) {
        ag_console_printf("%s: %s\n", (argc > 2) ? argv[2] : name,
                          strerr(err));
        return 1;
    }

    ag_stat_t st;
    if (ag_vfs_stat(dest, NULL, &st) == AG_OK && (st.attr & AG_A_DIR) != 0) {
        char joined[AG_PATH_MAX];
        if (ag_path_join(dest, name, joined, sizeof(joined)) != AG_OK) {
            ag_console_puts("that path is too long\n");
            return 1;
        }
        snprintf(dest, sizeof(dest), "%s", joined);
    }

    if (strcmp(u.scheme, "ftp") == 0) {
        err = ag_ftp_fetch(&u, dest);
    } else {
        err = ag_http_fetch(argv[1], dest);
    }

    if (err != AG_OK) {
        return 1;
    }

    char shown[AG_PATH_MAX];
    ag_shell_dos_path(dest, shown, sizeof(shown));
    ag_console_printf("saved %s\n", shown);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* httpd                                                                  */
/* ---------------------------------------------------------------------- */

int ag_cmd_httpd(int argc, char **argv)
{
    uint16_t    port = 80;
    const char *root_arg = ".";
    bool        writable = false;

    /*
     * /w is a switch rather than the default, and it is spelled out in the
     * usage: with it, anybody who can reach this board can put a file on the
     * card and take one off it.  On a home network that is the point; there is
     * no password here, so it should never happen by accident.
     */
    int  positional = 0;
    const char *args[2] = {NULL, NULL};
    for (int i = 1; i < argc; i++) {
        if (ag_path_icmp(argv[i], "/w") == 0) {
            writable = true;
        } else if (positional < 2) {
            args[positional++] = argv[i];
        }
    }

    int next = 0;
    if (args[next] != NULL && args[next][0] >= '0' && args[next][0] <= '9') {
        const long v = strtol(args[next], NULL, 10);
        if (v < 1 || v > 65535) {
            ag_console_puts("usage: httpd [port] [directory] [/w]\n");
            ag_console_puts("  /w  let browsers send files here and delete "
                            "them\n");
            return 1;
        }
        port = (uint16_t)v;
        next++;
    }
    if (next < 2 && args[next] != NULL) {
        root_arg = args[next];
    }

    char           root[AG_PATH_MAX];
    const ag_err_t perr =
        ag_path_resolve(root_arg, ag_shell_cwd(), root, sizeof(root));
    if (perr != AG_OK) {
        ag_console_printf("%s: %s\n", root_arg, strerr(perr));
        return 1;
    }

    ag_stat_t st;
    if (ag_vfs_stat(root, NULL, &st) != AG_OK || (st.attr & AG_A_DIR) == 0) {
        ag_console_printf("%s: not a directory\n", root_arg);
        return 1;
    }

    return (ag_httpd_run(port, root, writable) == AG_OK) ? 0 : 1;
}

/* ---------------------------------------------------------------------- */
/* ftp                                                                    */
/* ---------------------------------------------------------------------- */

int ag_cmd_ftp(int argc, char **argv)
{
    if (argc < 2) {
        ag_console_puts("usage: ftp <host|ftp://[user[:pass]@]host[/dir]> "
                        "[user] [password]\n");
        return 1;
    }

    ag_url_t u;
    if (strstr(argv[1], "://") != NULL) {
        const ag_err_t err = ag_url_parse(argv[1], &u);
        if (err != AG_OK || strcmp(u.scheme, "ftp") != 0) {
            ag_console_printf("%s: not an ftp address\n", argv[1]);
            return 1;
        }
    } else {
        /*
         * A bare host is the common case at a prompt, and "host:port" is the
         * second commonest - so it is taken here rather than left to fail as a
         * name that cannot be resolved, which is what it looks like otherwise.
         */
        memset(&u, 0, sizeof(u));
        snprintf(u.scheme, sizeof(u.scheme), "ftp");
        u.port = 21;
        u.path[0] = '\0';

        char host[AG_URL_HOST_MAX + 16];
        if ((size_t)snprintf(host, sizeof(host), "%s", argv[1]) >=
            sizeof(host)) {
            ag_console_puts("that host name is too long\n");
            return 1;
        }
        char *colon = strchr(host, ':');
        if (colon != NULL) {
            *colon++ = '\0';
            const long port = strtol(colon, NULL, 10);
            if (port < 1 || port > 65535) {
                ag_console_printf("%s: not a port\n", colon);
                return 1;
            }
            u.port = (uint16_t)port;
        }
        if ((size_t)snprintf(u.host, sizeof(u.host), "%s", host) >=
            sizeof(u.host)) {
            ag_console_puts("that host name is too long\n");
            return 1;
        }
    }

    if (argc > 2) {
        snprintf(u.user, sizeof(u.user), "%s", argv[2]);
        u.pass[0] = '\0';
    }
    if (argc > 3) {
        snprintf(u.pass, sizeof(u.pass), "%s", argv[3]);
    }

    return (ag_ftp_run(&u, ag_shell_cwd()) == AG_OK) ? 0 : 1;
}

#endif /* CONFIG_ARGON_ENABLE_NET */
