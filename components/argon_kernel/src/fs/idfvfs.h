/*
 * ArgonOS - VFS backend over an ESP-IDF filesystem (kernel private).
 *
 * ESP-IDF already implements FAT, littlefs and SPIFFS behind a POSIX layer,
 * along with the wear levelling and the SD host drivers underneath.  Rewriting
 * any of that would be work with no product in it, so instead this adapts one
 * of its mount points to an ArgonOS backend.
 *
 * The adapter is what makes the choice of on-flash filesystem a one-line
 * decision rather than a rewrite: swapping FAT for littlefs changes which
 * mount call runs, not this file.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_IDFVFS_H
#define ARGON_IDFVFS_H

#include <argon/vfs.h>

typedef struct ag_idfvfs ag_idfvfs_t;

/*
 * Reports capacity.  Each ESP-IDF filesystem spells this differently, so the
 * caller supplies it; NULL means the mount reports no size, which the shell
 * shows as zero rather than inventing a number.
 */
typedef ag_err_t (*ag_idfvfs_space_fn)(void *ctx, uint64_t *total,
                                       uint64_t *available);

/*
 * `base_path` is the path ESP-IDF was told to mount at, for example "/flash".
 * `fs_name` is what the mount command displays, for example "fat".
 */
ag_err_t ag_idfvfs_create(const char *base_path, const char *fs_name,
                          ag_idfvfs_space_fn space, void *space_ctx,
                          ag_idfvfs_t **out);
void ag_idfvfs_destroy(ag_idfvfs_t *fs);

const ag_fs_ops_t *ag_idfvfs_ops(void);

#endif /* ARGON_IDFVFS_H */
