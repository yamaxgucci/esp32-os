/*
 * ArgonOS - the other node's devices, in this node's registry.
 *
 * This is the piece the whole cluster was asked for: Wi-Fi on one board, sound
 * on another, and a system in which they are simply there.  A device on the far
 * node is registered HERE, under `<node>.<device>`, with the ordinary device
 * vtable - so `dev` lists it, an application opens it by name, and nothing
 * above the registry learns that a wire is involved.
 *
 * The qualified name is the node's, not an index.  `n2.pcm0` and not `pcm1`:
 * a number assigned in order of arrival means a config file written yesterday
 * points at a different board today, and that is the one failure nobody can
 * debug from the symptom.
 *
 * This does NOT make the far device as good as a local one, and is not meant
 * to.  The placement rule stands: a process should go to its data, and a
 * program that reaches across the wire for every sample is paying a round trip
 * for each of them.  This is the escape hatch the rule needs in order to be
 * advice rather than a prohibition - a rule that forbids gets worked around.
 *
 * Every call here is a QUESTION that waits for its answer, so all of it rides
 * the reliable class.  The link's stream class exists for sound and is
 * deliberately allowed to drop frames; pouring samples at a far converter
 * belongs there, and it is a different mechanism from this one, not a faster
 * setting of it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NODERDEV_H
#define ARGON_NODERDEV_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Answer the other node's device calls against this node's registry. */
ag_err_t ag_noderdev_serve(void);

/*
 * Ask the far node what it has and put proxies for it in this registry, named
 * `<peer>.<device>`.  Needs the peer's name, so it needs the hello to have been
 * answered; returns -AG_EAGAIN when it has not.  Returns how many appeared.
 */
int32_t ag_noderdev_attach(uint32_t timeout_ms);

/* Take them all out again.  Open handles are revoked, not waited for. */
uint32_t ag_noderdev_detach(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_NODERDEV_H */
