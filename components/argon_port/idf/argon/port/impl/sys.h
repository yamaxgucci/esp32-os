/*
 * ArgonOS port: ESP-IDF - the machine itself.
 *
 * AG_PORT_NOINIT is RTC_NOINIT_ATTR: RTC slow memory keeps its contents across
 * everything except a power cycle, which is exactly the distinction the boot
 * counter needs - a crash streak must survive the reboot it causes and must not
 * survive somebody switching the board off and on to clear it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_SYS_H
#define ARGON_PORT_IMPL_SYS_H

#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "sdkconfig.h"

#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

#define AG_PORT_NOINIT      RTC_NOINIT_ATTR
#define AG_PORT_TARGET_NAME CONFIG_IDF_TARGET

static inline ag_reset_t ag_port_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return AG_RESET_POWERON;
    case ESP_RST_SW:      return AG_RESET_SOFTWARE;
    default:              return AG_RESET_OTHER;
    }
}

static inline void ag_port_restart(void) { esp_restart(); }

static inline uint8_t ag_port_cpu_cores(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    return (uint8_t)chip.cores;
}

static inline uint8_t ag_port_cpu_revision(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    return (uint8_t)(chip.revision / 100);
}

static inline uint32_t ag_port_cpu_hz(void)
{
    return (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000u;
}

static inline size_t ag_port_psram_size(void)
{
#if CONFIG_SPIRAM
    return esp_psram_is_initialized() ? esp_psram_get_size() : 0u;
#else
    return 0u;
#endif
}

/*
 * Running code out of PSRAM needs an MMU that can map the SPI RAM onto the
 * instruction bus.  Only the S3 and the P4 can; on the original ESP32 the PSRAM
 * is data-only.  The capability is architectural - whether a given map call
 * succeeds is spike S-1's business, on real hardware.
 */
static inline bool ag_port_psram_executable(void)
{
#if CONFIG_SPIRAM && (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4)
    return true;
#else
    return false;
#endif
}

#endif /* ARGON_PORT_IMPL_SYS_H */
