/*
 * ArgonOS - what the other node is running.
 *
 * One `ps` for the whole system, which is half of what "the user does not know
 * how many boards there are" means - the other half being one `dev`, and both
 * were asked for in the same sentence.
 *
 * A remote process is named `<node>.<pid>`, the same shape as `<node>.<device>`.
 * NOT a pid with a node number packed into its high bits, which was the obvious
 * alternative and is worse for the same reason a device is not renumbered: a
 * number assigned by position means the thing an operator wrote down yesterday
 * points somewhere else today, and a pid is written down constantly - into a
 * `kill`, into a `fg`, into a note on paper.  A name stays a name.
 *
 * It also costs nothing: pids stay exactly what they are on each node, no ABI
 * changes, and an application that never heard of a cluster is unaffected.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NODEPS_H
#define ARGON_NODEPS_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Answer the other node's "what are you running". */
ag_err_t ag_nodeps_serve(void);

/*
 * What the far node is running.  Returns how many entries were filled, or a
 * negative error.  -AG_ETIMEDOUT is not "nothing": a node that did not answer
 * and a node running nothing mean different things, and a `ps` that shows an
 * empty space for a board that has stopped talking is a `ps` that lies.
 */
int32_t ag_nodeps_list(ag_procinfo_t *out, uint32_t max, uint32_t timeout_ms);

/* Stop a process on the far node, by ITS pid. */
ag_err_t ag_nodeps_kill(ag_pid_t pid, uint32_t timeout_ms);

/*
 * Splits `n2.7` into the node name and the pid.  True when it had a node
 * prefix; false means it is a plain local pid and `pid_out` holds it.
 */
bool ag_nodeps_split(const char *text, char *node_out, size_t node_len,
                     ag_pid_t *pid_out);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_NODEPS_H */
