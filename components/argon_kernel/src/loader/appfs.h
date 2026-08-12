/*
 * ArgonOS - flash slots for application code (R-1 XIP).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_APPFS_H
#define ARGON_APPFS_H

#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AG_APPFS_NAME "appfs"

typedef struct ag_appfs_slot ag_appfs_slot_t;

/* Finds the partition; safe to call more than once. */
ag_err_t ag_appfs_init(void);

bool ag_appfs_ready(void);

/*
 * Reserves `bytes` (rounded up to the flash mmap page), erases the range, and
 * probes the instruction-window address the region will have when mapped.
 * On success `out_slot` owns the reservation until ag_appfs_release.
 */
ag_err_t ag_appfs_reserve(size_t bytes, ag_appfs_slot_t **out_slot,
                          void **out_exec_addr);

/* Programs already-relocated code into a reserved slot. */
ag_err_t ag_appfs_program(ag_appfs_slot_t *slot, const void *data, size_t bytes);

/*
 * Maps the programmed slot into the instruction window.  `out_ptr` is where
 * execution starts (same address reserve predicted).
 */
ag_err_t ag_appfs_mmap(ag_appfs_slot_t *slot, const void **out_ptr);

/* Unmaps (if mapped) and frees the reservation. */
void ag_appfs_release(ag_appfs_slot_t *slot);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_APPFS_H */
