/*
 * ArgonOS - the two ends of a link telling each other who they are.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NODEHELLO_H
#define ARGON_NODEHELLO_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start listening on the link's own channel and say who we are.
 *
 * Returns once the question has gone out, not once it has been answered: the
 * other node may not be up yet, and a bind that waits for a neighbour fails on
 * whichever board boots first.  ag_node_peer_name() is NULL until it replies.
 */
ag_err_t ag_nodehello_start(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_NODEHELLO_H */
