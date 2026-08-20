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
/*
 * The same slots, for bytes that are read rather than run.
 *
 * A cartridge, a font, a table of impulse responses: data too big to keep in
 * memory, unchanging, and wanted at an address.  Flash holds it and the cache
 * fetches it, exactly as it does for code - the only difference is which bus the
 * range is mapped into, and on this class of part that difference is absolute:
 * an instruction-bus address cannot be read as bytes.
 *
 * reserve_data skips the address prediction that reserve does, because nothing
 * has to be relocated against it.  program_at exists because the file may be
 * half a megabyte and there is nowhere to hold that while writing it.
 */
ag_err_t ag_appfs_reserve_data(size_t bytes, ag_appfs_slot_t **out_slot);
ag_err_t ag_appfs_program_at(ag_appfs_slot_t *slot, size_t off,
                             const void *data, size_t bytes);
ag_err_t ag_appfs_mmap_data(ag_appfs_slot_t *slot, const void **out_ptr);

void ag_appfs_release(ag_appfs_slot_t *slot);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_APPFS_H */
