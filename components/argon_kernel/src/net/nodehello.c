/*
 * ArgonOS - the two ends of a link telling each other who they are.
 *
 * Channel 0, which is the one reserved from the start for the link talking
 * about itself rather than carrying anything.  Nothing here is a request for
 * work: it is the smallest fact each side needs before any qualified name
 * (`n2.pcm0`) or any listing that says WHERE something is can mean anything.
 *
 * A hello is tagged as a question or an answer, and only a question is answered.
 * That is not caution for its own sake - the first draft of the shell's ping
 * echoed messages back unchanged, both ends mistook their own echo for a
 * request, and they bounced one message at each other until the window filled.
 * One byte prevents it, so it is here from the start.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/nodehello.h>

#include <string.h>

#include <argon/libc.h>
#include <argon/linkprov.h>
#include <argon/log.h>
#include <argon/kernel.h>
#include <argon/nodename.h>

#include <argon/port/task.h>
#include <argon/port/time.h>

#define NH_CHAN 0u

enum {
    NH_ASK = 1, /* "who are you"; my own name follows   */
    NH_TELL = 2, /* the answer; nobody answers an answer */
};

#define NH_HDR 2u

static bool s_listening;

/*
 * After the name, what the board is made of: cores and clock.
 *
 * Appended rather than sent as a second message, and after the name rather than
 * before it, so an image built before this reads exactly what it did - it takes
 * the name by its stated length and ignores the tail.  Five bytes, and without
 * them a job that wants every core of every board has nothing to divide the
 * work by; `[cluster] cores =` in a config file would be a number somebody has
 * to keep in step with the hardware by hand.
 */
#define NH_CAPS 5u

/*
 * A hello, and WHO IT IS FOR.
 *
 * A QUESTION IS ASKED OF THE COPPER and therefore carries `dst` zero - "for
 * whoever is reading".  That is not the same thing as "where the board is
 * pointed": a question is "who are you" about the board at the other end of
 * THIS wire, and one addressed to the target asked a node two hops away
 * instead.  An ANSWER goes to the node that asked, which is what `from` is, and
 * which is NOT the target as soon as a board two hops away can be named.
 *
 * WHY THE QUESTION'S ADDRESS IS NOT A STYLE CHOICE.  `dst` zero is the one
 * address the relay never forwards (linkprov's route(): a frame for anybody but
 * this board goes back out), so a frame that arrives with `dst` zero is CERTAIN
 * to have been written by the board on the far end of the wire that carried it.
 * A name is remembered against the wire it arrived on - the slot `link use c3`,
 * `link nodes`, `link devs`, the `ps` label and linkprov's wire_towards() all
 * read - and that slot means "the neighbour of this wire" and nothing else.
 * Addressed to the target, a question was relayed instead, the far node
 * answered "I am zero" to a leaf whose neighbour is c3, and the leaf filed the
 * ANSWER against its wire: the label became `zero.1` and `link use c3` answered
 * -19 because no wire had a neighbour by that name any more.  On a middle board
 * it is worse than a wrong label - wire_towards() routes by those names.
 *
 * Asking the wire is what makes both branches of on_msg() safe, and this is the
 * rule they rely on: a question is always addressed to "whoever is reading", so
 * it is never relayed and its sender is this wire's neighbour; an answer is
 * addressed to the asker, so the only question it can be the answer to is the
 * one this board asked, on this wire, of its own neighbour.
 *
 * THIS IS THE WHOLE OF THE FIX FOR A MIDDLE BOARD THAT FORGETS ITS NEIGHBOURS.
 * A wire is chosen by NAME - linkprov's wire_towards() compares a frame's
 * destination with the name a hello taught it - and names are in RAM, so a
 * middle that restarts starts with none.  It asks both wires who they are, and
 * with the answer addressed to the TARGET the exchange failed in exactly the
 * case that matters:
 *
 *   - the CYD (target = zero, `link use zero`) receives the question, learns
 *     "c3", and answers with the destination ZERO - the wire's own neighbour is
 *     skipped, the frame is addressed past the middle board;
 *   - the middle, holding no names yet, cannot choose a wire for it and drops
 *     it: `relay: a frame for e763 was dropped (no bound wire has a neighbour
 *     by that name yet)`, measured on the bench, e763 being ag_node_hash
 *     ("zero");
 *   - so the middle never learns either neighbour, every relayed frame is
 *     dropped for good, and `ps` across two hops answers -60 for ever while
 *     single-hop work still looks perfect.  THAT ASYMMETRY IS THE SYMPTOM.
 *
 * Addressed to the asker, the same exchange refills the table in one round trip
 * and a relay is routable again before anyone asks it anything.
 *
 * The same rule is stated next door in nodeps.c, which takes ag_linkprov_from()
 * once and copies it into every frame of its answer for this reason.
 */
static ag_err_t send_hello_to(uint16_t dst, uint8_t op)
{
    uint8_t     buf[NH_HDR + AG_NODE_NAME_MAX + NH_CAPS];
    const char *me = ag_node_name();
    size_t      n = strlen(me);
    if (n >= AG_NODE_NAME_MAX) {
        n = AG_NODE_NAME_MAX - 1u;
    }
    buf[0] = op;
    buf[1] = (uint8_t)n;
    memcpy(buf + NH_HDR, me, n);

    const ag_sysinfo_t *si = ag_sysinfo();
    const uint32_t      hz = (si != NULL) ? si->cpu_hz : 0;
    uint8_t            *caps = buf + NH_HDR + n;
    caps[0] = (si != NULL && si->cpu_cores > 0) ? si->cpu_cores : 1u;
    caps[1] = (uint8_t)(hz & 0xffu);
    caps[2] = (uint8_t)((hz >> 8) & 0xffu);
    caps[3] = (uint8_t)((hz >> 16) & 0xffu);
    caps[4] = (uint8_t)((hz >> 24) & 0xffu);
    n += NH_CAPS;

    /* Short and bounded: a hello that cannot go out now is one the next bind
     * or the next `node` will send again. */
    for (int t = 0; t < 100; t++) {
        /*
         * send_to, NOT send: `send` stamps the target `link use` last named,
         * and a question must reach the board on this wire and no other.  With
         * `dst` zero this picks the same wire `send` would have; it only stops
         * the far node from being asked, and answered, in the wire's place.
         */
        const int32_t r = ag_linkprov_send_to(dst, NH_CHAN, AG_LINK_CONTROL,
                                              buf, (uint16_t)(NH_HDR + n));
        if (r >= 0) {
            return AG_OK;
        }
        if (r != -AG_EAGAIN) {
            return (ag_err_t)r;
        }
        /*
         * DO NOT PUMP THE WIRE FROM THE TASK THAT PUMPS IT.
         *
         * The answer to a hello is sent from inside the handler, which runs on
         * the link's reader task - so ag_linkprov_poll here re-enters the
         * driver this task is already inside a recv() of.  The same trap
         * nodeclu's wait_for_room is written around, and it cost a real
         * symptom: the node that ANSWERED a hello never got its answer out, so
         * one board knew its neighbour's name and the other went on calling it
         * "node0" for ever.  Asking is sent from the shell task at bind time
         * and was never affected, which is why the fault looked one-sided.
         */
        if (ag_linkprov_in_reader()) {
            ag_port_task_delay(ag_port_ms_to_ticks(2));
        } else {
            ag_linkprov_poll(2);
        }
    }
    return -AG_ETIMEDOUT;
}

static void on_msg(uint8_t chan, const void *data, uint16_t n, void *ctx)
{
    (void)chan;
    (void)ctx;
    if (n < NH_HDR) {
        return;
    }
    const uint8_t *b = (const uint8_t *)data;
    uint16_t       len = b[1];
    if (len > (uint16_t)(n - NH_HDR)) {
        len = (uint16_t)(n - NH_HDR);
    }

    char name[AG_NODE_NAME_MAX];
    if (len >= sizeof(name)) {
        len = (uint16_t)(sizeof(name) - 1u);
    }
    memcpy(name, b + NH_HDR, len);
    name[len] = '\0';

    /* The tail, if this hello has one.  A node that says nothing about its
     * cores is one running an image from before they were said, and is left at
     * zero - which every reader is told to take as one. */
    const uint16_t stated = b[1];
    if ((uint32_t)n >= (uint32_t)NH_HDR + stated + NH_CAPS) {
        const uint8_t *caps = b + NH_HDR + stated;
        const uint32_t hz = (uint32_t)caps[1] | ((uint32_t)caps[2] << 8) |
                            ((uint32_t)caps[3] << 16) |
                            ((uint32_t)caps[4] << 24);
        ag_node_set_peer_caps(caps[0], hz);
    }

    if (b[0] == NH_ASK) {
        /*
         * WHO ASKED, taken before anything is sent: the answer is addressed to
         * this node, and `from` is only meaningful while the handler runs.
         * Zero - a question that named nobody, which an old image sends - keeps
         * the old meaning and goes out for whoever is reading.
         */
        const uint16_t from = ag_linkprov_from();
        /*
         * The sender of a QUESTION is this wire's neighbour, always: a question
         * is addressed to "whoever is reading" and the relay never forwards
         * such a frame (see send_hello_to).  The ANSWER below is recorded on
         * the same footing - it is addressed to the asker, so the only question
         * it can be answering is the one this board asked on this wire.  That
         * is what makes this slot mean "the board at the end of this copper"
         * rather than "whoever spoke last".
         */
        ag_node_set_peer_name(name);
        /*
         * Said out loud when it fails.  This was `(void)` - an answer that
         * never went out looked exactly like one that was never needed, and
         * the only visible consequence was a node stuck calling its
         * neighbour "node0", three layers away from here.
         */
        const ag_err_t e = send_hello_to(from, NH_TELL);
        if (e != AG_OK) {
            ag_log(AG_LOG_WARN, "hello",
                   "could not answer %s's hello (%d); it will go on calling "
                   "us by the wire's name",
                   name, (int)e);
        }
    } else if (b[0] == NH_TELL) {
        ag_node_set_peer_name(name);
    }
}

ag_err_t ag_nodehello_start(void)
{
    if (!ag_linkprov_bound()) {
        return -AG_ENODEV;
    }
    if (!s_listening) {
        const ag_err_t e = ag_linkprov_listen(NH_CHAN, on_msg, NULL);
        if (e != AG_OK) {
            return e;
        }
        s_listening = true;
    }
    /*
     * Asked, not waited for.  The other node may not be up yet, and a bind that
     * blocks until a neighbour answers is a bind that fails on the board that
     * happens to boot first - which is half of them.
     *
     * Zero is "whoever is on this wire", and that is the whole of what a
     * question asks: `ag_node_autobind` binds one wire at a time and asks on
     * each, so the middle of a cluster asks BOTH of its neighbours who they
     * are.  It is the answer that has to be addressed to the asker - see
     * send_hello_to.
     */
    return send_hello_to(0u, NH_ASK);
}
