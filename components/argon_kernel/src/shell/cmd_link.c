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
#include <argon/path.h>

/*
 * Channel 1 is this command's own conversation.  Channel 0 is reserved for the
 * link talking about itself, and the numbered ones above are for the things
 * that will come later - a remote device, a process's console - each of which
 * wants its own so that a stalled one does not hold up the others.
 */
#define LINK_CHAN_SHELL 1u

#define LINK_MSG_MAX 240u

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
    ag_console_printf("bytes   %u out, %u in\n", (unsigned)st.tx_bytes,
                      (unsigned)st.rx_bytes);
}

static int link_status(void)
{
    if (!ag_linkprov_bound()) {
        ag_console_puts("no link bound\n");
        ag_console_puts("  `dev` lists what is here; `link use <dev>` binds "
                        "one\n");
        return 1;
    }
    ag_console_printf("link on %s, far end %s\n", ag_linkprov_active(),
                      ag_linkprov_up() ? "heard" : "silent");
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
    uint8_t  buf[LINK_MSG_MAX + 1];
    uint32_t answered = 0;
    uint32_t ms = secs * 1000u;

    ag_console_printf("echo: %u s, any key stops\n", (unsigned)secs);
    while (ms > 0) {
        uint8_t       chan = 0;
        const int32_t n = ag_linkprov_recv(&chan, buf, sizeof(buf), 200);
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

static int link_ping(uint32_t count)
{
    uint8_t  out[64];
    uint8_t  in[LINK_MSG_MAX + 1];
    uint32_t got = 0;

    ag_console_printf("ping: %u messages\n", (unsigned)count);
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
            s = ag_linkprov_send(LINK_CHAN_SHELL, AG_LINK_CONTROL, out,
                                 (size_t)len + 1u);
            if (s == -AG_EAGAIN) {
                ag_linkprov_poll(5);
            }
        }
        if (s < 0) {
            ag_console_printf("send %u: %d\n", (unsigned)i, (int)s);
            break;
        }

        uint8_t       chan = 0;
        const int32_t n = ag_linkprov_recv(&chan, in, sizeof(in), 2000);
        if (n > 0 && in[0] == (uint8_t)LINK_TAG_REP) {
            got++;
        }
        if (ag_console_getch(0) >= 0) {
            break;
        }
    }
    ag_console_printf("%u of %u came back\n", (unsigned)got, (unsigned)count);
    print_stats();
    return (got == count) ? 0 : 1;
}

int ag_cmd_link(int argc, char **argv)
{
    if (argc < 2) {
        return link_status();
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
        return 0;
    }

    if (!ag_linkprov_bound()) {
        return link_status();
    }

    if (ag_path_icmp(argv[1], "ping") == 0) {
        const uint32_t n = (argc > 2) ? (uint32_t)atoi(argv[2]) : 8u;
        return link_ping((n == 0) ? 1u : n);
    }
    if (ag_path_icmp(argv[1], "echo") == 0) {
        const uint32_t s = (argc > 2) ? (uint32_t)atoi(argv[2]) : 20u;
        return link_echo((s == 0) ? 1u : s);
    }
    if (ag_path_icmp(argv[1], "poll") == 0) {
        const uint32_t ms = (argc > 2) ? (uint32_t)atoi(argv[2]) : 500u;
        ag_linkprov_poll(ms);
        return link_status();
    }

    ag_console_puts("usage: link [use <dev>|none | ping [n] | echo [secs] | "
                    "poll [ms]]\n");
    return 1;
}
