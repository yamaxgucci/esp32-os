/*
 * ArgonOS - storage bring-up (kernel private).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
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
 * Lets the card go, so it can be taken out.
 *
 * There is no line on this class of board that says a card has been removed, so
 * the system cannot notice: it has to be told, before rather than after.  Open
 * files are ejected rather than waited for - whatever is using the card must
 * fail now, because the card is about to leave whether it likes it or not.
 *
 * -AG_ENODEV when nothing was mounted, which is not a failure worth a message.
 */
ag_err_t ag_storage_eject_media(void);

/* Optional HostFS (H:) after media; no-op when the host helper is absent. */
ag_err_t ag_storage_mount_hostfs(void);

/*
 * Writes a fresh filesystem to the card, destroying what was on it, then mounts
 * it.  Only reachable through the format command, which asks first.
 */
ag_err_t ag_storage_format_media(void);

bool ag_storage_media_present(void);

/*
 * Registers the storage that is always there - the internal flash partition -
 * with the device registry, so that /dev has something behind it that is real
 * hardware and not a built-in.  Called by the devices stage; the card registers
 * and unregisters itself as it is mounted and pulled, which is later and is not
 * a stage.
 */
void ag_storage_register_devices(void);

/*
 * Which task holds the filesystem lock, as a FreeRTOS task handle, or NULL.
 * Asked for the same reason as the console's: a task deleted while it holds this
 * would leave every later file operation waiting forever.
 */
void *ag_storage_vfs_lock_holder(void);

#endif /* ARGON_STORAGE_H */
