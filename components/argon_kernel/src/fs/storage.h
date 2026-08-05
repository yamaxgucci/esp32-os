/*
 * ArgonOS - storage bring-up (kernel private).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_STORAGE_H
#define ARGON_STORAGE_H

#include <argon/abi.h>

/* Initialises the VFS and mounts what does not need a board: /tmp. */
ag_err_t ag_storage_init(void);

#endif /* ARGON_STORAGE_H */
