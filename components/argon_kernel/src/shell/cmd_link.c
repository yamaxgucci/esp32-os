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
#include "net/rdpaint.h"
#include "net/wtrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <argon/abi.h>
#include <argon/audio.h>
#include <argon/console.h>
#include <argon/linkprov.h>
#include <argon/loader.h>
#include <argon/nodedev.h>
#include <argon/nodedrv.h>
#include <argon/nodehello.h>
#include <argon/nodename.h>
#include <argon/noderdev.h>
#include <argon/clu.h>
#include <argon/kernel.h>
#include <argon/nodeboot.h>
#include <argon/nodeclu.h>
#include <argon/nodeps.h>
#include <argon/nodefs.h>
#include <argon/nodestandby.h>
/* For the SPI slave service's own counters - see print_spilink below. */
#include <argon/spilink.h>
#include <argon/uartlink.h>
#include <argon/port/task.h>
#include <argon/port/uart.h>
#include <argon/nodeproc.h>
#include <argon/path.h>

/*
 * Channel 1 is this command's own conversation.  Channel 0 is the hello
 * exchange's (nodehello.c), and 15 the background window's credits
 * (linkprov.c); the ones between are the services', each on its own so that
 * a stalled one does not hold up the others.
 */
#define LINK_CHAN_SHELL 1u

#define LINK_MSG_MAX 240u

/*
 * The two tags of the ping/echo protocol, here rather than beside `link echo`
 * because the receive path counts the answers and so has to know which byte
 * says a message is one.
 *
 * A reply is the message with one byte in front of it, so that a node cannot
 * mistake its own echo for a request and the two nodes cannot echo at each
 * other for ever.  That is not hypothetical: the first draft echoed the message
 * unchanged and both ends bounced one message until the window filled.
 */
#define LINK_TAG_REQ 'q'
#define LINK_TAG_REP 'r'

/*
 * THE PING'S OWN COUNT, taken where the answers arrive rather than where they
 * are read.
 *
 * `link ping` used to count the answers it took out of the one-message mailbox
 * below, and a mailbox that holds one message drops everything that arrives
 * while it is full - so what was being counted was how often the command got
 * back to the mailbox in time, not how many answers came back.  Measured on two
 * boards: `0 of 500 came back` while the link's own counters read 539 frames
 * sent and 539 received, no gaps, no duplicates, no bad frames and a one
 * millisecond round trip.  The wire had carried every answer; the instrument
 * was reading its own drain rate and calling it the wire.
 *
 * So the count is incremented HERE, on the link's reader task, for every answer
 * that arrives - and the mailbox below is left exactly as it is, because it is
 * right for what it is for: a consumer that genuinely wants one outstanding
 * message (`link echo`) takes them one at a time and does not care what it
 * misses while it is busy.
 *
 * THE RUN is what makes the count belong to one command.  An answer that
 * arrives after its ping has given up carries the run it was sent for, and only
 * the armed run is counted, so a straggler cannot be added to the next
 * command's total.  Zero means nothing is armed, which is why the run number
 * starts at one.
 */
static volatile uint32_t s_ping_run; /* the run whose answers count; 0 = none */
static volatile uint32_t s_ping_got; /* answers that arrived for that run     */
static uint32_t          s_ping_next; /* the run number the next ping takes   */

/* The run number out of "ping NNNNN", or 0 when the message is not that. */
static uint32_t ping_run_of(const uint8_t *p, uint16_t len)
{
    static const char k[] = "ping ";
    if (len < sizeof(k) - 1u || memcmp(p, k, sizeof(k) - 1u) != 0) {
        return 0u;
    }
    uint32_t v = 0u;
    for (uint16_t i = sizeof(k) - 1u; i < len; i++) {
        if (p[i] < '0' || p[i] > '9') {
            break;
        }
        v = v * 10u + (uint32_t)(p[i] - '0');
    }
    return v;
}

/*
 * A one-message mailbox, filled by the link's reader task.
 *
 * There is no recv() to call any more, and the reason is a bug this file
 * caused: a server for remote runs appeared beside `link ping`, both read the
 * one inbox, and six messages in twenty-four went to the wrong one - which the
 * shell then reported as lost.  One reader, handlers per channel, and a
 * consumer waits on its own channel's mailbox.
 *
 * WHO SENT IT AND WHICH WIRE IT CAME IN ON travel with the message, because
 * the task that answers is not the task that received: the link's reader fills
 * this, and the shell takes from it here - and ag_linkprov_from() means nothing
 * on the shell's own task.  A reply that does not carry both is a reply taken
 * by the board in between instead of being passed home - on the middle of three
 * boards, `link echo` would answer the wrong node and `link ping` would report
 * zero answers on a wire that is working perfectly.
 */
typedef struct {
    uint8_t  chan;
    uint16_t from; /* the node that sent it, for the answer */
    int32_t  wire; /* the wire it arrived on, so the answer leaves by it */
} link_origin_t;

static volatile bool s_have;
static uint8_t       s_msg[LINK_MSG_MAX + 1];
static volatile uint16_t s_msg_len;
static link_origin_t     s_msg_who;

static void on_shell_msg(uint8_t chan, const void *data, uint16_t len,
                         void *ctx)
{
    (void)ctx;
    const uint8_t *d = (const uint8_t *)data;

    /*
     * COUNTED BEFORE THE MAILBOX CAN REFUSE IT, and that order is the whole of
     * this instrument: an answer is an answer whether or not anybody was
     * waiting to take it out of the queue.  See s_ping_run above.
     */
    if (len > 1u && d[0] == (uint8_t)LINK_TAG_REP) {
        const uint32_t run = s_ping_run;
        if (run != 0u && ping_run_of(d + 1u, (uint16_t)(len - 1u)) == run) {
            s_ping_got++;
        }
    }

    if (s_have) {
        return; /* the last one has not been taken; drop rather than overwrite */
    }
    if (len > sizeof(s_msg)) {
        len = (uint16_t)sizeof(s_msg);
    }
    memcpy(s_msg, data, len);
    s_msg_len = len;
    s_msg_who.chan = chan;
    s_msg_who.from = ag_linkprov_from();
    s_msg_who.wire = ag_linkprov_current();
    s_have = true;
}

/*
 * Milliseconds since boot, from the loader's clock.
 *
 * One place for the unit conversion, because `link ping` times a whole run with
 * it and a second copy is a second thing to get the factor wrong in.
 */
static uint32_t now_ms(void)
{
    return (uint32_t)(ag_loader_api()->time->us() / 1000u);
}

/* Wait for one, up to `ms`.  Returns its length, or -AG_EAGAIN.  Where it came
 * from is filled in when `who` is not NULL. */
static int32_t shell_wait(link_origin_t *who, uint8_t *buf, size_t max,
                          uint32_t ms)
{
    /*
     * On the clock, not by counting sleeps: a tick is ten milliseconds whatever
     * a shorter sleep asks for, and a timeout built out of those is wrong by
     * that factor - which the driver next door learned the expensive way.
     */
    const uint32_t start = now_ms();
    for (;;) {
        if (s_have) {
            uint16_t n = s_msg_len;
            if (n > max) {
                n = (uint16_t)max;
            }
            memcpy(buf, s_msg, n);
            if (who != NULL) {
                *who = s_msg_who;
            }
            s_have = false;
            return (int32_t)n;
        }
        if ((uint32_t)(now_ms() - start) >= ms) {
            return -AG_EAGAIN;
        }
        ag_linkprov_poll(2);
    }
}

/*
 * An ANSWER, to whoever asked and out the wire it arrived on.
 *
 * The wire as well as the hash: the selected wire is where a NEW conversation
 * goes, and this board may have several neighbours, so an answer sent without
 * saying which wire would leave by whichever one was last selected - which on
 * the board in the middle is the other one half the time.
 */
static int32_t reply_to(const link_origin_t *who, uint8_t cls, const void *buf,
                        size_t len)
{
    if (who->wire >= 0) {
        return ag_linkprov_send_to_on(who->wire, who->from, who->chan, cls, buf,
                                      len);
    }
    return ag_linkprov_send_to(who->from, who->chan, cls, buf, len);
}

/*
 * THE BLOCK LAYER, UNDER THE FRAMES, on the end that does not clock it.
 *
 * Everything above says frames and bytes, which is the right unit for a wire
 * whose two ends both get to speak when they like.  On the SPI pair only one
 * end clocks, so the far end can put a frame out ONLY inside a block the
 * master opened - and then "frames received" and "blocks clocked" are two
 * different questions with two different cures.  A link carrying a tenth of
 * what the wire measured could be a scheduler that is not filling blocks or a
 * master that is not clocking them, and from above the two look the same.
 *
 * The counters existed and were readable through the ABI from the day the
 * service was written; nothing printed them, which in this tree is the same as
 * not having them.  So: both buses asked, only the open one printed, and no
 * line at all on a board with no SPI link - which is most of them.
 *
 * Deltas are the operator's arithmetic, deliberately: two `link` calls around
 * a transfer say what that transfer cost, and a rate printed here would be a
 * rate over the whole life of the link, which is the number nobody wants.
 */
static void print_spilink(void)
{
    for (int bus = 2; bus <= 3; bus++) {
        ag_spilink_stats_t sp;
        if (ag_spilink_stats(bus, &sp) != AG_OK || !sp.open) {
            continue;
        }
        ag_console_printf("spi%d    %u blocks clocked by the peer, %u with "
                          "bytes in them\n",
                          bus, (unsigned)sp.blocks_clocked,
                          (unsigned)sp.blocks_data);
        /*
         * Only when non-zero, all four of them.  A line of zeros on a healthy
         * link is noise an operator learns to skip, and then the one run where
         * it is not zero gets skipped too.
         */
        if (sp.blocks_unarmed != 0u || sp.bytes_dropped != 0u ||
            sp.timeouts != 0u || sp.stalls != 0u) {
            ag_console_printf("        %u into nothing armed, %u bytes with no "
                              "room, %u timeouts, %u stalls\n",
                              (unsigned)sp.blocks_unarmed,
                              (unsigned)sp.bytes_dropped, (unsigned)sp.timeouts,
                              (unsigned)sp.stalls);
        }
        /* a framed link (ABI 0.60): what its own acknowledgements did */
        if (sp.frame != 0u) {
            ag_console_printf("        frame %u B: %u bad, %u holes asked for, "
                              "%u sent again, %u restarts of the other end\n",
                              (unsigned)sp.frame, (unsigned)sp.frames_bad,
                              (unsigned)sp.seq_gaps, (unsigned)sp.rewinds,
                              (unsigned)sp.peer_restarts);
        }
        if (sp.recoveries != 0u || sp.errors != 0u || sp.task_stuck) {
            ag_console_printf("        %u rebuilds, %u errors (last %d)%s\n",
                              (unsigned)sp.recoveries, (unsigned)sp.errors,
                              (int)sp.last_error,
                              sp.task_stuck ? ", TASK STUCK" : "");
        }
    }
}

/*
 * THE UART WIRES' OWN COUNTERS (29 Sep 2026).  The C3 counted 2485 bad frames
 * and 3088 holes from the Zero, and "bad" is two different faults: bytes that
 * never arrived - a receive that fell behind the line, the DMA going round
 * over what was not yet read - and bytes that arrived wrong, which is the
 * line.  The port has counted both apart since it was written; nothing
 * printed them.  Only a port with a link, and only what is not zero.
 */
static void print_uartlink(void)
{
    for (int port = 0; port < 3; port++) {
        ag_spilink_stats_t u;
        if (ag_uartlink_stats(port, &u) != AG_OK) {
            continue;
        }
        uint32_t dd[8];
        ag_port_uart_dma_diag(port, dd);
        if (dd[0] != 0u) {
            ag_console_printf("uart%d   by DMA: %u interrupts (%u found "
                              "nothing), %u descriptors filled whole (%u "
                              "with a length, %u none), %u ended by an idle "
                              "line (%u owner given, %u end only)\n",
                              port, (unsigned)dd[0], (unsigned)dd[1],
                              (unsigned)dd[2], (unsigned)dd[4],
                              (unsigned)dd[5], (unsigned)dd[3],
                              (unsigned)dd[6], (unsigned)dd[7]);
        }
        if (u.short_frames == 0u && u.blocks_unarmed == 0u &&
            u.stalls == 0u && u.timeouts == 0u) {
            continue;
        }
        ag_console_printf("uart%d   %u frames short of bytes (%u bytes "
                          "missing), %u the right length and wrong; the "
                          "receive fell behind %u times; %u skipped\n",
                          port, (unsigned)u.short_frames, (unsigned)u.wire_us,
                          (unsigned)u.blocks_unarmed, (unsigned)u.stalls,
                          (unsigned)u.timeouts);
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
    /*
     * THE TWO NUMBERS THAT WERE MISSING WHEN THIS SHIPPED, and here the words
     * matter more than the figures.  Both were counted inside the driver while
     * no command printed either, which is how a link that was quietly throwing
     * messages away looked identical to a link that was working: the counter
     * existed, so the defect was assumed to be visible, and nobody could see
     * it.
     *
     * So they are printed apart and said out loud, because a reader has to be
     * able to tell them from each other at a glance and neither from loss:
     * refused is the peer sending it again (normal under load - the window
     * doing its job), dropped is STREAM only and is that class working as
     * designed.  Neither is a gap, a duplicate or a bad frame, and a reader who
     * takes either for wire loss will look in the wrong place.
     */
    ag_console_printf("queue   %u refused, %u dropped\n",
                      (unsigned)st.rx_refused, (unsigned)st.rx_dropped);
    ag_console_puts("        refused: no room for it, so the peer sends it "
                    "again - normal under load\n");
    ag_console_puts("        dropped: stream only, which is never acknowledged "
                    "- by design\n");
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
    uint32_t relayed = 0, dropped = 0, held = 0;
    ag_linkprov_relay_counts(&relayed, &dropped, &held);
    if (relayed != 0u || dropped != 0u) {
        ag_console_printf("relay   %u moved between neighbours, %u dropped, "
                          "%u times a wire waited for room\n",
                          (unsigned)relayed, (unsigned)dropped,
                          (unsigned)held);
    }
    uint32_t sp = 0, sl = 0, sg = 0, ss = 0, sq = 0, sc = 0;
    ag_noderdev_screen_counts(&sp, &sl, &sg, &ss, &sq, &sc);
    if (sp != 0u || sl != 0u) {
        ag_console_printf("screen  %u passes drawn here, %u pieces not drawn, "
                          "the longest wait between two %u ms\n",
                          (unsigned)sp, (unsigned)sl, (unsigned)sg);
    }
    if (sl != 0u) {
        uint32_t nd = 0, np = 0, mop = 0, mlen = 0, mf = 0;
        ag_noderdev_draw_misses(&nd, &np, &mop, &mlen, &mf);
        ag_console_printf("misses  %u with no device, %u that drew nothing "
                          "(last op %u, %u bytes), %u faults\n",
                          (unsigned)nd, (unsigned)np, (unsigned)mop,
                          (unsigned)mlen, (unsigned)mf);
    }
    uint32_t fs = 0, fsl = 0, fst = 0, fsms = 0, fmax = 0, fnr = 0;
    ag_noderdev_frames_seen(&fs, &fsl, &fst, &fsms, &fmax, &fnr);
    if (fs != 0u) {
        ag_console_printf("seen    %u frames drawn whole here; %u waits of "
                          "250 ms or more between two (%u ms together), %u of "
                          "a second or more, the longest %u ms (before "
                          "frame %u)\n",
                          (unsigned)fs, (unsigned)fsl, (unsigned)fsms,
                          (unsigned)fst, (unsigned)fmax, (unsigned)fnr);
    }
    uint32_t tfc = 0, bnp = 0, bno = 0, braw = 0;
    int32_t  berr = 0;
    ag_noderdev_blit_counts(&tfc, &bnp, &bno, &berr, &braw);
    if (tfc != 0u) {
        ag_console_printf("to far  %u blits for the neighbour's screen: %u with "
                          "none chosen, %u it would not open (last %d), %u "
                          "raw\n",
                          (unsigned)tfc, (unsigned)bnp, (unsigned)bno,
                          (int)berr, (unsigned)braw);
    }
    uint32_t whole = 0, passes = 0, pass_wait = 0;
    rdp_counts(&whole, &passes, &pass_wait);
    if (passes != 0u) {
        uint32_t pto = 0, pmax = 0, pun = 0;
        ag_noderdev_paint_waits(&pto, &pmax, &pun);
        ag_console_printf("painter %u whole frames sent in %u passes, the far "
                          "end took %u ms a pass\n",
                          (unsigned)whole, (unsigned)passes,
                          (unsigned)(pass_wait / 1000u));
        uint32_t tpm = 0, tsm = 0, tym = 0, tsn = 0;
        rdp_times(&tpm, &tsm, &tym, &tsn);
        ag_console_printf("        passes took %u ms: %u in sending %u payloads, "
                          "%u waiting for the far end\n",
                          (unsigned)tpm, (unsigned)tsm, (unsigned)tsn,
                          (unsigned)tym);
        uint32_t tfw = 0, tmw = 0, tww = 0, tpr = 0;
        rdp_waits(&tfw, &tmw, &tww, &tpr);
        ag_console_printf("        between passes: %u ms waiting for the "
                          "program's frame, %u for the least pass, %u for "
                          "anything; %u presents\n",
                          (unsigned)tfw, (unsigned)tmw, (unsigned)tww,
                          (unsigned)tpr);
        ag_console_printf("        %u times all of it again, %u waits ran "
                          "out, the longest %u ms, %u markers refused\n",
                          (unsigned)rdp_redraws(), (unsigned)pto,
                          (unsigned)pmax, (unsigned)pun);
    }
    if (ss != 0u || sq != 0u) {
        uint32_t wm = 0, wg = 0;
        ag_noderdev_snd_writes(&wm, &wg);
        ag_console_printf("sound   the longest wait between two periods %u ms,"
                          " its queue up to %u of %u; the converter's "
                          "longest write %u ms, longest between two %u ms\n",
                          (unsigned)ss, (unsigned)sq, (unsigned)sc,
                          (unsigned)wm, (unsigned)wg);
    }
    uint32_t rw = 0, rl = 0;
    ag_noderdev_wait_counts(&rw, &rl);
    if (rw != 0u || rl != 0u) {
        ag_console_printf("waits   the reader for picture room %u times; %u "
                          "periods of sound late\n",
                          (unsigned)rw, (unsigned)rl);
    }
    uint32_t bw = 0, bc = 0, br = 0, bh = 0;
    ag_linkprov_bg_counts(&bw, &bc, &br, &bh);
    if (bw != 0u || bc != 0u || br != 0u || bh != 0u) {
        ag_console_printf("backgr  %u sends held for the window, %u answers "
                          "given, %u counts started again, %u held here "
                          "for want of time\n",
                          (unsigned)bw, (unsigned)bc, (unsigned)br,
                          (unsigned)bh);
    }
    uint32_t dry = 0, dry_us = 0;
    ag_pcmhw_dry(&dry, &dry_us);
    uint32_t dry_at[8], pauses = 0;
    const uint32_t dry_n = ag_pcmhw_dry_at(dry_at, 8u, &pauses);
    if (dry != 0u || pauses != 0u) {
        ag_console_printf("dry     the converter ran out %u times, the "
                          "longest %u ms; %u pauses of 300 ms or more\n",
                          (unsigned)dry, (unsigned)(dry_us / 1000u),
                          (unsigned)pauses);
    }
    if (dry_n != 0u) {
        /* when, since this board's first sound: the start or the middle */
        ag_console_printf("        at");
        for (uint32_t i = 0; i < dry_n; i++) {
            const uint32_t ms = dry_at[i] & 0x7FFFFFFFu;
            ag_console_printf(" %u.%u s%s", (unsigned)(ms / 1000u),
                              (unsigned)(ms % 1000u / 100u),
                              (dry_at[i] & 0x80000000u) ? " (pause)" : "");
        }
        ag_console_printf("\n");
    }
    uint32_t pp = 0, pl = 0, pf = 0, pq = 0;
    ag_noderdev_pour_counts(&pp, &pl, &pf, &pq);
    if (pp != 0u || pl != 0u) {
        ag_console_printf("poured  %u periods here, %u lost (%u to a full queue),"
                          " %u frames squeezed out\n",
                          (unsigned)pp, (unsigned)pl, (unsigned)pf,
                          (unsigned)pq);
        uint32_t       lat[8];
        uint8_t        lwhy[8];
        const uint32_t ln = ag_noderdev_lost_at(lat, lwhy, 8u);
        static const char *const why_s[] = {"?", "drew nothing", "no device",
                                            "converter slow", "no memory",
                                            "queue full"};
        if (ln != 0u) {
            ag_console_printf("        lost at");
            for (uint32_t i = 0; i < ln; i++) {
                ag_console_printf(" %u.%u s (%s)", (unsigned)(lat[i] / 1000u),
                                  (unsigned)(lat[i] % 1000u / 100u),
                                  why_s[(lwhy[i] <= 5u) ? lwhy[i] : 0u]);
            }
            ag_console_printf("\n");
        }
    }
    ag_console_printf("reader  above applications for 2 ms in 10; ran over "
                      "%u times, the relay %u\n",
                      (unsigned)ag_linkprov_reader_overruns(),
                      (unsigned)ag_linkprov_relay_overruns());
    print_spilink();
    print_uartlink();
}

static int link_status(void)
{
    if (!ag_linkprov_bound()) {
        ag_console_puts("no link bound\n");
        ag_console_puts("  `dev` lists what is here; `link use <dev>` binds "
                        "one, and again for a second neighbour\n");
        ag_console_puts("  a board may also be named: `link use <node>`, which "
                        "reaches one beyond the neighbour\n");
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
    /*
     * AND WHO A NEW COMMAND ACTUALLY TALKS TO.
     *
     * Said here because it is a standing condition that outlives the command
     * that set it: `link use zero` leaves a board two hops away as the
     * destination, and `link run` after it goes there rather than next door.
     * Only when it differs from this wire's own neighbour, which is the case
     * nobody needs told twice.
     */
    if (ag_linkprov_target() != 0) {
        const int32_t cw = ag_linkprov_current();
        const char   *np = (cw >= 0) ? ag_node_peer_name_at(cw) : NULL;
        if (np == NULL || ag_node_hash(np) != ag_linkprov_target()) {
            ag_console_puts("  a board beyond the neighbour is selected; "
                            "`link nodes` marks it\n"
                            "  `link use none` clears it\n");
        }
    }
    /*
     * The reader's remaining stack, per wire.  Here rather than in some
     * diagnostic corner because of what running out of it did: the board
     * serving a file read rebooted, and the only visible symptom was an error
     * on the OTHER board fifteen seconds later.
     */
    for (int32_t w = 0; w < (int32_t)AG_LINK_WIRES; w++) {
        const char *nm = ag_linkprov_name_at(w);
        if (nm == NULL) {
            continue;
        }
        const uint32_t left = ag_linkprov_reader_stack_left(w);
        ag_console_printf("reader  %s: %u stack bytes unused%s\n", nm,
                          (unsigned)left,
                          (left < 1024u) ? "  <- close to the edge" : "");
    }
    print_stats();
    return 0;
}

/*
 * Answer whatever arrives, for `secs` seconds.  The other half of `ping`.
 *
 * One message at a time, out of the mailbox, which is what the mailbox is for:
 * an echo does not care what arrived while it was busy, and it is the ping -
 * counting on the receive path, see s_ping_run - that has to see every answer.
 */
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
        link_origin_t who;
        memset(&who, 0, sizeof(who));
        who.wire = -1;
        const int32_t n = shell_wait(&who, buf, sizeof(buf), 200);
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
        /*
         * THE OTHER HALF OF `ping`, and the answer goes back to the node that
         * asked, out the wire its question arrived on.  An echo that says "for
         * whoever is reading" is taken by a board in between, and a ping across
         * two hops then reports nothing answered on a wire that is fine.
         */
        buf[0] = (uint8_t)LINK_TAG_REP;
        if (reply_to(&who, AG_LINK_CONTROL, buf, (size_t)n) > 0) {
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
 *
 * WHAT IS MEASURED, and it is a ROUND TRIP: one exchange is this end's shell
 * sending a request, the far end's shell reading it and answering, and the
 * answer coming back.  The number below is therefore exchanges per second, and
 * half of it is not a one-way latency - it contains both wires and both ends'
 * shells.  The caveat is printed with the number for that reason: a figure
 * whose halves are both counted once reads exactly like a one-way time, and the
 * difference between them is a factor of two in whatever is sized from it.
 */
#define LINK_PING_STALL_MS 2000u

static int link_ping(uint32_t count, uint8_t cls)
{
    if (ag_linkprov_listen(LINK_CHAN_SHELL, on_shell_msg, NULL) != AG_OK) {
        ag_console_puts("channel 1 is taken\n");
        return 1;
    }

    /*
     * The request is the same message every time, so it is built once: the run
     * number is what tells this command's answers from anything else's, and the
     * width is fixed so that the exchange size printed with the rate is one
     * number rather than one per message.
     */
    uint8_t        out[64];
    const uint32_t run = ++s_ping_next; /* never 0: zero means "not armed" */
    const int plen = snprintf((char *)out + 1, sizeof(out) - 1,
                              "ping %05u", (unsigned)run);
    if (plen < 1 || (size_t)plen + 2u > sizeof(out)) {
        ag_console_puts("ping: cannot build the request\n");
        return 1;
    }
    out[0] = (uint8_t)LINK_TAG_REQ;
    const uint32_t bytes = (uint32_t)plen + 1u; /* the tag, then the text */

    /*
     * ARMED, and armed in this order: disarm, clear, arm.  The reader task
     * counts only when the armed run matches the one in the message, and the
     * new run number has never been sent, so an increment racing this cannot
     * land on the fresh count.
     */
    s_ping_run = 0u;
    s_ping_got = 0u;
    s_ping_run = run;

    ag_console_printf("ping: %u messages on %s, %u bytes each way\n",
                      (unsigned)count,
                      (cls == AG_LINK_STREAM)  ? "stream"
                      : (cls == AG_LINK_BULK) ? "bulk"
                                              : "control",
                      (unsigned)bytes);

    const uint32_t start = now_ms();
    uint32_t       sent = 0;
    uint32_t       seen = 0;      /* what the count was when we last looked  */
    uint32_t       moved = 0;     /* when it last moved - the stall clock    */
    bool           stopped = false;

    for (uint32_t i = 0; i < count; i++) {
        /*
         * A full window is back-pressure, not a failure: pump and try again.
         * Bounded, because a link whose far end has gone away would otherwise
         * spin here for ever and look like a hung shell.
         */
        /*
         * A NEW conversation, so it is addressed to the node `link use` last
         * named - which makes a ping a two-hop test: with a far leaf selected,
         * the request is relayed by the board in between and the answer below
         * is addressed straight back here.
         */
        int32_t s = -AG_EAGAIN;
        for (int t = 0; t < 200 && s == -AG_EAGAIN; t++) {
            s = ag_linkprov_send(LINK_CHAN_SHELL, cls, out, bytes);
            if (s == -AG_EAGAIN) {
                ag_linkprov_poll(5);
            }
        }
        if (s < 0) {
            ag_console_printf("send %u: %d\n", (unsigned)i, (int)s);
            break;
        }
        sent++;
        if (sent == 1u) {
            moved = now_ms(); /* the stall clock starts with the first question */
        }

        /*
         * ONE EXCHANGE OUTSTANDING, and that is this instrument measuring the
         * wire rather than the far end's shell: `link echo` keeps one request
         * in its mailbox and answers them one at a time, so a burst of `count`
         * requests would overflow that mailbox and the missing answers would be
         * that mailbox's fault - the same mistake as the one being fixed, one
         * board along.  Waiting for this answer is also what makes the elapsed
         * time a round trip.
         *
         * The wait is for the COUNT, not for a message in the mailbox, so
         * nothing that arrives is lost on the way to the total.
         */
        while (s_ping_got < sent) {
            const uint32_t now = now_ms();
            if (s_ping_got != seen) {
                seen = s_ping_got;
                moved = now;
            }
            if ((uint32_t)(now - moved) >= LINK_PING_STALL_MS) {
                stopped = true;
                break;
            }
            if (ag_console_getch(0) >= 0) {
                stopped = true;
                break;
            }
            ag_linkprov_poll(2);
        }
        if (stopped) {
            break;
        }
    }

    /*
     * Disarmed BEFORE the count is read, so the number printed is the number
     * this run ended with: an answer still in flight is no longer this
     * command's, and cannot be the next one's either - it carries this run.
     */
    s_ping_run = 0u;
    const uint32_t got = s_ping_got;

    uint32_t ms = (uint32_t)(now_ms() - start);
    if (ms == 0u) {
        ms = 1u; /* a run can be shorter than the clock; the rate is not 0/0 */
    }

    /*
     * THE NUMBER, SAID AS A RATE AND WITH THE EXCHANGE SIZE BESIDE IT.  A tally
     * on its own says nothing about the link: exchanges per second over 11-byte
     * messages and over 240-byte ones are different measurements, and elapsed
     * time is the other half of what makes a count readable.
     */
    ag_console_printf("%u of %u came back in %u ms - %u exchanges/s, %u B "
                      "each way\n",
                      (unsigned)got, (unsigned)count, (unsigned)ms,
                      (unsigned)(((uint64_t)got * 1000u) / ms),
                      (unsigned)bytes);
    ag_console_puts("  round trip: question, far shell, answer - not a one-way "
                    "time, do not halve it\n");

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

/*
 * `link flood <secs> [size] [prio] [bulk]`: the wire filled with bytes that mean
 * nothing, to measure it beside whatever else is running.  See the sink in
 * linkprov.c.  Priority 2 by default: an application's LOW class, so a real
 * program on this board comes first for the processor.
 */
static int link_flood(int argc, char **argv)
{
    const uint32_t secs = (argc > 2) ? (uint32_t)atoi(argv[2]) : 10u;
    const uint32_t size = (argc > 3) ? (uint32_t)atoi(argv[3]) : 0u;
    const unsigned prio = (argc > 4) ? (unsigned)atoi(argv[4]) : 2u;
    const bool     bulk = argc > 5 && ag_path_icmp(argv[5], "bulk") == 0;
    ag_console_printf("flood: %u s at priority %u, %s, to %s\n",
                      (unsigned)secs, prio, bulk ? "bulk" : "background",
                      ag_linkprov_active());
    ag_linkprov_flood_t f;
    const ag_err_t e = ag_linkprov_flood(
        ((secs == 0u) ? 1u : secs) * 1000u, size, prio,
        bulk ? (uint8_t)AG_LINK_BULK : (uint8_t)AG_LINK_BACKGROUND, &f);
    if (e != AG_OK) {
        ag_console_printf("flood: %d\n", (int)e);
        return 1;
    }
    const uint32_t ms = (f.sent_ms == 0u) ? 1u : f.sent_ms;
    ag_console_printf("sent     %u B in %u frames of %u in %u ms = %u KB/s; "
                      "the wire was full %u times\n",
                      (unsigned)f.sent_bytes, (unsigned)f.sent_frames,
                      (unsigned)f.size, (unsigned)f.sent_ms,
                      (unsigned)((uint64_t)f.sent_bytes * 1000u / ms / 1024u),
                      (unsigned)f.full);
    if (!f.answered) {
        ag_console_puts("arrived  the far end did not say\n");
    } else {
        const uint32_t gms = (f.got_ms == 0u) ? 1u : f.got_ms;
        ag_console_printf("arrived  %u B in %u frames over %u ms = %u KB/s, "
                          "by the far end's count\n",
                          (unsigned)f.got_bytes, (unsigned)f.got_frames,
                          (unsigned)f.got_ms,
                          (unsigned)((uint64_t)f.got_bytes * 1000u / gms /
                                     1024u));
        ag_console_printf("checked  %u frames out of turn, %u with a wrong "
                          "byte\n",
                          (unsigned)f.got_order, (unsigned)f.got_wrong);
    }
    print_stats();
    return (f.answered && f.got_bytes == f.sent_bytes && f.got_order == 0u &&
            f.got_wrong == 0u)
               ? 0
               : 1;
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
            ag_console_puts("usage: link use <dev>|none|<node>\n");
            return 1;
        }
        /*
         * A board told it is not in a cluster says so, rather than binding
         * and then refusing everything one service at a time.
         */
        if (!ag_cluster_allowed()) {
            ag_console_puts("this board is not in a cluster\n");
            ag_console_puts("  [cluster] multiboard = none in SYSTEM.CFG "
                            "says so; set it to yes to join one\n");
            return 1;
        }

        /*
         * THREE FORMS, AND THE THIRD ONE IS NEW.
         *
         *   none         let the selected wire go and forget who was named
         *   nodeN        a wire device: bind it, and talk to ITS neighbour
         *   <node>       a board by name, which need not be a neighbour
         *
         * A NAME IS TWO FACTS.  A board two hops away is reached THROUGH the
         * neighbour that told us about it, so the wire has to be selected - the
         * first hop - AND the frame has to say which node it is for, so that
         * the board in between passes it on instead of taking it.  `link use
         * zero` used to do only the first, and every frame after it said "for
         * whoever is reading", which the middle board is.
         *
         * The target is CLEARED for a wire and for `none`, and that is not
         * tidiness: without it, a far node named a moment ago would still be
         * the destination of the next `link run`, sent out a wire that does not
         * reach it.
         */
        const char *want = argv[2];

        if (ag_path_icmp(want, "none") == 0) {
            const ag_err_t err = ag_node_bind(want, true);
            if (err != AG_OK) {
                ag_console_printf("link use %s: %d\n", want, (int)err);
                return 1;
            }
            ag_linkprov_set_target(0);
            ag_console_printf("link is %s\n", ag_linkprov_active());
            ag_console_puts("  nothing named: commands go to the neighbour in "
                            "hand\n");
            return 0;
        }

        /* The wire, if the word is one of ours: `node0` is what `dev` lists
         * and what a script already knows. */
        int32_t wire = ag_linkprov_index_of(want);
        bool    far = false;

        /* Otherwise a neighbour's node name - what the board calls itself,
         * which is the identity that survives a reboot and a re-cable. */
        if (wire < 0) {
            for (int32_t w = 0; w < (int32_t)AG_LINK_WIRES && wire < 0; w++) {
                const char *peer = ag_node_peer_name_at(w);
                if (peer != NULL && peer[0] != '\0' &&
                    ag_path_icmp(peer, want) == 0) {
                    wire = w;
                }
            }
        }

        /* And finally a board beyond them, which the cluster transport heard
         * about from a neighbour and which names the wire it is heard through
         * (see ag_clu_peers_seen).  That wire index is THIS board's - the
         * sender's indices would name a different board here. */
        if (wire < 0 && ag_clu_api_table.peers_seen != NULL) {
            ag_clu_node_t  seen[AG_LINK_WIRES * 2u];
            int32_t        via[AG_LINK_WIRES * 2u];
            const uint32_t scap = (uint32_t)(sizeof(seen) / sizeof(seen[0]));
            const int32_t  m = ag_clu_api_table.peers_seen(seen, via, scap);
            for (int32_t i = 0; i < m && (uint32_t)i < scap; i++) {
                if (via[i] >= 0 && ag_path_icmp(seen[i].name, want) == 0) {
                    wire = via[i];
                    far = true;
                    break;
                }
            }
        }

        /*
         * Bind or select the wire.  A word that is none of the above is still
         * given to the binder, because a link device that is not bound yet
         * answers to its own name and nothing here can tell it from a node
         * name that has not been learned.
         */
        const char *dev = (wire >= 0) ? ag_linkprov_name_at(wire) : want;
        if (dev == NULL) {
            dev = want; /* the wire went between the lookup and here */
        }
        const ag_err_t err = ag_node_bind(dev, true);
        if (err != AG_OK) {
            ag_console_printf("link use %s: %d\n", dev, (int)err);
            return 1;
        }
        wire = ag_linkprov_index_of(dev);

        /*
         * WHO THE NEXT NEW CONVERSATION IS FOR.  A neighbour, by its own name
         * hash - 0 there would mean the same thing today, but the hash keeps
         * meaning it after another wire is selected.  A far node, by ITS hash,
         * which is the whole of what makes the relay pass the frame on.
         */
        uint16_t target = 0;
        if (wire >= 0) {
            target = far ? ag_node_hash(want)
                         : ag_node_hash(ag_node_peer_name_at(wire));
        }
        ag_linkprov_set_target(target);

        /*
         * Say how many wires this board now holds, because `link use` stopped
         * meaning "instead of" and started meaning "as well as" the moment a
         * board could have three neighbours.  Silence there would read as the
         * old behaviour, and the first sign of the difference would be a job
         * spread over more boards than anyone meant to wire up.
         */
        const int32_t wires = ag_linkprov_count();
        if (wires > 1) {
            ag_console_printf("link is %s (%d wires bound)\n",
                              ag_linkprov_active(), (int)wires);
        } else {
            ag_console_printf("link is %s\n", ag_linkprov_active());
        }
        /*
         * And WHICH BOARD the next command talks to, in a sentence rather than
         * a count: "the link is node0" no longer says it, now that a board two
         * hops away can be named through that wire.
         */
        if (far) {
            const char *via = (wire >= 0) ? ag_node_peer_name_at(wire) : NULL;
            ag_console_printf("  next command goes to %s, through %s\n", want,
                              (via != NULL && via[0] != '\0')
                                  ? via
                                  : ag_linkprov_active());
        } else if (target != 0) {
            const char *peer = ag_node_peer_name_at(wire);
            ag_console_printf("  next command goes to %s\n", peer);
        } else {
            ag_console_puts("  the neighbour has not said its name yet; "
                            "commands go to whoever is on that wire\n");
        }
        return 0;
    }

    /*
     * What a job has to work with: the boards, their cores, and what the wire
     * between them will carry.
     *
     * The last line is the one worth having in front of you before deciding to
     * spread anything: it turns the link into the number that decides whether a
     * piece of work should travel at all - how many cycles of a local core a
     * byte over this wire is worth.  Under about that many cycles per byte, the
     * work was cheaper at home, and no amount of cores changes it.
     */
    /*
     * LET GO OF ONE NAMED WIRE.
     *
     * `link use none` drops the selected one, which is what it always meant
     * when there was only ever one.  With several, the wire somebody wants
     * rid of is usually not the one that happens to be selected, and saying
     * which is the difference between letting go of a board and letting go of
     * whichever board was last spoken to.
     */
    /*
     * The standby pair, by hand.  `link standby` hands the work over now and
     * `link wake` asks for it back, which is what the timer does on its own -
     * and which is the only way to try the shape without waiting out the
     * quiet period, so it exists for the bench as much as for a person.
     */
    if (ag_path_icmp(argv[1], "standby") == 0) {
        const ag_err_t e = ag_standby_handover();
        if (e != AG_OK) {
            ag_console_printf("link standby: %d\n", (int)e);
            return 1;
        }
        return 0;
    }
    if (ag_path_icmp(argv[1], "wake") == 0) {
        const ag_err_t e = ag_standby_fetch();
        if (e != AG_OK) {
            ag_console_printf("link wake: %d\n", (int)e);
            return 1;
        }
        ag_console_puts("asked for our work back\n");
        return 0;
    }

    if (ag_path_icmp(argv[1], "drop") == 0) {
        if (argc < 3) {
            ag_console_puts("usage: link drop <dev>\n");
            return 1;
        }
        const ag_err_t err = ag_linkprov_drop(argv[2]);
        if (err != AG_OK) {
            ag_console_printf("link drop %s: %d\n", argv[2], (int)err);
            return 1;
        }
        const int32_t left = ag_linkprov_count();
        ag_console_printf("dropped %s; %d wire%s left\n", argv[2], (int)left,
                          (left == 1) ? "" : "s");
        return 0;
    }

    if (ag_path_icmp(argv[1], "nodes") == 0) {
        /*
         * One more than the wires: the list the cluster layer builds starts
         * with THIS board.  Four was exactly right while a board had one
         * neighbour and is one short of the smallest interesting cluster now,
         * and the entry it would have dropped is a whole board's cores.
         */
        ag_clu_node_t nodes[AG_LINK_WIRES + 1u];
        const int32_t cap = (int32_t)(sizeof(nodes) / sizeof(nodes[0]));
        const int32_t n = ag_clu_api_table.nodes(nodes, (uint32_t)cap);
        uint32_t      cores = 0;
        /*
         * WHICH ONE A NEW COMMAND WOULD TALK TO, marked as it is listed.
         *
         * By the target hash rather than by the wire, because that is what an
         * outgoing frame carries: `link use cyd` names a board, and the mark
         * has to be on the board, not on the wire it happens to be reached
         * through.  ag_node_hash() of a name is what the link layer stamps.
         */
        const uint16_t sel = ag_linkprov_target();
        for (int32_t i = 0; i < n && i < cap; i++) {
            const bool chosen =
                (sel != 0) && (ag_node_hash(nodes[i].name) == sel);
            ag_console_printf("%-9s %u core%s  %u thread%s%s%s\n",
                              nodes[i].name, (unsigned)nodes[i].cores,
                              (nodes[i].cores == 1) ? "" : "s",
                              (unsigned)nodes[i].threads,
                              (nodes[i].threads == 1) ? "" : "s",
                              nodes[i].here ? "  (here)" : "",
                              chosen ? "  (selected)" : "");
            cores += nodes[i].cores;
        }
        ag_console_printf("%d node%s, %u core%s in all\n", (int)n,
                          (n == 1) ? "" : "s", (unsigned)cores,
                          (cores == 1) ? "" : "s");

        /*
         * AND WHAT IS BEYOND THEM.
         *
         * A leaf holds one wire and therefore one neighbour, so the list above
         * says "only me and the one next door" while a third board is running
         * two hops away.  The middle knows, and says so - net/nodeclu.c sends
         * each neighbour the names it knows - and this is where an operator
         * reads it.
         *
         * USABLE BY NAME, which is what changed: a frame carries a destination
         * now, so `link use <name>` selects the wire that reaches the board in
         * between AND addresses the far board itself, and the request is
         * relayed.  Still NOT counted with the ones above, because that list is
         * the wires this board holds and the job layer picks a node from it: a
         * thread placed "on" a far board through the neighbour's index would
         * land on the neighbour.
         */
        if (ag_clu_api_table.peers_seen != NULL) {
            ag_clu_node_t seen[AG_LINK_WIRES * 2u];
            int32_t       via[AG_LINK_WIRES * 2u];
            const uint32_t scap =
                (uint32_t)(sizeof(seen) / sizeof(seen[0]));
            const int32_t m = ag_clu_api_table.peers_seen(seen, via, scap);
            if (m > 0) {
                ag_console_puts("beyond them, seen through a neighbour "
                                "(`link use <name>` talks to one):\n");
                for (int32_t i = 0; i < m && (uint32_t)i < scap; i++) {
                    const char *far = (via[i] >= 0)
                                          ? ag_node_peer_name_at(via[i])
                                          : NULL;
                    const char *wire = (via[i] >= 0)
                                           ? ag_linkprov_name_at(via[i])
                                           : NULL;
                    ag_console_printf(
                        "%-9s %u core%s  via %s%s\n", seen[i].name,
                        (unsigned)seen[i].cores,
                        (seen[i].cores == 1) ? "" : "s",
                        (far != NULL && far[0] != '\0')
                            ? far
                            : ((wire != NULL) ? wire : "?"),
                        ((sel != 0) &&
                         (ag_node_hash(seen[i].name) == sel))
                            ? "  (selected)"
                            : "");
                }
            }
        }

        /*
         * AND THE WORD THAT CHANGES IT, in one line, because the mark above is
         * what a reader sees and this is what a reader types.  `none` is said
         * too: leaving a far node selected is the state that surprises, since
         * the next `link run` would then go two hops away.
         */
        if (sel == 0) {
            ag_console_puts("nothing selected: commands go to the neighbour in "
                            "hand\n  `link use <name>` names one; `link use "
                            "none` also clears it\n");
        } else {
            ag_console_puts("the marked node is what a new command talks to\n"
                            "  `link use <name>|nodeN` changes it; `link use "
                            "none` clears it\n");
        }

        ag_link_info_t info;
        if (ag_linkprov_info(&info) == AG_OK) {
            const ag_sysinfo_t *si = ag_sysinfo();
            const uint32_t hz = (si != NULL && si->cpu_hz > 0) ? si->cpu_hz
                                                               : 240000000u;
            const uint32_t per_byte =
                (info.bytes_per_sec > 0) ? (hz / info.bytes_per_sec) : 0;
            /*
             * Name the wire when there is more than one of them.  The numbers
             * below are the SELECTED wire's, and on a board holding three a
             * line that just says "wire" invites reading them as the
             * cluster's - which they are not, and the slowest wire is the one
             * that decides whether work should travel.
             */
            if (ag_linkprov_count() > 1) {
                ag_console_printf("wires ");
                for (int32_t w = 0; w < (int32_t)AG_LINK_WIRES; w++) {
                    const char *nm = ag_linkprov_name_at(w);
                    if (nm != NULL) {
                        ag_console_printf("%s%s", nm,
                                          (w == ag_linkprov_current()) ? "* "
                                                                       : " ");
                    }
                }
                ag_console_puts("  (* is the one these numbers are for)\n");
            }
            ag_console_printf("wire  %s, %u B per message, %u B/s\n",
                              info.wire, (unsigned)info.mtu,
                              (unsigned)info.bytes_per_sec);
            ag_console_printf("      a byte over it costs %u cycles here; work "
                              "worth less than that per byte stays home\n",
                              (unsigned)per_byte);
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
    if (ag_path_icmp(argv[1], "window") == 0) {
        uint8_t a = 0, m = 0;
        const bool set = argc > 3;
        if (set) {
            a = (uint8_t)atoi(argv[2]);
            m = (uint8_t)atoi(argv[3]);
        }
        ag_noderdev_paint_window(&a, &m, set);
        ag_console_printf("the painter's window: %u passes ahead, %u messages "
                          "unanswered\n", (unsigned)a, (unsigned)m);
        return 0;
    }
    if (ag_path_icmp(argv[1], "trace") == 0) {
        /* `link trace <ms>`: the wire's events in time (net/wtrace.h) */
        const uint32_t ms = (argc > 2) ? (uint32_t)atoi(argv[2]) : 100u;
        if (!wt_start(ms)) {
            ag_console_puts("trace: no memory\n");
            return 1;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(ms + 20u));
        wt_stop();
        const uint32_t n = wt_count();
        ag_console_printf("trace: %u events in %u ms\n", (unsigned)n,
                          (unsigned)ms);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t us = 0, a = 0;
            uint8_t  ev = 0;
            if (wt_get(i, &us, &ev, &a)) {
                ag_console_printf("T %u %s %u\n", (unsigned)us, wt_name(ev),
                                  (unsigned)a);
            }
        }
        ag_console_puts("trace: end\n");
        return 0;
    }
    if (ag_path_icmp(argv[1], "probe") == 0) {
        /* where a UART link's sender spends its time, since the last look */
        for (int port = 0; port < 3; port++) {
            ag_uartlink_probe_t p;
            if (ag_uartlink_probe(port, &p) != AG_OK) {
                continue;
            }
            const uint32_t ms = p.elapsed_us / 1000u;
            ag_console_printf(
                "uart%d over %u ms: %u frames out (%u KB, %u B each), %u in\n",
                port, (unsigned)ms, (unsigned)p.frames,
                (unsigned)(p.wire_bytes / 1024u),
                (unsigned)(p.frames ? p.wire_bytes / p.frames : 0u),
                (unsigned)p.frames_rx);
            ag_console_printf(
                "  %u writes, %u us in them; %u waited for room, %u us\n",
                (unsigned)p.sends, (unsigned)p.send_us,
                (unsigned)p.space_waits, (unsigned)p.space_us);
            ag_console_printf(
                "  sender stopped: %u nothing to send, %u no credit, %u "
                "window full; %u buffer waits, %u us\n",
                (unsigned)p.stop_empty, (unsigned)p.stop_credit,
                (unsigned)p.stop_win, (unsigned)p.settles,
                (unsigned)p.settle_us);
            if (p.acked != 0u) {
                ag_console_printf("  %u frames answered: round trip %u us on "
                                  "average, %u at most; the far end's room "
                                  "%u B on average, %u at least (this end "
                                  "keeps %u)\n",
                                  (unsigned)p.acked, (unsigned)p.rtt_avg_us,
                                  (unsigned)p.rtt_max_us,
                                  (unsigned)p.room_avg, (unsigned)p.room_min,
                                  (unsigned)p.rxcap_peer);
            }
            if (p.small_ooo + p.small_hole + p.small_full != 0u) {
                ag_console_printf("  told under a frame of room: %u for "
                                  "frames past a hole, %u for a hole, %u "
                                  "for a full ring\n",
                                  (unsigned)p.small_ooo,
                                  (unsigned)p.small_hole,
                                  (unsigned)p.small_full);
            }
            uint32_t slow = 0, slow_us = 0, slow_len = 0;
            ag_port_uart_dma_tx_slow(port, &slow, &slow_us, &slow_len);
            if (slow != 0u) {
                ag_console_printf("  DMA transfers slower than 1 B/us: %u, "
                                  "the slowest %u us for %u B\n",
                                  (unsigned)slow, (unsigned)slow_us,
                                  (unsigned)slow_len);
            }
            if (p.dma_subs != 0u) {
                ag_console_printf("  by DMA: %u transfers, %u found the wire "
                                  "quiet, %u us of quiet\n",
                                  (unsigned)p.dma_subs, (unsigned)p.dma_idle,
                                  (unsigned)p.dma_idle_us);
            }
        }
        uint32_t rmax = 0, rslow = 0, rslow_us = 0;
        uint8_t  rchan = 0;
        ag_linkprov_reader_slow(&rmax, &rchan, &rslow, &rslow_us);
        ag_console_printf("reader: the longest message %u us (channel %u); "
                          "%u of 1 ms or more, %u us in all\n",
                          (unsigned)rmax, (unsigned)rchan, (unsigned)rslow,
                          (unsigned)rslow_us);
        return 0;
    }
    if (ag_path_icmp(argv[1], "report") == 0) {
        const bool on = argc > 2 && ag_path_icmp(argv[2], "on") == 0;
        ag_noderdev_report(on);
        ag_linkprov_report(on);
        rdp_report(on);
        ag_console_printf("the screen's report each second: %s\n",
                          on ? "on" : "off");
        return 0;
    }
    if (ag_path_icmp(argv[1], "flood") == 0) {
        return link_flood(argc, argv);
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
        /* Every neighbour's, one wire at a time: the others' stay as they
         * are while one is asked again. */
        int bad = 0, asked = 0;
        for (int32_t w = 0; w < (int32_t)AG_LINK_WIRES; w++) {
            if (ag_linkprov_name_at(w) == NULL) {
                continue;
            }
            asked++;
            const int32_t n = ag_noderdev_attach_on(w, 8000u);
            if (n == -AG_EAGAIN) {
                ag_console_printf("%s: the node there has not said its name "
                                  "yet; try again\n",
                                  ag_linkprov_name_at(w));
                bad++;
                continue;
            }
            if (n < 0) {
                ag_console_printf("%s: link devs: %d\n",
                                  ag_linkprov_name_at(w), (int)n);
                bad++;
                continue;
            }
            ag_console_printf("%d device(s) of %s are in this registry now\n",
                              (int)n, ag_node_peer_name_at(w));
        }
        if (asked == 0) {
            ag_console_puts("link devs: no wire is bound\n");
            return 1;
        }
        ag_console_puts("  `dev` lists them; open one by its qualified name\n");
        return (bad > 0) ? 1 : 0;
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

    ag_console_puts("usage: link [use <dev>|none|<node> | drop <dev> | "
                    "run <path> [args] | "
                    "get <there> <here> | mount | umount | devs | nodevs | "
                    "nodes | name [x] | ping [n] [control|stream|bulk] | "
                    "echo [secs] | poll [ms] | standby | wake]\n");
    return 1;
}
