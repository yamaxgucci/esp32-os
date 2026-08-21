/*
 * ArgonOS - kernel entry point.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_KERNEL_H
#define ARGON_KERNEL_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARGON_VERSION_MAJOR 0
#define ARGON_VERSION_MINOR 1
#define ARGON_VERSION_PATCH 0
#define ARGON_VERSION_STR "0.1.0"

/*
 * Boot stages, in execution order.  Each stage is allowed to fail; the boot
 * sequence records the failure, keeps going where it can, and reports the
 * degraded state through `ag_kernel_status()`.  A board with no SD card must
 * still reach a usable shell.
 */
typedef enum {
    AG_STAGE_PLATFORM = 0, /* chip / memory / profile detection          */
    AG_STAGE_MEMORY,       /* arenas, executable app-text arena          */
    AG_STAGE_LOG,          /* early log ring buffer                      */
    AG_STAGE_BOARD,        /* board pack selection, pin map              */
    AG_STAGE_CONSOLE,      /* virtual text screen + UART endpoint        */
    AG_STAGE_STORAGE,      /* littlefs /sys, ramfs /tmp                  */
    AG_STAGE_CONFIG,       /* SYSTEM.CFG                                 */
    AG_STAGE_POWER,        /* the clock this machine has, and its modes   */
    AG_STAGE_DEVICES,      /* static drivers, device manager             */
    AG_STAGE_MEDIA,        /* SD card, FAT /sd                           */
    AG_STAGE_MODULES,      /* loadable .SYS drivers listed in the config */
    AG_STAGE_SUPERVISOR,   /* watchdog, hotkeys, crash handling          */
    AG_STAGE_SHELL,        /* hand control to the shell or AUTOEXEC      */
    AG_STAGE_COUNT
} ag_boot_stage_t;

typedef struct {
    ag_err_t stage_result[AG_STAGE_COUNT];
    uint32_t stage_us[AG_STAGE_COUNT];
    uint32_t boot_us;      /* total time to the shell prompt             */
    bool     degraded;     /* at least one non-fatal stage failed        */
} ag_boot_report_t;

/*
 * Runs the whole system.  Called once from app_main() and never returns
 * under normal operation.
 */
void ag_kernel_main(void);

const ag_sysinfo_t *ag_sysinfo(void);
const ag_boot_report_t *ag_boot_report(void);

/* The API table handed to every process. */
const ag_api_t *ag_kernel_api(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_KERNEL_H */
