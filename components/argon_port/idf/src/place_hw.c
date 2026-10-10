/*
 * ArgonOS port: ESP-IDF - what the linker did with each build-time piece of
 * [memory].  The kernel compares it with the configuration and says
 * "rebuild" when the two disagree (`mem /p`).
 *
 * Each answer is the build's own sdkconfig, the same switches
 * tools/argon.ps1 writes from a board pack's [memory] section.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/place.h>

#include <string.h>

#include "esp_memory_utils.h"
#include "sdkconfig.h"

static const char *iram_or_flash(bool in_flash)
{
    return in_flash ? "flash" : "iram";
}

const char *ag_port_place_built(const char *piece)
{
    if (piece == NULL) {
        return NULL;
    }
#if defined(CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH) && CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH
    const bool freertos = true;
#else
    const bool freertos = false;
#endif
#if defined(CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH) && CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH
    const bool heap = true;
#else
    const bool heap = false;
#endif
#if defined(CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH) && CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH
    const bool ringbuf = true;
#else
    const bool ringbuf = false;
#endif
#if defined(CONFIG_ESP_WIFI_IRAM_OPT) && CONFIG_ESP_WIFI_IRAM_OPT
    const bool wifi = false;
#else
    const bool wifi = true;
#endif
#if defined(CONFIG_ESP_PHY_IRAM_OPT) && CONFIG_ESP_PHY_IRAM_OPT
    const bool phy = false;
#else
    const bool phy = true;
#endif
#if defined(CONFIG_LIBC_LOCKS_PLACE_IN_IRAM) && CONFIG_LIBC_LOCKS_PLACE_IN_IRAM
    const bool locks = false;
#else
    const bool locks = true;
#endif
    /* The I2C master's handler stands for the group: argon.ps1 sets them all
     * together. */
#if defined(CONFIG_I2C_MASTER_ISR_HANDLER_IN_IRAM) && CONFIG_I2C_MASTER_ISR_HANDLER_IN_IRAM
    const bool isr = false;
#else
    const bool isr = true;
#endif
#if defined(CONFIG_SPI_FLASH_ROM_IMPL) && CONFIG_SPI_FLASH_ROM_IMPL
    const char *flash_driver = "rom";
#else
    const char *flash_driver = "iram";
#endif
#if defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY && \
    defined(CONFIG_ARGON_PLACE_KERNEL_STATIC_PSRAM) && CONFIG_ARGON_PLACE_KERNEL_STATIC_PSRAM
    const char *kstatic = "psram";
#else
    const char *kstatic = "iram";
#endif
#if defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) && CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY && \
    defined(CONFIG_ARGON_PLACE_DEV_TABLE_PSRAM) && CONFIG_ARGON_PLACE_DEV_TABLE_PSRAM
    const char *devtab = "psram";
#else
    const char *devtab = "iram";
#endif

    if (strcmp(piece, "freertos") == 0)      return iram_or_flash(freertos);
    if (strcmp(piece, "heap.code") == 0)     return iram_or_flash(heap);
    if (strcmp(piece, "ringbuf.code") == 0)  return iram_or_flash(ringbuf);
    if (strcmp(piece, "phy.fast") == 0)      return iram_or_flash(phy);
    if (strcmp(piece, "libc.locks") == 0)    return iram_or_flash(locks);
    if (strcmp(piece, "driver.isr") == 0)    return iram_or_flash(isr);
    if (strcmp(piece, "flash.driver") == 0)  return flash_driver;
    if (strcmp(piece, "kernel.static") == 0) return kstatic;
    if (strcmp(piece, "dev.table") == 0)     return devtab;
    if (strcmp(piece, "wifi.fast") == 0) {
#if defined(CONFIG_ESP_WIFI_ENABLED) && CONFIG_ESP_WIFI_ENABLED && \
    defined(CONFIG_ARGON_NET_WIFI) && CONFIG_ARGON_NET_WIFI
        return iram_or_flash(wifi);
#else
        (void)wifi;
        return NULL; /* no radio in this build */
#endif
    }
    return NULL;
}

bool ag_port_ptr_is_slow(const void *p) { return esp_ptr_external_ram(p); }
