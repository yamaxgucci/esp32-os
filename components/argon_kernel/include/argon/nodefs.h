/*
 * ArgonOS - fetching a file from the other node.
 *
 * The cluster's `scp`, and the first thing on this link that is BULK rather
 * than CONTROL.  That second part is most of why it exists now: the window, the
 * go-back-N and the retransmit timer were written with care and proved against
 * an injected loss model on a development machine, and until something streamed
 * kilobytes through them between two kernels none of it had met a wire.
 *
 * It is also the smallest useful piece of "one disk for the whole system": a
 * program that should run over there has to get over there.  A mounted remote
 * drive is the larger version, and it is what the manifest half of placement
 * waits for - to decide by a program's declaration you must read the program,
 * and a node can only read what it can reach.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NODEFS_H
#define ARGON_NODEFS_H

#include <stdint.h>

#include <argon/abi.h>

/* Start answering fetches.  Idempotent; called when a link binds. */
ag_err_t ag_nodefs_start(void);

/*
 * Copy `remote` from the other node into `local` here.  Returns the bytes
 * written, or a negative error - the far node's own, when it was the far node
 * that could not do it.
 */
int32_t ag_nodefs_get(const char *remote, const char *local, const char *cwd,
                      uint32_t timeout_ms);

#endif /* ARGON_NODEFS_H */
