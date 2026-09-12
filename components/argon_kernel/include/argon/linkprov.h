/*
 * ArgonOS - the node link, as the kernel reaches it.
 *
 * The same shape as the network provider next door (argon/netprov.h) and for
 * the same reason: one indirection, one bound device at a time, and everything
 * above it written against the indirection rather than against a driver.
 *
 * The difference is what is on the far end.  A network provider reaches
 * somebody else's machine; this reaches the other half of THIS machine - a node
 * of the same system, running the same image, whose processes appear in the
 * same `ps` and whose devices appear in the same `dev`.  What crosses it is
 * messages with a channel and a deadline, never bytes: see ag_link_ops_t.
 *
 * NOTHING IS BOUND BY DEFAULT and there is no built-in provider, which is the
 * other difference.  A board with no second node has no link, and every call
 * here answers -AG_ENODEV until `link use <dev>` says otherwise.  A built-in
 * fallback would be a loopback pretending to be a neighbour, and a cluster that
 * silently talks to itself is worse than one that says it is alone.
 *
 * PUMPING.  A loadable driver has no task (see nodelink.c), so the wire only
 * advances while somebody is inside one of these calls.  recv() with a timeout
 * and poll() are how a caller with nothing to say still lets the far end be
 * heard.  Until the ABI grows a service task, a node that must hear an
 * unsolicited request has to poll for it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_LINKPROV_H
#define ARGON_LINKPROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

/* Bind an AG_DEV_LINK device by name, or "none" to let go of the one bound. */
ag_err_t ag_linkprov_use(const char *name);

/* The bound device's name, or "none". */
const char *ag_linkprov_active(void);
bool        ag_linkprov_bound(void);

/* Has anything been heard from the far end since the link came up. */
bool ag_linkprov_up(void);

int32_t ag_linkprov_send(uint8_t chan, uint8_t cls, const void *buf,
                         size_t len);
void    ag_linkprov_poll(uint32_t ms);

/* ------------------------------------------------------------------------ */
/* Receiving: one reader, and handlers per channel                           */
/* ------------------------------------------------------------------------ */
/*
 * THERE IS NO recv().  There was, and it was a mistake with a history: the link
 * hands back whatever arrived next, so two callers reading it steal from each
 * other.  The moment a second consumer appeared - a server for remote runs
 * beside the shell's own ping - six messages in twenty-four went to the wrong
 * one, and the shell reported them lost.  This tree has now made that mistake
 * on a UART twice and on a link once.
 *
 * So exactly one task reads, here, and hands each message to the handler for
 * its channel.  A channel is one conversation; a consumer takes one and hears
 * nothing else.
 *
 * A HANDLER RUNS ON THE READER TASK, which has two consequences worth knowing
 * rather than discovering.  It must not block for long: while it is inside,
 * nothing else on this link is read.  And it must not call ag_linkprov_listen.
 * Sending IS allowed and is the normal thing to do - that is how an answer is
 * given.
 */
typedef void (*ag_link_rx_fn)(uint8_t chan, const void *buf, uint16_t len,
                              void *ctx);

ag_err_t ag_linkprov_listen(uint8_t chan, ag_link_rx_fn fn, void *ctx);

/* Stop listening on a channel.  Safe on one nobody holds. */
void ag_linkprov_unlisten(uint8_t chan);

/* Is the reader task up.  It starts with the first bind and stops with the
 * last unbind. */
bool ag_linkprov_reading(void);

/*
 * True only on the reader task itself.  Anything that sends a request and then
 * waits for its answer must refuse to run here: the answer is delivered by
 * this task, so waiting on it waits for itself.
 */
bool ag_linkprov_in_reader(void);

ag_err_t ag_linkprov_stats(ag_link_stats_t *out);

#endif /* ARGON_LINKPROV_H */
