/*
 * ArgonOS - in-memory filesystem.
 *
 * Backs /tmp.  Small, allocation-bounded, and free of any dependency on the
 * chip, which also makes it the filesystem the VFS core is tested against.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_RAMFS_H
#define ARGON_RAMFS_H

#include <argon/vfs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ag_ramfs ag_ramfs_t;

typedef struct {
    /*
     * Caps everything the filesystem will allocate, directory entries
     * included.  A RAM disk with no limit is a way to run the system out of
     * memory by copying a file into it.
     */
    size_t budget;

    /*
     * Supplies timestamps.  NULL means files have no modification time, which
     * is better than a wrong one.
     */
    uint64_t (*now_unix)(void);

    /*
     * Where the contents live.  NULL uses malloc and free, which on the target
     * means internal RAM; pass a PSRAM-backed pair to give /tmp real room.
     */
    void *(*alloc)(size_t bytes);
    void (*release)(void *ptr);
} ag_ramfs_config_t;

ag_ramfs_t *ag_ramfs_create(const ag_ramfs_config_t *config);
void        ag_ramfs_destroy(ag_ramfs_t *fs);

const ag_fs_ops_t *ag_ramfs_ops(void);

/* Bytes currently accounted against the budget. */
size_t ag_ramfs_used(const ag_ramfs_t *fs);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_RAMFS_H */
