/*
 * ArgonOS - joining a cluster: whether this board may, and doing it at boot.
 *
 * TWO SETTINGS, AND NEITHER OF THEM IS A ROLE.
 *
 *   [cluster] multiboard = yes | none
 *   [link]    bind       = node0 | auto
 *
 * `multiboard = none` is a board saying it is not part of any cluster: it will
 * not bind a wire, and with no wire bound nothing can be asked of it - no
 * remote run, no guest process, no reading of its card, no borrowing of its
 * devices.  One gate rather than seven, because a board that cannot hear
 * cannot be asked, and a refusal that has to be remembered in seven services
 * is a refusal that will be forgotten in one of them.
 *
 * There is deliberately no Master and no Slave.  Roles here are per REQUEST,
 * not per board, and the shapes this is built for need both directions at
 * once: a board that is the screen for its neighbour's compute is subordinate
 * in one sense and in charge in the other, and the standby pair has the SMALL
 * board holding the work while the big one sleeps, then handing it back.  A
 * role written into a config would have to be changed while running, which is
 * a worse thing to have than not to have.
 *
 * `bind` is what used to be a line in AUTOEXEC.BAT, and it belongs here: the
 * binding is lost on every reset, so every test that resets a board lost it,
 * and so did every power cut in the field.  A board that is meant to be half
 * of a cluster should come up as half of a cluster.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NODEBOOT_H
#define ARGON_NODEBOOT_H

#include <stdbool.h>

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * May this board be part of a cluster at all.  True unless [cluster]
 * multiboard says none - the default is yes because a board that has gone to
 * the trouble of loading a link driver has already said what it wants.
 */
bool ag_cluster_allowed(void);

/*
 * Bind `dev` as this board's wire and start everything that answers a
 * neighbour on it.  The one sequence, used by `link use` and by the boot-time
 * bind, so the two cannot drift apart - which they would, because a service
 * added to one of them is easy to forget in the other.
 *
 * `say` prints a line per service that refused; the boot path passes false and
 * leaves those to the journal, because a board coming up should not bury its
 * own boot messages under seven lines about a cluster that is working.
 */
ag_err_t ag_node_bind(const char *dev, bool say);

/*
 * At boot, after the modules are loaded and before AUTOEXEC: bind the wire
 * [link] bind names, or the only link device there is when it says `auto`.
 * Silent and harmless when nothing asked for it.
 */
void ag_node_autobind(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_NODEBOOT_H */
