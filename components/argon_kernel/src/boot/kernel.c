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
#include <argon/cfg.h>
#include <argon/console.h>
#include <argon/display.h>
#include <argon/log.h>
#include <argon/module.h>
#include <argon/power.h>
#include <argon/recovery.h>
#include <argon/shell.h>
#include <argon/textpanel.h>

#include <argon/port/time.h>
#include <argon/port/task.h>
#include <argon/port/uart.h>
#include <argon/port/config.h>

#include "boot/platform.h"
#include "console/uart_console.h"
#include "core/sysconfig.h"
#include "dev/devices.h"
#include "fs/storage.h"
#include "proc/supervisor.h"

/*
 * Eighty by twenty-five, unless the board says otherwise.
 *
 * It is a build option because of when it is needed: the console exists three
 * stages before anything has read a file off the flash, and a console that has
 * to be rebuilt later is a console whose first three stages of output went
 * somewhere else.
 *
 * It is not the final word, though.  `[console] cols/rows` in SYSTEM.CFG is
 * applied once the devices are up and again once the loadable drivers are
 * (see apply_console_size), so the size is a line in a file for anyone who
 * plugs a different display into a board that is already flashed - which is
 * the whole point of not baking it in.  Twice because `auto` asks the panel,
 * and a panel that arrives as a .SYS is not there for the first question.
 *
 * The board that wants it smaller is one whose only screen is the panel
 * soldered to it: 320 pixels hold forty of the 8-pixel cells this system
 * draws, and forty columns that are all visible beat eighty of which half are
 * off the glass.  That was the choice on the original PC too, and for the same
 * reason.
 */
#ifndef CONFIG_ARGON_CONSOLE_COLS
#define CONFIG_ARGON_CONSOLE_COLS 80
#endif
#ifndef CONFIG_ARGON_CONSOLE_ROWS
#define CONFIG_ARGON_CONSOLE_ROWS 25
#endif
#define AG_CONSOLE_COLS CONFIG_ARGON_CONSOLE_COLS
#define AG_CONSOLE_ROWS CONFIG_ARGON_CONSOLE_ROWS

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

    AG_TRACE("attaching uart%d\n", AG_PORT_UART_CONSOLE);
    err = ag_uart_console_attach(AG_PORT_UART_CONSOLE, 115200);
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
 * The console size, from the configuration.
 *
 *     [console]
 *     cols=40        ; a number, or `auto` for whatever the panel can show
 *     rows=25
 *
 * Run after the devices stage rather than after the config stage, because
 * `auto` has to ask the panel and the panel is a device.  Nothing is lost by
 * waiting: resizing keeps what is on the screen, so the boot report written at
 * the built-in size is still there afterwards.
 *
 * A size that will not fit is reported and ignored - the console is how anyone
 * would find out about it, so it is the last thing to break over a typo.
 */
static uint16_t size_from_cfg(const ag_cfg_t *cfg, const char *key,
                              uint16_t built_in, uint16_t panel,
                              uint16_t max)
{
    const char *v = ag_cfg_get(cfg, key, NULL);
    if (v == NULL) {
        return built_in;
    }
    if (v[0] == 'a' || v[0] == 'A') { /* auto */
        return (panel > 0) ? panel : built_in;
    }

    const int32_t n = ag_cfg_get_int(cfg, key, (int32_t)built_in);
    if (n < 1 || n > (int32_t)max) {
        kout("console: %s=%s ignored (1..%u, or auto)\n", key, v,
             (unsigned)max);
        return built_in;
    }
    return (uint16_t)n;
}

static void apply_console_size(void)
{
    const ag_cfg_t *cfg = ag_sysconfig();
    if (cfg == NULL) {
        return;
    }

    /*
     * What `auto` means, in the order it is asked.
     *
     * A panel driver with text ops knows its own answer (8x8 cells: forty by
     * thirty on the 320x240 glass of a CYD).  Failing that, the framebuffer
     * console draws 8x16 cells on whatever surface there is, so the surface
     * divided by that cell is what fits.  Failing both, the board is headless
     * and only the serial console is watching, where the built-in size is
     * right and nothing should shrink it.
     */
    uint16_t pcols = 0, prows = 0;
    if (ag_textpanel_geometry(&pcols, &prows) != AG_OK) {
        pcols = 0;
        prows = 0;
        (void)ag_display_text_cells(&pcols, &prows);
    }

    const uint16_t cols = size_from_cfg(cfg, "console.cols", AG_CONSOLE_COLS,
                                        pcols, AG_SCREEN_MAX_COLS);
    const uint16_t rows = size_from_cfg(cfg, "console.rows", AG_CONSOLE_ROWS,
                                        prows, AG_SCREEN_MAX_ROWS);

    /*
     * Nothing to do when the answer has not changed, which is the ordinary
     * case for the second call: a resize reflows the screen, and doing that
     * to an identical size would throw away the boot report for nothing.
     */
    static uint16_t s_cols, s_rows;
    if (cols == s_cols && rows == s_rows) {
        return;
    }

    const ag_err_t err = ag_console_resize(cols, rows);
    if (err != AG_OK) {
        kout("console: %ux%u refused with %d, staying at %ux%u\n",
             (unsigned)cols, (unsigned)rows, (int)err,
             (unsigned)AG_CONSOLE_COLS, (unsigned)AG_CONSOLE_ROWS);
        return;
    }
    s_cols = cols;
    s_rows = rows;
}

static ag_err_t stage_devices(void)
{
    const ag_err_t err = ag_devices_init();
    apply_console_size();
    return err;
}

static ag_err_t stage_modules(void)
{
    /*
     * Recovery is decided here: storage and config are already up (so the
     * marker and SYSTEM.CFG can be read), but loadable .SYS have not run yet.
     */
    ag_boot_recovery_begin();
    if (ag_boot_in_recovery()) {
        return AG_OK;
    }
    const ag_err_t err = ag_modules_boot();

    /*
     * And again, now that the loadable drivers are up.
     *
     * `auto` asks the panel first, and on a board whose panel is a .SYS the
     * panel does not exist yet at the devices stage: the question fell through
     * to the framebuffer and a CYD answered 20x7 (a 160x120 surface in 8x16
     * cells) for glass that holds 40x30.  That is worse than the built-in
     * size, so the one documented way to fit the console to the screen made
     * the console smaller.  Asked a second time here, the panel answers for
     * itself; an explicit size in SYSTEM.CFG gets the same number twice and
     * the call does nothing.
     */
    apply_console_size();
    return err;
}

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
    /* After the configuration, because [power] lives in it. */
    [AG_STAGE_POWER]      = {"power",      ag_powerctl_init, false},
    /* Also the console size from [console]: `auto` asks the panel. */
    [AG_STAGE_DEVICES]    = {"devices",    stage_devices,  false},
    [AG_STAGE_MEDIA]      = {"media",      ag_storage_mount_media, false},
    [AG_STAGE_MODULES]    = {"modules",    stage_modules,  false},
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
    const int64_t t0 = ag_port_us();

    memset(&s_report, 0, sizeof(s_report));

    bool fatal_failure = false;

    for (int i = 0; i < AG_STAGE_COUNT; i++) {
        const ag_stage_desc_t *st = &s_stages[i];

        if (st->fn == NULL) {
            s_report.stage_result[i] = -AG_ENOSYS;
            s_report.degraded = true;
            continue;
        }

        const int64_t ts = ag_port_us();
        const ag_err_t err = st->fn();
        s_report.stage_us[i] = (uint32_t)(ag_port_us() - ts);
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

    s_report.boot_us = (uint32_t)(ag_port_us() - t0);
    print_summary();

    if (fatal_failure) {
        /* Keep the board alive so the failure can be read off the console. */
        for (;;) {
            ag_port_task_delay(ag_port_ms_to_ticks(1000));
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
