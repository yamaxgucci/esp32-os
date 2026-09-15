/*
 * ArgonOS - cluster threads over the link.
 *
 * The transport half of argon/clu.h: it fills in ag_clu_transport_t and serves
 * the other node's half of a job.  Channel 8, which nothing else uses, so a
 * thread's messages never queue behind a file being read or a screen being
 * sent.
 *
 * THE RULE THAT SHAPES THIS FILE is the one every other node service in this
 * directory has had to learn: the link has exactly one reader task, handlers
 * run on it, and anything that waits for an answer must not run there - the
 * answer is delivered by the task that would be waiting.  A spawn waits for a
 * whole image to be loaded on the far board, over this same wire, so serving
 * one on the reader task would stop both nodes with nothing in any log.  So
 * requests are queued by the handler and served on a task of this file's own.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NODECLU_H
#define ARGON_NODECLU_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Listen on the job channel and offer this wire to the cluster layer. */
ag_err_t ag_nodeclu_start(void);

/* Is it listening. */
bool ag_nodeclu_serving(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_NODECLU_H */
