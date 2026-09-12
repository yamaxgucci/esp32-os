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
int32_t ag_linkprov_recv(uint8_t *chan, void *buf, size_t max,
                         uint32_t timeout_ms);
void    ag_linkprov_poll(uint32_t ms);

ag_err_t ag_linkprov_stats(ag_link_stats_t *out);

#endif /* ARGON_LINKPROV_H */
