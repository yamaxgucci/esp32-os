/*
 * ArgonOS - the device registry as a filesystem.
 *
 * Mounted at /dev, which is drive D:.  Every registered device shows up as one
 * entry, and opening it gives an ordinary handle: `type d:\zero`, `copy d:\con
 * t:\notes.txt` and `hexdump d:\sd0` all work with no code that knows about
 * devices, because the handle table, the ownership and the reclaim on a killed
 * process are the ones the VFS already has.
 *
 * That reuse is the whole reason devices live behind a filesystem rather than
 * behind a second handle table of their own: two tables would mean two places
 * that have to remember to close what a dead process left open, and the second
 * one would be the one that forgets.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DEVFS_H
#define ARGON_DEVFS_H

#include <argon/device.h>
#include <argon/vfs.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How many devices can be open at once through the filesystem.  Smaller than
 * the VFS handle table on purpose: files are the common case and a device that
 * cannot be opened because the pool is full is a clearer failure than a file
 * that cannot be opened because a device took the last slot.
 */
#define AG_DEVFS_MAX_OPEN 8
#define AG_DEVFS_MAX_DIRS 4

/* Pass to ag_vfs_mount with a NULL context: the registry is the one there is. */
const ag_fs_ops_t *ag_devfs_ops(void);

/* Forgets every open file and directory.  For the tests; boot does it once. */
void ag_devfs_reset(void);

/*
 * The device behind an open handle, or NULL when the handle is not a device.
 * This is what lets ioctl and the class vtable be reached from a plain handle -
 * the two things a file has no room for.
 */
ag_device_t *ag_devfs_device_of(ag_handle_t h);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_DEVFS_H */
