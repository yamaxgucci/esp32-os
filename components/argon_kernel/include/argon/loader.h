/*
 * ArgonOS - loading and running an application.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_LOADER_H
#define ARGON_LOADER_H

#include <argon/axeload.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ag_axe_header_t  header;
    ag_axe_place_t   place;
    ag_axe_binding_t binding;

    /*
     * The data part's allocation, when it has one of its own.  NULL for a
     * contiguous image, whose data lives inside the code allocation and is
     * released with it.
     */
    void *data_owned;

    /*
     * R-1 flash XIP: code runs from a mapped appfs slot.  `code_scratch` held
     * the relocated bytes until they were programmed; it is freed after mmap.
     * `xip_slot` is an opaque ag_appfs_slot_t*.
     */
    void *code_scratch;
    void *xip_slot;
    bool  code_from_xip;
} ag_loaded_app_t;

/*
 * Reads a .AXE, places its code in executable memory and its data in writable
 * memory, and binds the syscall table into it.  Nothing runs yet.
 */
ag_err_t ag_loader_load(const char *path, const char *cwd,
                        ag_loaded_app_t *out);

void ag_loader_unload(ag_loaded_app_t *app);

/*
 * The code arena.  Running an image is the process layer's business (argon/proc.h);
 * what belongs here is how much room there is for one.
 */
size_t ag_loader_arena_size(void);
size_t ag_loader_arena_free(void);
size_t ag_loader_arena_largest(void);
bool   ag_loader_arena_busy(void);

/* The syscall table handed to applications. */
const ag_api_t *ag_loader_api(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_LOADER_H */
