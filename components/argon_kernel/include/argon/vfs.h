/*
 * ArgonOS - virtual filesystem.
 *
 * A thin layer over a set of backends, not a filesystem itself.  Its jobs are
 * the ones that have to be done in exactly one place: turning a path into a
 * mount plus a relative path, owning the handle table, and knowing which
 * process opened what so that killing it can close them.
 *
 * Backends supply the actual storage.  Some are native (the RAM disk); others
 * wrap what ESP-IDF already provides (FAT on a card or on flash).  The core
 * has no idea which is which, which is what lets it be tested on a host.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_VFS_H
#define ARGON_VFS_H

#include <argon/abi.h>
#include <argon/path.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AG_VFS_MAX_MOUNTS 6
#define AG_VFS_MAX_HANDLES 24

/*
 * Backend operations.  Paths handed to a backend are relative to its mount
 * point and always start with '/', so the mount root is "/".
 *
 * Any entry may be NULL; the core reports -AG_ENOTSUP for it.
 */
typedef struct {
    const char *name; /* "ram", "fat", "lfs" - shown by the mount command */

    ag_err_t (*open)(void *ctx, const char *rel, uint32_t flags, void **file);
    ag_err_t (*close)(void *ctx, void *file);
    int32_t (*read)(void *ctx, void *file, void *buf, size_t len);
    int32_t (*write)(void *ctx, void *file, const void *buf, size_t len);
    int64_t (*seek)(void *ctx, void *file, int64_t off, int whence);
    ag_err_t (*sync)(void *ctx, void *file);
    ag_err_t (*truncate)(void *ctx, void *file, uint64_t len);

    ag_err_t (*stat)(void *ctx, const char *rel, ag_stat_t *out);
    ag_err_t (*unlink)(void *ctx, const char *rel);
    ag_err_t (*rename)(void *ctx, const char *from, const char *to);
    ag_err_t (*mkdir)(void *ctx, const char *rel);
    ag_err_t (*rmdir)(void *ctx, const char *rel);

    ag_err_t (*opendir)(void *ctx, const char *rel, void **dir);
    ag_err_t (*readdir)(void *ctx, void *dir, ag_dirent_t *out);
    ag_err_t (*closedir)(void *ctx, void *dir);

    ag_err_t (*info)(void *ctx, ag_fsinfo_t *out);
} ag_fs_ops_t;

enum ag_mount_flags {
    AG_MOUNT_READONLY = 1u << 0,
    AG_MOUNT_REMOVABLE = 1u << 1,
};

/*
 * Serialisation is supplied rather than assumed, so the core can be built and
 * tested without FreeRTOS.  Pass NULL for no locking.
 */
typedef struct {
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void *ctx;
} ag_vfs_lock_t;

ag_err_t ag_vfs_init(const ag_vfs_lock_t *lock);

ag_err_t ag_vfs_mount(const char *mountpoint, const ag_fs_ops_t *ops,
                      void *ctx, uint32_t flags);
/* Fails with -AG_EBUSY while any handle on the mount is still open. */
ag_err_t ag_vfs_unmount(const char *mountpoint);
/*
 * Drops a mount whose media has gone away.  Open handles are poisoned rather
 * than left pointing at a backend that no longer exists: every later call on
 * them returns -AG_EIO until they are closed.
 */
ag_err_t ag_vfs_eject(const char *mountpoint);

typedef struct {
    char       mount[32];
    ag_fsinfo_t info;
    uint32_t   flags;
    uint32_t   open_handles;
} ag_mountinfo_t;

ag_err_t ag_vfs_mount_info(uint32_t index, ag_mountinfo_t *out);

/* Paths may be relative; they are resolved against `cwd` when given. */
ag_handle_t ag_vfs_open(const char *path, const char *cwd, uint32_t flags);
ag_err_t ag_vfs_close(ag_handle_t h);
int32_t ag_vfs_read(ag_handle_t h, void *buf, size_t len);
int32_t ag_vfs_write(ag_handle_t h, const void *buf, size_t len);
int64_t ag_vfs_seek(ag_handle_t h, int64_t off, int whence);
ag_err_t ag_vfs_sync(ag_handle_t h);
ag_err_t ag_vfs_truncate(ag_handle_t h, uint64_t len);

ag_err_t ag_vfs_stat(const char *path, const char *cwd, ag_stat_t *out);
ag_err_t ag_vfs_unlink(const char *path, const char *cwd);
ag_err_t ag_vfs_rename(const char *from, const char *to, const char *cwd);
ag_err_t ag_vfs_mkdir(const char *path, const char *cwd);
ag_err_t ag_vfs_rmdir(const char *path, const char *cwd);

ag_handle_t ag_vfs_opendir(const char *path, const char *cwd);
ag_err_t ag_vfs_readdir(ag_handle_t h, ag_dirent_t *out);
ag_err_t ag_vfs_closedir(ag_handle_t h);

/*
 * Ownership.  Every handle records who opened it, so that killing a process
 * can close its files without the supervisor keeping a list of its own.
 *
 * The owner is asked for rather than set, because a global "current process"
 * would be wrong the moment two tasks open a file at once.  The process layer
 * installs a function that reports the calling task's owner; until then
 * everything belongs to the kernel.
 */
void ag_vfs_set_owner_fn(ag_pid_t (*current_owner)(void));
uint32_t ag_vfs_close_owned_by(ag_pid_t pid);

/* Number of handles currently open, for the shell and for leak checks. */
uint32_t ag_vfs_open_count(void);

/*
 * The backend's own object behind an open file handle, or NULL.
 *
 * `ops` says which backend the caller believes it is talking to, and a handle
 * belonging to any other one answers NULL.  Without that the caller would be
 * casting whatever the last mount put there, and a handle on a file would look
 * exactly like a handle on a device.
 *
 * This exists for the one thing a file handle has no room for: the device layer
 * needs to get from an open handle back to the device, so that ioctl and the
 * class vtable can be reached without a second handle table.
 */
void *ag_vfs_backend_object(ag_handle_t h, const ag_fs_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_VFS_H */
