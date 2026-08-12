/*
 * ArgonOS - the devices boot stage.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_DEV_DEVICES_H
#define ARGON_DEV_DEVICES_H

#include <argon/abi.h>

/*
 * Brings up the registry, mounts it at /dev, and registers what the system
 * itself provides.  Runs after storage, because /dev is a mount and the VFS has
 * to exist, and before media, so that a card that appears has somewhere to
 * register itself.
 */
ag_err_t ag_devices_init(void);

#endif /* ARGON_DEV_DEVICES_H */
