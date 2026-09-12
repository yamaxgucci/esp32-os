/*
 * ArgonOS - the other node's card, as a drive letter.
 *
 * Not a command but a VFS backend, and that is the whole point: everything the
 * system already knows how to do - run, dir, copy, fm - starts working on the
 * far node's card without a line of change.  It is also what the manifest half
 * of placement is waiting for: to decide by a program's declaration you must
 * read the program, and a node can only read what it can reach.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NODEDRV_H
#define ARGON_NODEDRV_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where it goes.  The shell shows this one as N:. */
#define AG_NODEDRV_MOUNT "/node"

/* Answer the other node's file requests against our own card. */
ag_err_t ag_nodedrv_serve(void);

/* Mount the other node's card.  `mountpoint` is AG_NODEDRV_MOUNT. */
ag_err_t ag_nodedrv_mount(const char *mountpoint);
ag_err_t ag_nodedrv_unmount(const char *mountpoint);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_NODEDRV_H */
