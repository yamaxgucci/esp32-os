/*
 * ArgonOS - loadable .SYS driver modules.
 *
 * Same image format as an application (.AXE bytes, usually named .SYS), but the
 * entry point is ag_driver_init and the image stays resident until unloaded.
 * Devices it registers are owned by the module cookie, so unload revokes them
 * the same way pulling a card revokes sd0 - holders learn on the next call.
 *
 * Not a process: no task, no heap arena, no foreground.  The code still lives
 * in the shared executable arena, which is why a driver and an application
 * compete for the same kilobytes (docs/04-roadmap.md, phase 4.5).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_MODULE_H
#define ARGON_MODULE_H

#include <argon/loader.h>
#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Eight is a bound, not a prediction.  A handful of small drivers fit the code
 * arena; twenty next to an application do not.  Hitting the bound is reported.
 */
#define AG_MODULE_MAX 8

typedef struct {
    char name[32];
    char path[AG_PATH_MAX];
    char version[16];
    size_t code_bytes;
    size_t data_bytes;
} ag_modinfo_t;

/*
 * Loads a .SYS (or any .AXE with AG_AXE_DRIVER), calls ag_driver_init, and keeps
 * the image.  -AG_EEXIST when a module of that name is already loaded,
 * -AG_EINVAL when the image is an ordinary application, -AG_ENFILE when the
 * table is full.  A failing init unloads the image and revokes anything it
 * managed to register before returning the error.
 */
ag_err_t ag_module_load(const char *path, const char *cwd);

/*
 * Same as ag_module_load, but ag_driver_init can ask for `hint` through
 * dev->probe_hint().  Used when the module was chosen by a bus match.
 */
ag_err_t ag_module_load_hinted(const char *path, const char *cwd,
                               const ag_probe_hint_t *hint);

/*
 * Revokes every device the module published and frees its image.  `name` is the
 * name from the image header (what `drv` lists), not the file path.
 * -AG_ENOENT when nothing of that name is loaded.
 */
ag_err_t ag_module_unload(const char *name);

ag_err_t ag_module_info(uint32_t index, ag_modinfo_t *out);
uint32_t ag_module_count(void);

/*
 * The module whose ag_driver_init is running right now, or NULL.  The device
 * registry uses this as the owner cookie for add().
 */
const void *ag_module_loading(void);

/* Probe hint for that same window, or NULL.  What dev->probe_hint() returns. */
const ag_probe_hint_t *ag_module_probe_hint(void);

/*
 * Walks [modules] device=... from SYSTEM.CFG, then runs I2C probe against
 * modules.probe.  A missing or failing module is logged; the board still
 * reaches the shell.  Boot stage `modules` calls this.
 */
ag_err_t ag_modules_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_MODULE_H */
