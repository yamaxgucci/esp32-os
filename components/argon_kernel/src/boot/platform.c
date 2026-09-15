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

#include <stdio.h>
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

/*
 * A label field, filled and cut to fit.
 *
 * strncpy cannot say that truncation is intended and snprintf cannot
 * either - the compiler proves the constant does not fit and refuses both.
 * memcpy with a length worked out here says what is meant and is checked
 * the same way.
 */
static void put_field(char *dst, size_t cap, const char *src)
{
    const size_t n = strlen(src);
    const size_t k = (n < cap - 1u) ? n : (cap - 1u);

    memcpy(dst, src, k);
    dst[k] = '\0';
}

void ag_platform_fill_sysinfo(ag_sysinfo_t *out)
{
    /*
     * These fields are labels and are meant to be cut to fit.  Nothing
     * here had noticed that the build id no longer does - 39 characters
     * into 24 - because the game builds the app and not the firmware.
     */
    memset(out, 0, sizeof(*out));
    put_field(out->os_name, sizeof(out->os_name), "ArgonOS");
    put_field(out->os_version, sizeof(out->os_version), ARGON_VERSION_STR);
    {
        /*
         * A git describe identifies itself at its END - the commit hash -
         * and on a long-lived branch the name in front of it is longer than
         * this field on its own.  So when it does not fit, keep the tail.
         */
        const char  *id = ARGON_BUILD_ID;
        const size_t n  = strlen(id);

        if (n >= sizeof(out->build)) {
            id += n - (sizeof(out->build) - 1u);
        }
        put_field(out->build, sizeof(out->build), id);
    }
    put_field(out->chip, sizeof(out->chip), AG_PORT_TARGET_NAME);
    put_field(out->board, sizeof(out->board), "generic");
    put_field(out->profile, sizeof(out->profile),
              ag_profile_name(s_plat.profile));

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
