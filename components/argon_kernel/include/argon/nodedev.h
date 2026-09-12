/*
 * ArgonOS - what the other node has.
 *
 * The first half of "one `dev` for the whole system": a node can ask its
 * neighbour what devices it has, and print them beside its own.  Opening one is
 * the second half and is not here yet - a screen and a sound card have to cross
 * the wire as a stream, which is the mux's job, while a LIST is one message and
 * one answer.
 *
 * WHY THE LIST COMES FIRST, and not as a stepping stone.  It is what placement
 * needs: `run` cannot decide where a program should go until it knows which
 * node has the device the program is for, and that question is answered by a
 * list rather than by opening anything.  The plan's rule - move the process to
 * the data - is exactly a question about this list.
 *
 * NAMES ARE NOT QUALIFIED YET, and that is deliberate rather than forgotten.
 * `n2.pcm0` is the decided form, and it needs node names, and node names are
 * what a THIRD board makes necessary: with two, "the other one" is unambiguous
 * and a prefix would be ceremony.  The listing marks a remote device instead.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NODEDEV_H
#define ARGON_NODEDEV_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

/* Start answering "what have you got".  Idempotent; called when a link binds. */
ag_err_t ag_nodedev_start(void);

/*
 * Ask the other node for its devices.  Fills up to `max` entries and returns
 * how many arrived, or a negative error.
 *
 * Blocks for up to `timeout_ms`.  -AG_ENODEV with no link; -AG_ETIMEDOUT when
 * the far node does not answer, which is a different thing from a node that
 * answers with nothing and is reported differently on purpose: no devices and
 * no neighbour look the same in a listing and mean opposite things.
 */
int32_t ag_nodedev_list(ag_devinfo_t *out, uint32_t max, uint32_t timeout_ms);

/*
 * "Could you run a program that needs this?"  `axe_flags` is a .AXE flags word;
 * returns 1 for yes, 0 for no, or a negative error.
 *
 * Separate from the listing on purpose.  A listing cannot answer it: every node
 * has a sound device, because pcmnull is one, so a board that can make no noise
 * at all still lists an AG_DEV_AUDIO row.  This makes the far node apply the
 * same test its own loader would, which is the only test whose answer matches
 * what would actually happen there.
 *
 * -AG_ETIMEDOUT is not "no".  A node that did not answer and a node that
 * answered no mean different things: the first is a broken wire, the second is
 * a reason to place the program somewhere else.
 */
int32_t ag_nodedev_can(uint32_t axe_flags, uint32_t timeout_ms);

#endif /* ARGON_NODEDEV_H */
