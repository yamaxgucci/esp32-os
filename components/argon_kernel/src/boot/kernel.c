/*
 * ArgonOS - boot sequence.
 *
 * The boot sequence is a flat table of stages rather than a call chain, so
 * that a failing stage degrades the system instead of bricking the boot: a
 * board with no SD card, no display or no config file must still reach a
 * usable prompt on the serial console.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/kernel.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <argon/board.h>
#include <argon/console.h>
#include <argon/log.h>
#include <argon/module.h>
#include <argon/shell.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "boot/platform.h"
#include "console/uart_console.h"
#include "core/sysconfig.h"
#include "dev/devices.h"
#include "fs/storage.h"
#include "proc/supervisor.h"

#define AG_CONSOLE_COLS 80
#define AG_CONSOLE_ROWS 25

/*
 * Early boot tracing writes straight to the raw console, bypassing everything
 * ArgonOS provides.  It is the only way to see anything when the failure is in
 * the code that makes output possible, which is exactly when it is needed.
 * Off by default; build with -DAG_BOOT_TRACE=1 to bring up a new board.
 */
#ifndef AG_BOOT_TRACE
#define AG_BOOT_TRACE 0
#endif

#if AG_BOOT_TRACE
#define AG_TRACE(...)                                                        \
    do {                                                                     \
        printf("argon-trace: " __VA_ARGS__);                                 \
        fflush(stdout);                                                      \
    } while (0)
#else
#define AG_TRACE(...) ((void)0)
#endif

typedef ag_err_t (*ag_stage_fn)(void);

typedef struct {
    const char  *name;
    ag_stage_fn  fn;    /* NULL while the stage is not implemented yet     */
    bool         fatal; /* boot cannot continue without it                 */
} ag_stage_desc_t;

static ag_sysinfo_t     s_sysinfo;
static ag_boot_report_t s_report;

/*
 * Before the console exists there is nowhere to print but the raw UART, and
 * after it exists printing anywhere else would corrupt the screen.
 */
static void kout(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (ag_console_ready()) {
        (void)ag_console_vprintf(fmt, ap);
    } else {
        (void)vprintf(fmt, ap);
    }
    va_end(ap);
}

/* ---------------------------------------------------------------------- */

static ag_err_t stage_platform(void)
{
    ag_err_t err = ag_platform_init();
    if (err != AG_OK) {
        return err;
    }
    ag_platform_fill_sysinfo(&s_sysinfo);
    return AG_OK;
}

static void print_banner(void)
{
    const ag_platform_t *p = ag_platform();

    kout("\n");
    kout("ArgonOS %s (%s)  %s  profile=%s\n", s_sysinfo.os_version,
         s_sysinfo.build, s_sysinfo.chip, s_sysinfo.profile);
    kout("%u KB conventional memory, %u KB extended\n",
         (unsigned)(p->sram_free_at_boot / 1024u),
         (unsigned)(p->psram_total / 1024u));
    kout("ABI %u.%u  cores=%u  app core=%u\n", s_sysinfo.abi_major,
         s_sysinfo.abi_minor, s_sysinfo.cpu_cores, s_sysinfo.app_core);
    kout("\n");
}

static ag_err_t stage_console(void)
{
    AG_TRACE("console init %ux%u\n", AG_CONSOLE_COLS, AG_CONSOLE_ROWS);
    ag_err_t err = ag_console_init(AG_CONSOLE_COLS, AG_CONSOLE_ROWS);
    if (err != AG_OK) {
        return err;
    }

    AG_TRACE("attaching uart%d\n", CONFIG_ESP_CONSOLE_UART_NUM);
    err = ag_uart_console_attach(CONFIG_ESP_CONSOLE_UART_NUM, 115200);
    if (err != AG_OK) {
        return err;
    }
    AG_TRACE("uart attached\n");

    /*
     * Log output already goes through the journal, which took over ESP-IDF's
     * hook in the log stage.  Nothing needs redirecting here: the console has
     * simply become one of the journal's readers.
     */
    print_banner();
    return AG_OK;
}

static ag_err_t stage_shell(void) { return AG_OK; }

/*
 * Stages are added here as they are implemented; see docs/04-roadmap.md.
 * Keeping the unimplemented ones visible makes the boot report honest about
 * what the system can and cannot do yet.
 */
static const ag_stage_desc_t s_stages[AG_STAGE_COUNT] = {
    [AG_STAGE_PLATFORM]   = {"platform",   stage_platform, true},
    [AG_STAGE_MEMORY]     = {"memory",     NULL,           false},
    [AG_STAGE_LOG]        = {"log",        ag_log_init,    false},
    [AG_STAGE_BOARD]      = {"board",      ag_board_init,  false},
    [AG_STAGE_CONSOLE]    = {"console",    stage_console,  true},
    [AG_STAGE_STORAGE]    = {"storage",    ag_storage_init, false},
    [AG_STAGE_CONFIG]     = {"config",     ag_sysconfig_init, false},
    [AG_STAGE_DEVICES]    = {"devices",    ag_devices_init, false},
    [AG_STAGE_MEDIA]      = {"media",      ag_storage_mount_media, false},
    [AG_STAGE_MODULES]    = {"modules",    ag_modules_boot, false},
    [AG_STAGE_SUPERVISOR] = {"supervisor", ag_supervisor_init, false},
    [AG_STAGE_SHELL]      = {"shell",      stage_shell,    true},
};

/* ---------------------------------------------------------------------- */

static void print_summary(void)
{
    int pending = 0;
    for (int i = 0; i < AG_STAGE_COUNT; i++) {
        if (s_stages[i].fn == NULL) {
            pending++;
        }
    }

    kout("boot: %u us", (unsigned)s_report.boot_us);
    if (pending > 0) {
        kout(", %d subsystems not implemented yet ('boot' for details)",
             pending);
    }
    kout("\n");

    for (int i = 0; i < AG_STAGE_COUNT; i++) {
        if (s_stages[i].fn != NULL && s_report.stage_result[i] != AG_OK) {
            kout("  stage '%s' FAILED with %d\n", s_stages[i].name,
                 (int)s_report.stage_result[i]);
        }
    }
}

void ag_kernel_main(void)
{
    AG_TRACE("kernel entry\n");
    const int64_t t0 = esp_timer_get_time();

    memset(&s_report, 0, sizeof(s_report));

    bool fatal_failure = false;

    for (int i = 0; i < AG_STAGE_COUNT; i++) {
        const ag_stage_desc_t *st = &s_stages[i];

        if (st->fn == NULL) {
            s_report.stage_result[i] = -AG_ENOSYS;
            s_report.degraded = true;
            continue;
        }

        const int64_t ts = esp_timer_get_time();
        const ag_err_t err = st->fn();
        s_report.stage_us[i] = (uint32_t)(esp_timer_get_time() - ts);
        s_report.stage_result[i] = err;

        if (err != AG_OK) {
            s_report.degraded = true;
            /*
             * Logged rather than printed: the journal is the record and the
             * console is one of its readers, so this reaches both without
             * being written twice.  A boot failure is exactly the kind of
             * message that used to scroll away before anyone read it.
             */
            ag_log(AG_LOG_ERROR, "boot", "stage '%s' failed with %d", st->name,
                   (int)err);
            if (st->fatal) {
                ag_log(AG_LOG_ERROR, "boot", "cannot continue");
                fatal_failure = true;
                break;
            }
        }
    }

    s_report.boot_us = (uint32_t)(esp_timer_get_time() - t0);
    print_summary();

    if (fatal_failure) {
        /* Keep the board alive so the failure can be read off the console. */
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ag_shell_run();
}

const ag_sysinfo_t *ag_sysinfo(void) { return &s_sysinfo; }

const ag_boot_report_t *ag_boot_report(void) { return &s_report; }

const ag_api_t *ag_kernel_api(void)
{
    /* Populated in phase 2 together with the process loader. */
    return NULL;
}
