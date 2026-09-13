/*
 * ArgonOS - the node link, at the prompt.  See shell/cmd_link.h.
 *
 * What this is for, beyond looking at a link: `ping` and `echo` are the two
 * halves of a round trip, and between two nodes they are the first thing that
 * proves the whole stack - the shell, the indirection, the driver, NLINK's
 * scheduler and window, a UART, and all of it again backwards.  Everything
 * later (a remote process, a remote device) is that round trip with a payload
 * that means something.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "shell/cmd_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <argon/abi.h>
#include <argon/console.h>
#include <argon/linkprov.h>
#include <argon/loader.h>
#include <argon/nodedev.h>
#include <argon/nodedrv.h>
#include <argon/nodehello.h>
#include <argon/nodename.h>
#include <argon/noderdev.h>
#include <argon/nodeps.h>
#include <argon/nodefs.h>
#include <argon/nodeproc.h>
#include <argon/path.h>

/*
 * Channel 1 is this command's own conversation.  Channel 0 is reserved for the
 * link talking about itself, and the numbered ones above are for the things
 * that will come later - a remote device, a process's console - each of which
 * wants its own so that a stalled one does not hold up the others.
 */
#define LINK_CHAN_SHELL 1u

#define LINK_MSG_MAX 240u

/*
 * A one-message mailbox, filled by the link's reader task.
 *
 * There is no recv() to call any more, and the reason is a bug this file
 * caused: a server for remote runs appeared beside `link ping`, both read the
 * one inbox, and six messages in twenty-four went to the wrong one - which the
 * shell then reported as lost.  One reader, handlers per channel, and a
 * consumer waits on its own channel's mailbox.
 */
static volatile bool s_have;
static uint8_t       s_msg[LINK_MSG_MAX + 1];
static volatile uint16_t s_msg_len;
static volatile uint8_t  s_msg_chan;

static void on_shell_msg(uint8_t chan, const void *data, uint16_t len,
                         void *ctx)
{
    (void)ctx;
    if (s_have) {
        return; /* the last one has not been taken; drop rather than overwrite */
    }
    if (len > sizeof(s_msg)) {
        len = (uint16_t)sizeof(s_msg);
    }
    memcpy(s_msg, data, len);
    s_msg_len = len;
    s_msg_chan = chan;
    s_have = true;
}

/* Wait for one, up to `ms`.  Returns its length, or -AG_EAGAIN. */
static int32_t shell_wait(uint8_t *chan, uint8_t *buf, size_t max, uint32_t ms)
{
    /*
     * On the clock, not by counting sleeps: a tick is ten milliseconds whatever
     * a shorter sleep asks for, and a timeout built out of those is wrong by
     * that factor - which the driver next door learned the expensive way.
     */
    const uint32_t start = (uint32_t)(ag_loader_api()->time->us() / 1000u);
    for (;;) {
        if (s_have) {
            uint16_t n = s_msg_len;
            if (n > max) {
                n = (uint16_t)max;
            }
            memcpy(buf, s_msg, n);
            *chan = s_msg_chan;
            s_have = false;
            return (int32_t)n;
        }
        const uint32_t now = (uint32_t)(ag_loader_api()->time->us() / 1000u);
        if ((uint32_t)(now - start) >= ms) {
            return -AG_EAGAIN;
        }
        ag_linkprov_poll(2);
    }
}

static void print_stats(void)
{
    ag_link_stats_t st;
    if (ag_linkprov_stats(&st) != AG_OK) {
        return;
    }
    ag_console_printf("frames  %u sent (%u again), %u received\n",
                      (unsigned)st.sent, (unsigned)st.retransmits,
                      (unsigned)st.received);
    /*
     * Gaps and duplicates apart, because they say different things about which
     * direction of the wire is bad: a gap is a frame lost going out, a
     * duplicate is an acknowledgement lost coming back.
     */
    ag_console_printf("losses  %u gaps, %u duplicates, %u bad\n",
                      (unsigned)st.gaps, (unsigned)st.dups, (unsigned)st.bad);
    ag_console_printf("acks    %u sent alone (the wire was idle and one was "
                      "owed)\n",
                      (unsigned)st.acks);
    ag_console_printf("bytes   %u out, %u in\n", (unsigned)st.tx_bytes,
                      (unsigned)st.rx_bytes);
    /*
     * Beside the retransmits, because it is what makes them readable: frames
     * going out twice with no gaps and no duplicates means the acknowledgement
     * was late against this number, not that the wire lost anything.
     */
    if (st.srtt_ms != 0u) {
        ag_console_printf("timer   %u ms, round trip %u ms (measured)\n",
                          (unsigned)st.rto_ms, (unsigned)st.srtt_ms);
    } else {
        ag_console_printf("timer   %u ms, round trip not measured yet\n",
                          (unsigned)st.rto_ms);
    }
}

static int link_status(void)
{
    if (!ag_linkprov_bound()) {
        ag_console_puts("no link bound\n");
        ag_console_puts("  `dev` lists what is here; `link use <dev>` binds "
                        "one\n");
        return 1;
    }
    const char *peer = ag_node_peer_name();
    ag_console_printf("link on %s, far end %s\n", ag_linkprov_active(),
                      ag_linkprov_up() ? "heard" : "silent");
    /*
     * Who, not just whether.  "The far end is heard" and "the far end is n2"
     * are different facts, and only the second one can be typed into a config.
     */
    ag_console_printf("this node is %s, the other is %s\n", ag_node_name(),
                      (peer != NULL) ? peer : "not saying yet");
    /*
     * Said here as well as when the two first meet, because a warning you can
     * only see if you happened to be watching at that second is a warning that
     * will be missed - the console is twenty-five lines and a bind is followed
     * by everything else.  A standing condition belongs in the status.
     */
    if (peer != NULL && strcmp(peer, ag_node_name()) == 0) {
        ag_console_printf("  both are called %s - `link name <x>` on one of "
                          "them\n", peer);
    }
    print_stats();
    return 0;
}

/*
 * Answer whatever arrives, for `secs` seconds.  The other half of `ping`.
 *
 * A reply is the message with one byte in front of it, so that a node cannot
 * mistake its own echo for a request and the two nodes cannot echo at each
 * other for ever.  That is not hypothetical: the first draft echoed the message
 * unchanged and both ends bounced one message until the window filled.
 */
#define LINK_TAG_REQ 'q'
#define LINK_TAG_REP 'r'

static int link_echo(uint32_t secs)
{
    if (ag_linkprov_listen(LINK_CHAN_SHELL, on_shell_msg, NULL) != AG_OK) {
        ag_console_puts("channel 1 is taken\n");
        return 1;
    }

    uint8_t  buf[LINK_MSG_MAX + 1];
    uint32_t answered = 0;
    uint32_t ms = secs * 1000u;

    ag_console_printf("echo: %u s, any key stops\n", (unsigned)secs);
    while (ms > 0) {
        uint8_t       chan = 0;
        const int32_t n = shell_wait(&chan, buf, sizeof(buf), 200);
        if (ms >= 200u) {
            ms -= 200u;
        } else {
            ms = 0;
        }
        /*
         * getch with a zero timeout, not peek-then-get.  Peeking sees ANY
         * event and getch without a timeout waits for a character for ever, so
         * the two together hang the moment a pointer event arrives - which on
         * a touch board is the moment somebody touches it.
         */
        if (ag_console_getch(0) >= 0) {
            break;
        }
        if (n <= 0 || buf[0] != (uint8_t)LINK_TAG_REQ) {
            continue;
        }
        buf[0] = (uint8_t)LINK_TAG_REP;
        if (ag_linkprov_send(chan, AG_LINK_CONTROL, buf, (size_t)n) > 0) {
            answered++;
        }
    }
    ag_console_printf("answered %u\n", (unsigned)answered);
    print_stats();
    return 0;
}

/*
 * `count` messages there and back, on `cls`.
 *
 * The class is an argument because the STREAM class has never carried anything
 * between two nodes.  Everything built so far - a remote run, a file, a device
 * call - is a question with an answer, so all of it went on CONTROL or BULK,
 * and the class that audio is meant to ride has only ever been exercised by the
 * host tests.  A path nothing has used is a path nobody knows the state of, and
 * the whole plan for sound over the wire rests on this one.
 *
 * STREAM is UNRELIABLE by design: unsequenced, unacknowledged, dropped rather
 * than queued when the wire is behind.  So the pass mark for it is not "all of
 * them" - it is "most of them, and the ones that came are whole".
 */
static int link_ping(uint32_t count, uint8_t cls)
{
    if (ag_linkprov_listen(LINK_CHAN_SHELL, on_shell_msg, NULL) != AG_OK) {
        ag_console_puts("channel 1 is taken\n");
        return 1;
    }

    uint8_t  out[64];
    uint8_t  in[LINK_MSG_MAX + 1];
    uint32_t got = 0;

    ag_console_printf("ping: %u messages on %s\n", (unsigned)count,
                      (cls == AG_LINK_STREAM)  ? "stream"
                      : (cls == AG_LINK_BULK) ? "bulk"
                                              : "control");
    for (uint32_t i = 0; i < count; i++) {
        const int len =
            snprintf((char *)out + 1, sizeof(out) - 1, "ping %u", (unsigned)i);
        out[0] = (uint8_t)LINK_TAG_REQ;

        /*
         * A full window is back-pressure, not a failure: pump and try again.
         * Bounded, because a link whose far end has gone away would otherwise
         * spin here for ever and look like a hung shell.
         */
        int32_t s = -AG_EAGAIN;
        for (int t = 0; t < 200 && s == -AG_EAGAIN; t++) {
            s = ag_linkprov_send(LINK_CHAN_SHELL, cls, out, (size_t)len + 1u);
            if (s == -AG_EAGAIN) {
                ag_linkprov_poll(5);
            }
        }
        if (s < 0) {
            ag_console_printf("send %u: %d\n", (unsigned)i, (int)s);
            break;
        }

        uint8_t       chan = 0;
        const int32_t n = shell_wait(&chan, in, sizeof(in), 2000);
        if (n > 0 && in[0] == (uint8_t)LINK_TAG_REP) {
            got++;
        }
        if (ag_console_getch(0) >= 0) {
            break;
        }
    }
    ag_console_printf("%u of %u came back\n", (unsigned)got, (unsigned)count);
    print_stats();
    /*
     * An unreliable class is not failed for being unreliable.  Losing one is
     * what it is allowed to do; losing most of them means the path is broken,
     * which is a different statement and the one worth making.
     */
    if (cls == AG_LINK_STREAM) {
        return (got * 2u >= count) ? 0 : 1;
    }
    return (got == count) ? 0 : 1;
}

int ag_cmd_link(int argc, char **argv)
{
    if (argc < 2) {
        return link_status();
    }

    /*
     * Before the "is a link bound" gate below, because naming a node is not a
     * link operation: a board has a name whether or not anything is wired to
     * it, and the sensible order is to name it and THEN bind - so the first
     * hello already carries the right name.
     */
    if (ag_path_icmp(argv[1], "name") == 0) {
        if (argc < 3) {
            ag_console_printf("this node is %s\n", ag_node_name());
            return 0;
        }
        const ag_err_t e = ag_node_name_set(argv[2]);
        if (e != AG_OK) {
            ag_console_puts("a node name is letters, digits, '-' and '_', "
                            "up to eight\n");
            /* No dot, and it is worth saying why: the dot is what separates a
             * node from a device in `n2.pcm0`. */
            ag_console_puts("  no dot: that separates a node from a device\n");
            return 1;
        }
        ag_console_printf("this node is %s\n", ag_node_name());
        if (ag_linkprov_bound()) {
            /* Say it again: the neighbour is holding the old one. */
            (void)ag_nodehello_start();
        }
        ag_console_puts("  put `name = ...` under [cluster] in SYSTEM.CFG to "
                        "keep it\n");
        return 0;
    }

    if (ag_path_icmp(argv[1], "use") == 0) {
        if (argc < 3) {
            ag_console_puts("usage: link use <dev>|none\n");
            return 1;
        }
        const ag_err_t err = ag_linkprov_use(argv[2]);
        if (err != AG_OK) {
            ag_console_printf("link use %s: %d\n", argv[2], (int)err);
            return 1;
        }
        ag_console_printf("link is %s\n", ag_linkprov_active());
        /*
         * Start answering the other node.  Here rather than at boot, because
         * before a link is bound there is nothing to answer on; and here rather
         * than inside linkprov, because a wire should not have to know what a
         * process is.
         */
        const ag_err_t serr = ag_nodeproc_start();
        if (serr != AG_OK) {
            ag_console_printf("  (not serving remote runs: %d)\n", (int)serr);
        }
        const ag_err_t derr = ag_nodedev_start();
        if (derr != AG_OK) {
            ag_console_printf("  (not answering device listings: %d)\n",
                              (int)derr);
        }
        const ag_err_t ferr = ag_nodefs_start();
        if (ferr != AG_OK) {
            ag_console_printf("  (not answering file fetches: %d)\n",
                              (int)ferr);
        }
        /*
         * Answer file requests from the moment a link exists, without waiting
         * to be asked: the other node may mount us before we mount it, and a
         * drive that appears only after both sides have run a command is a
         * drive nobody can rely on.
         */
        const ag_err_t verr = ag_nodedrv_serve();
        if (verr != AG_OK) {
            ag_console_printf("  (not serving our card: %d)\n", (int)verr);
        }
        /* Names, before anything that has to be addressed by one. */
        const ag_err_t herr = ag_nodehello_start();
        if (herr != AG_OK) {
            ag_console_printf("  (did not say hello: %d)\n", (int)herr);
        }
        const ag_err_t rerr = ag_noderdev_serve();
        if (rerr != AG_OK) {
            ag_console_printf("  (not serving our devices: %d)\n", (int)rerr);
        }
        const ag_err_t perr = ag_nodeps_serve();
        if (perr != AG_OK) {
            ag_console_printf("  (not answering `ps`: %d)\n", (int)perr);
        }
        return 0;
    }

    if (!ag_linkprov_bound()) {
        return link_status();
    }

    if (ag_path_icmp(argv[1], "ping") == 0) {
        const uint32_t n = (argc > 2) ? (uint32_t)atoi(argv[2]) : 8u;
        uint8_t        cls = AG_LINK_CONTROL;
        if (argc > 3) {
            if (ag_path_icmp(argv[3], "stream") == 0) {
                cls = AG_LINK_STREAM;
            } else if (ag_path_icmp(argv[3], "bulk") == 0) {
                cls = AG_LINK_BULK;
            } else if (ag_path_icmp(argv[3], "control") != 0) {
                ag_console_puts("class is control, stream or bulk\n");
                return 1;
            }
        }
        return link_ping((n == 0) ? 1u : n, cls);
    }
    if (ag_path_icmp(argv[1], "echo") == 0) {
        const uint32_t s = (argc > 2) ? (uint32_t)atoi(argv[2]) : 20u;
        return link_echo((s == 0) ? 1u : s);
    }
    if (ag_path_icmp(argv[1], "run") == 0) {
        if (argc < 3) {
            ag_console_puts("usage: link run <path> [args...]\n");
            ag_console_puts("  loads and runs it on the OTHER node; its output "
                            "comes back here\n");
            return 1;
        }
        /*
         * Rebuilt from argv rather than passed through whole: the shell has
         * already split it, and the far side splits it again with the same
         * code, so quoting means there exactly what it meant here.
         */
        char   line[AG_PATH_MAX + 64];
        size_t at = 0;
        for (int i = 2; i < argc; i++) {
            const size_t n = strlen(argv[i]);
            if (at + n + 2u >= sizeof(line)) {
                ag_console_puts("link run: command line too long\n");
                return 1;
            }
            if (at > 0) {
                line[at++] = ' ';
            }
            memcpy(line + at, argv[i], n);
            at += n;
        }
        line[at] = '\0';

        int32_t        code = 0;
        const ag_err_t err = ag_nodeproc_run(line, 60000u, &code);
        if (err == -AG_EBUSY) {
            ag_console_puts("the other node is already running something\n");
            return 1;
        }
        if (err != AG_OK) {
            ag_console_printf("link run: %d\n", (int)err);
            return 1;
        }
        /*
         * Two different answers, said differently.  A program that returned a
         * number and a program that never loaded are not the same event, and a
         * script that could not tell them apart would act on the wrong one.
         */
        if (code < 0) {
            ag_console_printf("did not run there: %d\n", (int)code);
            return 1;
        }
        ag_console_printf("returned %d from the other node\n", (int)code);
        return (code == 0) ? 0 : 1;
    }

    if (ag_path_icmp(argv[1], "get") == 0) {
        if (argc < 4) {
            ag_console_puts("usage: link get <there> <here>\n");
            ag_console_puts("  copies a file off the other node\n");
            return 1;
        }
        const int32_t n = ag_nodefs_get(argv[2], argv[3], "/", 120000u);
        if (n < 0) {
            ag_console_printf("link get: %d\n", (int)n);
            return 1;
        }
        ag_console_printf("%d bytes from the other node -> %s\n", (int)n,
                          argv[3]);
        return 0;
    }

    /*
     * The mountpoint is not the operator's business: a node has one card, so
     * there is one letter for it, and letting it be chosen only invents a way
     * for the two ends to disagree about where the other one is.
     */
    /*
     * Not automatic at `link use`, and that is a decision rather than an
     * omission: attaching asks the far node for its whole registry and puts a
     * dozen entries into this one, which is a thing an operator should ask for
     * rather than discover.  Also, the qualified names need the far node's
     * NAME, and the hello it is waiting on may not have been answered at the
     * instant the link is bound.
     */
    if (ag_path_icmp(argv[1], "devs") == 0) {
        const int32_t n = ag_noderdev_attach(8000u);
        if (n == -AG_EAGAIN) {
            ag_console_puts("the other node has not said its name yet; "
                            "try again\n");
            return 1;
        }
        if (n < 0) {
            ag_console_printf("link devs: %d\n", (int)n);
            return 1;
        }
        ag_console_printf("%d device(s) of %s are in this registry now\n",
                          (int)n, ag_node_peer_name());
        ag_console_puts("  `dev` lists them; open one by its qualified name\n");
        return 0;
    }

    if (ag_path_icmp(argv[1], "nodevs") == 0) {
        ag_console_printf("%u gone\n", (unsigned)ag_noderdev_detach());
        return 0;
    }

    if (ag_path_icmp(argv[1], "mount") == 0) {
        const ag_err_t e = ag_nodedrv_mount(AG_NODEDRV_MOUNT);
        if (e != AG_OK) {
            ag_console_printf("link mount: %d\n", (int)e);
            return 1;
        }
        ag_console_puts("the other node's card is N:\n");
        return 0;
    }

    if (ag_path_icmp(argv[1], "umount") == 0) {
        const ag_err_t e = ag_nodedrv_unmount(AG_NODEDRV_MOUNT);
        if (e != AG_OK) {
            ag_console_printf("link umount: %d\n", (int)e);
            return 1;
        }
        return 0;
    }

    if (ag_path_icmp(argv[1], "poll") == 0) {
        const uint32_t ms = (argc > 2) ? (uint32_t)atoi(argv[2]) : 500u;
        ag_linkprov_poll(ms);
        return link_status();
    }

    ag_console_puts("usage: link [use <dev>|none | run <path> [args] | "
                    "get <there> <here> | mount | umount | devs | nodevs | "
                    "name [x] | ping [n] [control|stream|bulk] | echo [secs] | "
                    "poll [ms]]\n");
    return 1;
}
