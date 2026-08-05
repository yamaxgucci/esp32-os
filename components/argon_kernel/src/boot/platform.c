/*
 * ArgonOS - platform detection.
 *
 * Decides which execution profile the board can support before anything else
 * is initialised: the loader, the memory manager and the console all size
 * themselves from the answer.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "platform.h"

#include <string.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "sdkconfig.h"

#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

#include <argon/kernel.h>

#ifndef ARGON_BUILD_ID
#define ARGON_BUILD_ID "dev"
#endif

/* An application worth loading needs at least this much room for .text. */
#define AG_LITE_MIN_SRAM (192u * 1024u)

/* Below this the board can host services but not loadable applications. */
#define AG_FULL_MIN_PSRAM (2u * 1024u * 1024u)

static ag_platform_t s_plat;

/*
 * Runtime code placement into PSRAM needs an MMU that can map the SPI RAM
 * into the instruction bus.  Only the S3 and P4 can do that today; on the
 * original ESP32 the PSRAM is data-only.
 */
static bool psram_is_executable(void)
{
#if CONFIG_SPIRAM && (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4)
    /*
     * The capability is architectural, but the mapping call is validated by
     * spike S-1 on real hardware before the loader relies on it.  Until then
     * the loader treats this as "probably yes, verify at map time".
     */
    return true;
#else
    return false;
#endif
}

ag_err_t ag_platform_init(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    memset(&s_plat, 0, sizeof(s_plat));
    s_plat.chip_revision = (uint8_t)(chip.revision / 100);

    s_plat.sram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    s_plat.sram_free_at_boot = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

#if CONFIG_SPIRAM
    s_plat.psram_present = esp_psram_is_initialized();
    if (s_plat.psram_present) {
        s_plat.psram_total = esp_psram_get_size();
    }
#endif
    s_plat.psram_executable = s_plat.psram_present && psram_is_executable();

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
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    memset(out, 0, sizeof(*out));
    strncpy(out->os_name, "ArgonOS", sizeof(out->os_name) - 1);
    strncpy(out->os_version, ARGON_VERSION_STR, sizeof(out->os_version) - 1);
    strncpy(out->build, ARGON_BUILD_ID, sizeof(out->build) - 1);
    strncpy(out->chip, CONFIG_IDF_TARGET, sizeof(out->chip) - 1);
    strncpy(out->board, "generic", sizeof(out->board) - 1);
    strncpy(out->profile, ag_profile_name(s_plat.profile),
            sizeof(out->profile) - 1);

    out->cpu_hz = (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000u;
    out->cpu_cores = (uint8_t)chip.cores;
    /*
     * With two cores the application owns core 1 outright; on a single core
     * part it shares core 0 with the kernel and the DOS illusion is weaker.
     */
    out->app_core = (chip.cores > 1) ? 1 : 0;
    out->abi_major = AG_ABI_MAJOR;
    out->abi_minor = AG_ABI_MINOR;
}
