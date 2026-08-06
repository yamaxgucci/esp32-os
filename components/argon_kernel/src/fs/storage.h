/*
 * ArgonOS - storage bring-up (kernel private).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_STORAGE_H
#define ARGON_STORAGE_H

#include <argon/abi.h>

/* Initialises the VFS and mounts what needs no board description: /tmp, /sys. */
ag_err_t ag_storage_init(void);

/*
 * Mounts removable media at /sd, using the pins from the board description, so
 * it must run after the board and configuration stages.  Returns -AG_ENODEV
 * when the board has no slot and -AG_EIO when there is no readable card, both
 * of which are ordinary conditions rather than faults.
 */
ag_err_t ag_storage_mount_media(void);

/*
 * Writes a fresh filesystem to the card, destroying what was on it, then mounts
 * it.  Only reachable through the format command, which asks first.
 */
ag_err_t ag_storage_format_media(void);

bool ag_storage_media_present(void);

/*
 * Which task holds the filesystem lock, as a FreeRTOS task handle, or NULL.
 * Asked for the same reason as the console's: a task deleted while it holds this
 * would leave every later file operation waiting forever.
 */
void *ag_storage_vfs_lock_holder(void);

#endif /* ARGON_STORAGE_H */
