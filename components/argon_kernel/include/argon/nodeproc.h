/*
 * ArgonOS - running a program on the other node.
 *
 * The first thing a cluster is FOR, and the smallest one: a command typed here
 * loads and runs there, its output comes back to this console, and its exit
 * code comes back as the command's own.  Everything else the plan asks for -
 * placement, a global pid, migration - is this with a policy in front of it.
 *
 * ONE READER.  Both halves of a run talk over the same link, and the link hands
 * back whatever arrived next; two readers of one inbox steal from each other,
 * which is a mistake this tree has already made twice with a UART.  So exactly
 * one task reads - the server task below - and it either serves a request or
 * hands the message to whoever is waiting for it.
 *
 * NO FRAMING OF ITS OWN.  The messages here have an eight-byte header and no
 * magic, no length prefix beyond it and no checksum, because the link is not a
 * byte stream: ag_linkprov_recv returns whole messages that have already been
 * checked, in order, or not at all.  Adding a second framing layer would be
 * writing down the same length twice and then having to decide which one to
 * believe.
 *
 * WHAT IT DOES NOT DO YET, said here rather than discovered:
 *   - one run at a time.  The server task executes the program on its own task
 *     and is therefore inside it until it ends, so a second request waits.
 *   - no input.  The far program reads a console nobody types at.
 *   - no addressing.  With two nodes "the other one" is unambiguous; names are
 *     what a third board needs, and it can bring them.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NODEPROC_H
#define ARGON_NODEPROC_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

/*
 * Start serving requests from the other node.  Idempotent; called when a link
 * is bound, because before that there is nothing to serve.
 */
ag_err_t ag_nodeproc_start(void);

/* Is the server task up. */
bool ag_nodeproc_serving(void);

/*
 * Run `cmdline` on the other node and wait for it.  Output arrives on this
 * console as it is produced; `exit_code` receives the program's own value.
 *
 * Returns -AG_ENODEV with no link, -AG_EBUSY when the far node is already
 * running something, -AG_ETIMEDOUT when it stops answering.
 */
ag_err_t ag_nodeproc_run(const char *cmdline, uint32_t timeout_ms,
                         int32_t *exit_code);

/*
 * The same, carrying a migrating program's state to the far node before it
 * starts (ABI 0.53).  The state is put in place over there BEFORE the program
 * runs, so its first call to migrate_restore finds it.
 */
ag_err_t ag_nodeproc_run_state(const char *cmdline, const void *state,
                               uint32_t state_len, uint32_t timeout_ms,
                               int32_t *exit_code);

/*
 * Tell the node that sent us a migrating program that the program has taken
 * its state and is running.  Called by the migration code, not by drivers.
 */
void ag_nodeproc_migrated(void);

/*
 * "Send me back what you are holding for me."  The far node starts an ordinary
 * migration homewards for the guest that came from here; it does not answer,
 * and a node holding nothing of ours does nothing.  -AG_ENODEV with no wire.
 */
ag_err_t ag_nodeproc_ask_return(void);

#endif /* ARGON_NODEPROC_H */
