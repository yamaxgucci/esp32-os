/*
 * ArgonOS port: ESP-IDF - littlefs on flash, FAT on a card.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/storage.h>

#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_littlefs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

/* How many files may be open on the card at once. */
#define AG_PORT_SD_MAX_FILES 8

/* The chip numbers SPI hosts from one below what the schematic calls them. */
#define AG_PORT_SPI_HOST_OF(chip) ((chip) - 1)

ag_err_t ag_port_sysfs_mount(const char *label, const char *base_path,
                             bool format_if_needed)
{
    if (label == NULL || base_path == NULL) {
        return -AG_EINVAL;
    }

    const esp_vfs_littlefs_conf_t cfg = {
        .base_path = base_path,
        .partition_label = label,
        .format_if_mount_failed = format_if_needed,
        .dont_mount = false,
    };
    return (esp_vfs_littlefs_register(&cfg) == ESP_OK) ? AG_OK : -AG_EIO;
}

ag_err_t ag_port_sysfs_unmount(const char *label)
{
    if (label == NULL) {
        return -AG_EINVAL;
    }
    return (esp_vfs_littlefs_unregister(label) == ESP_OK) ? AG_OK : -AG_EIO;
}

ag_err_t ag_port_sysfs_space(const char *label, uint64_t *total, uint64_t *used)
{
    if (label == NULL || total == NULL || used == NULL) {
        return -AG_EINVAL;
    }

    size_t bytes_total = 0;
    size_t bytes_used = 0;
    if (esp_littlefs_info(label, &bytes_total, &bytes_used) != ESP_OK) {
        return -AG_EIO;
    }
    *total = (uint64_t)bytes_total;
    *used = (uint64_t)bytes_used;
    return AG_OK;
}

/* ---------------------------------------------------------------------- */
/* Removable media                                                        */
/* ---------------------------------------------------------------------- */

static esp_vfs_fat_mount_config_t mount_config(bool allow_format)
{
    const esp_vfs_fat_mount_config_t cfg = {
        .max_files = AG_PORT_SD_MAX_FILES,
        .format_if_mount_failed = allow_format,
        .allocation_unit_size = 16 * 1024,
    };
    return cfg;
}

static esp_err_t mount_native(const ag_port_sd_cfg_t *sd, const char *base,
                              bool allow_format, sdmmc_card_t **out)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = (int)sd->max_khz;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = sd->width;
    /* Boards rarely fit the external pull-ups the SD specification asks for. */
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

#ifdef SOC_SDMMC_USE_GPIO_MATRIX
    slot.clk = sd->clk;
    slot.cmd = sd->cmd;
    slot.d0 = sd->d0;
    if (sd->width == 4) {
        slot.d1 = sd->d1;
        slot.d2 = sd->d2;
        slot.d3 = sd->d3;
    }
#endif

    const esp_vfs_fat_mount_config_t cfg = mount_config(allow_format);
    return esp_vfs_fat_sdmmc_mount(base, &host, &slot, &cfg, out);
}

static esp_err_t mount_spi(const ag_port_sd_cfg_t *sd, const char *base,
                           bool allow_format, sdmmc_card_t **out)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = AG_PORT_SPI_HOST_OF((int)sd->spi_host);
    host.max_freq_khz = (int)sd->max_khz;

    const spi_bus_config_t bus = {
        .mosi_io_num = sd->mosi,
        .miso_io_num = sd->miso,
        .sclk_io_num = sd->sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t err = spi_bus_initialize((spi_host_device_t)host.slot, &bus,
                                       SPI_DMA_CH_AUTO);
    /* Somebody else may already own the bus, which is fine. */
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
    device.gpio_cs = sd->cs;
    device.host_id = (spi_host_device_t)host.slot;

    const esp_vfs_fat_mount_config_t cfg = mount_config(allow_format);
    return esp_vfs_fat_sdspi_mount(base, &host, &device, &cfg, out);
}

ag_err_t ag_port_sd_mount(const ag_port_sd_cfg_t *cfg, const char *base_path,
                          bool allow_format, ag_port_sd_t *out)
{
    if (cfg == NULL || base_path == NULL || out == NULL) {
        return -AG_EINVAL;
    }
    if (cfg->kind == AG_PORT_SD_NONE) {
        return -AG_ENODEV;
    }

    sdmmc_card_t   *card = NULL;
    const esp_err_t err = (cfg->kind == AG_PORT_SD_NATIVE)
                              ? mount_native(cfg, base_path, allow_format, &card)
                              : mount_spi(cfg, base_path, allow_format, &card);
    if (err != ESP_OK) {
        return -AG_EIO;
    }
    *out = card;
    return AG_OK;
}

ag_err_t ag_port_sd_unmount(const char *base_path, ag_port_sd_t card)
{
    if (base_path == NULL || card == NULL) {
        return -AG_EINVAL;
    }
    return (esp_vfs_fat_sdcard_unmount(base_path, (sdmmc_card_t *)card) == ESP_OK)
               ? AG_OK
               : -AG_EIO;
}

uint32_t ag_port_sd_sector_size(ag_port_sd_t card)
{
    if (card == NULL) {
        return 0u;
    }
    const sdmmc_card_t *c = (const sdmmc_card_t *)card;
    return (c->csd.sector_size > 0) ? (uint32_t)c->csd.sector_size : 512u;
}

uint64_t ag_port_sd_sectors(ag_port_sd_t card)
{
    return (card != NULL) ? (uint64_t)((const sdmmc_card_t *)card)->csd.capacity
                          : 0u;
}

ag_err_t ag_port_sd_read_sectors(ag_port_sd_t card, uint64_t sector, void *buf,
                                 uint32_t count)
{
    if (card == NULL || buf == NULL || count == 0) {
        return -AG_EINVAL;
    }
    return (sdmmc_read_sectors((sdmmc_card_t *)card, buf, (size_t)sector,
                               (size_t)count) == ESP_OK)
               ? AG_OK
               : -AG_EIO;
}

ag_err_t ag_port_media_space(const char *base_path, uint64_t *total,
                             uint64_t *avail)
{
    if (base_path == NULL || total == NULL || avail == NULL) {
        return -AG_EINVAL;
    }

    uint64_t bytes_total = 0;
    uint64_t bytes_free = 0;
    if (esp_vfs_fat_info(base_path, &bytes_total, &bytes_free) != ESP_OK) {
        return -AG_EIO;
    }
    *total = bytes_total;
    *avail = bytes_free;
    return AG_OK;
}
