/*
 * ArgonOS - boot sequence.
 *
 * The boot sequence is a flat table of stages rather than a call chain, so
 * that a failing stage degrades the system instead of bricking the boot: a
 * board with no SD card, no display or no config file must still reach a
 * usable prompt on the serial console.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/kernel.h>

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "boot/platform.h"

typedef ag_err_t (*ag_stage_fn)(void);

typedef struct {
    const char  *name;
    ag_stage_fn  fn;    /* NULL while the stage is not implemented yet     */
    bool         fatal; /* boot cannot continue without it                 */
} ag_stage_desc_t;

static ag_sysinfo_t     s_sysinfo;
static ag_boot_report_t s_report;

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

/*
 * Stages are added here as they are implemented; see docs/04-roadmap.md.
 * Keeping the unimplemented ones visible makes the boot report honest about
 * what the system can and cannot do yet.
 */
static const ag_stage_desc_t s_stages[AG_STAGE_COUNT] = {
    [AG_STAGE_PLATFORM]   = {"platform",   stage_platform, true},
    [AG_STAGE_MEMORY]     = {"memory",     NULL,           true},
    [AG_STAGE_LOG]        = {"log",        NULL,           false},
    [AG_STAGE_BOARD]      = {"board",      NULL,           false},
    [AG_STAGE_CONSOLE]    = {"console",    NULL,           true},
    [AG_STAGE_STORAGE]    = {"storage",    NULL,           false},
    [AG_STAGE_CONFIG]     = {"config",     NULL,           false},
    [AG_STAGE_DEVICES]    = {"devices",    NULL,           false},
    [AG_STAGE_MEDIA]      = {"media",      NULL,           false},
    [AG_STAGE_MODULES]    = {"modules",    NULL,           false},
    [AG_STAGE_SUPERVISOR] = {"supervisor", NULL,           false},
    [AG_STAGE_SHELL]      = {"shell",      NULL,           true},
};

/* ---------------------------------------------------------------------- */

static void print_banner(void)
{
    const ag_platform_t *p = ag_platform();

    printf("\n");
    printf("ArgonOS %s (%s)  %s  profile=%s\n", s_sysinfo.os_version,
           s_sysinfo.build, s_sysinfo.chip, s_sysinfo.profile);
    printf("%u KB conventional memory, %u KB extended\n",
           (unsigned)(p->sram_free_at_boot / 1024u),
           (unsigned)(p->psram_total / 1024u));
    printf("ABI %u.%u  cores=%u  app core=%u\n", s_sysinfo.abi_major,
           s_sysinfo.abi_minor, s_sysinfo.cpu_cores, s_sysinfo.app_core);
    printf("\n");
}

static void print_report(void)
{
    printf("boot: %u us total%s\n", (unsigned)s_report.boot_us,
           s_report.degraded ? " (degraded)" : "");

    for (int i = 0; i < AG_STAGE_COUNT; i++) {
        const ag_stage_desc_t *st = &s_stages[i];
        if (st->fn == NULL) {
            printf("  %-11s  pending\n", st->name);
        } else if (s_report.stage_result[i] == AG_OK) {
            printf("  %-11s  ok      %6u us\n", st->name,
                   (unsigned)s_report.stage_us[i]);
        } else {
            printf("  %-11s  FAILED  err=%d\n", st->name,
                   (int)s_report.stage_result[i]);
        }
    }
}

void ag_kernel_main(void)
{
    const int64_t t0 = esp_timer_get_time();

    memset(&s_report, 0, sizeof(s_report));

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
            printf("argon: stage '%s' failed with %d\n", st->name, (int)err);
            if (st->fatal) {
                printf("argon: cannot continue\n");
                break;
            }
        }

        if (i == AG_STAGE_PLATFORM) {
            print_banner();
        }
    }

    s_report.boot_us = (uint32_t)(esp_timer_get_time() - t0);
    print_report();

    /*
     * The shell stage will take over from here.  Until it exists, park the
     * boot task so the board stays alive for inspection.
     */
    printf("\nargon: no shell yet, idling.\n");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

const ag_sysinfo_t *ag_sysinfo(void) { return &s_sysinfo; }

const ag_boot_report_t *ag_boot_report(void) { return &s_report; }

const ag_api_t *ag_kernel_api(void)
{
    /* Populated in phase 2 together with the process loader. */
    return NULL;
}
