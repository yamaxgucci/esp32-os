/*
 * ArgonOS - platform detection.
 *
 * Decides which execution profile the board can support before anything else
 * is initialised: the loader, the memory manager and the console all size
 * themselves from the answer.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "platform.h"

#include <string.h>

#include <argon/kernel.h>

#include <argon/port/mem.h>
#include <argon/port/sys.h>

#ifndef ARGON_BUILD_ID
#define ARGON_BUILD_ID "dev"
#endif

/* An application worth loading needs at least this much room for .text. */
#define AG_LITE_MIN_SRAM (192u * 1024u)

/* Below this the board can host services but not loadable applications. */
#define AG_FULL_MIN_PSRAM (2u * 1024u * 1024u)

static ag_platform_t s_plat;

ag_err_t ag_platform_init(void)
{
    memset(&s_plat, 0, sizeof(s_plat));
    s_plat.chip_revision = ag_port_cpu_revision();

    s_plat.sram_total = ag_port_mem_total(AG_MEM_FAST);
    s_plat.sram_free_at_boot = ag_port_mem_free(AG_MEM_FAST);

    s_plat.psram_total = ag_port_psram_size();
    s_plat.psram_present = s_plat.psram_total != 0;

    /*
     * The port answers whether instructions can be fetched from PSRAM at all.
     * Whether a particular mapping succeeds is spike S-1's business, on real
     * hardware; until then the loader treats this as "probably yes, verify at
     * map time".
     */
    s_plat.psram_executable =
        s_plat.psram_present && ag_port_psram_executable();

    if (s_plat.psram_executable && s_plat.psram_total >= AG_FULL_MIN_PSRAM) {
        s_plat.profile = AG_PROFILE_FULL;
    } else if (s_plat.sram_free_at_boot >= AG_LITE_MIN_SRAM) {
        s_plat.profile = AG_PROFILE_LITE;
    } else {
        s_plat.profile = AG_PROFILE_NANO;
    }

    return AG_OK;
}

const ag_platform_t *ag_platform(void) { return &s_plat; }

const char *ag_profile_name(ag_profile_t p)
{
    switch (p) {
    case AG_PROFILE_FULL: return "full";
    case AG_PROFILE_LITE: return "lite";
    default:              return "nano";
    }
}

void ag_platform_fill_sysinfo(ag_sysinfo_t *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->os_name, "ArgonOS", sizeof(out->os_name) - 1);
    strncpy(out->os_version, ARGON_VERSION_STR, sizeof(out->os_version) - 1);
    strncpy(out->build, ARGON_BUILD_ID, sizeof(out->build) - 1);
    strncpy(out->chip, AG_PORT_TARGET_NAME, sizeof(out->chip) - 1);
    strncpy(out->board, "generic", sizeof(out->board) - 1);
    strncpy(out->profile, ag_profile_name(s_plat.profile),
            sizeof(out->profile) - 1);

    const uint8_t cores = ag_port_cpu_cores();

    out->cpu_hz = ag_port_cpu_hz();
    out->cpu_cores = cores;
    /*
     * With two cores the application owns core 1 outright; on a single core
     * part it shares core 0 with the kernel and the DOS illusion is weaker.
     */
    out->app_core = (cores > 1) ? 1 : 0;
    out->abi_major = AG_ABI_MAJOR;
    out->abi_minor = AG_ABI_MINOR;
}
