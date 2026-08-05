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
    void            *image;
    size_t           image_size;
    ag_axe_binding_t binding;
} ag_loaded_app_t;

/*
 * Reads a .AXE, places it in executable memory and binds the syscall table into
 * it.  Nothing runs yet.
 */
ag_err_t ag_loader_load(const char *path, const char *cwd,
                        ag_loaded_app_t *out);

void ag_loader_unload(ag_loaded_app_t *app);

/*
 * Calls ag_main and returns its exit code, or the code passed to ag_exit().
 *
 * For now the application runs on the caller's task, which means a wild pointer
 * takes the shell with it.  The process model and the supervisor are what turn
 * that into something survivable; until then this is honest about what it is.
 */
int ag_loader_run(ag_loaded_app_t *app, int argc, char **argv);

/* Ends the running application.  Does not return.  Called by the ABI's exit. */
void ag_loader_exit(int code);

/* Size of the reserved application arena, and whether an image occupies it. */
size_t ag_loader_arena_size(void);
bool   ag_loader_arena_busy(void);

/* The syscall table handed to applications. */
const ag_api_t *ag_loader_api(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_LOADER_H */
