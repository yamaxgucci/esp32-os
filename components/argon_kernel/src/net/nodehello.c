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

static ag_err_t send_hello(uint8_t op)
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
        const int32_t r = ag_linkprov_send(NH_CHAN, AG_LINK_CONTROL, buf,
                                           (uint16_t)(NH_HDR + n));
        if (r >= 0) {
            return AG_OK;
        }
        if (r != -AG_EAGAIN) {
            return (ag_err_t)r;
        }
        ag_linkprov_poll(2);
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
        ag_node_set_peer_name(name);
        (void)send_hello(NH_TELL); /* an answer, which nobody answers */
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
     */
    return send_hello(NH_ASK);
}
