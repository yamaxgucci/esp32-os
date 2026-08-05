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
} ag_loaded_app_t;

/*
 * Reads a .AXE, places its code in executable memory and its data in writable
 * memory, and binds the syscall table into it.  Nothing runs yet.
 */
ag_err_t ag_loader_load(const char *path, const char *cwd,
                        ag_loaded_app_t *out);

void ag_loader_unload(ag_loaded_app_t *app);

/*
 * Calls ag_main on a task of its own and returns its exit code, or the code
 * passed to ag_exit().  The caller waits.
 *
 * The task gives the application its own stack, which is guarded, so an
 * application that overruns it is caught instead of quietly writing over the
 * shell.  It is not yet a process: there is no resource list, no Ctrl-C, and a
 * wild pointer still reaches the rest of the system.  That is the supervisor's
 * job, and the supervisor is not written yet.
 */
int ag_loader_run(ag_loaded_app_t *app, int argc, char **argv);

/* Ends the running application.  Does not return.  Called by the ABI's exit. */
void ag_loader_exit(int code);

/* Size of the reserved code arena, and whether an image occupies it. */
size_t ag_loader_arena_size(void);
bool   ag_loader_arena_busy(void);

/* The syscall table handed to applications. */
const ag_api_t *ag_loader_api(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_LOADER_H */
