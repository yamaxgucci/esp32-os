/*
 * ArgonOS - storage bring-up.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "fs/storage.h"

#include <string.h>
#include <time.h>

#include <argon/board.h>
#include <argon/device.h>
#include <argon/log.h>
#include <argon/ramfs.h>
#include <argon/vfs.h>

#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

#include "boot/platform.h"
#include "fs/idfvfs.h"

/* /tmp is generous when there is PSRAM to spend and modest when there is not. */
#define AG_TMP_BUDGET_PSRAM (1024u * 1024u)
#define AG_TMP_BUDGET_SRAM (32u * 1024u)

/* Where ESP-IDF mounts each filesystem, before we re-expose it. */
#define AG_SYS_BASE_PATH "/flash"
#define AG_SYS_PARTITION "sysfs"
#define AG_SYS_MAX_FILES 6

#define AG_SD_BASE_PATH "/sdcard"
#define AG_SD_MAX_FILES 8

static SemaphoreHandle_t s_vfs_mutex;
static ag_ramfs_t       *s_tmpfs;
static ag_idfvfs_t      *s_sysfs;
static wl_handle_t       s_sys_wl = WL_INVALID_HANDLE;
static ag_idfvfs_t      *s_sdfs;
static sdmmc_card_t     *s_sd_card;

static void vfs_lock(void *ctx)
{
    (void)ctx;
    xSemaphoreTakeRecursive(s_vfs_mutex, portMAX_DELAY);
}

static void vfs_unlock(void *ctx)
{
    (void)ctx;
    xSemaphoreGiveRecursive(s_vfs_mutex);
}

void *ag_storage_vfs_lock_holder(void)
{
    if (s_vfs_mutex == NULL) {
        return NULL;
    }
    return (void *)xSemaphoreGetMutexHolder(s_vfs_mutex);
}

static uint64_t now_unix(void)
{
    /*
     * Without an RTC or a time server this counts from the epoch at boot.
     * Timestamps are therefore relative, which is honest and still lets a
     * directory listing be sorted by age.
     */
    return (uint64_t)time(NULL);
}

/* Prefer PSRAM so that a large /tmp does not eat the scarce internal RAM. */
static void *tmp_alloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

static void tmp_free(void *ptr) { heap_caps_free(ptr); }

static ag_err_t sys_space(void *ctx, uint64_t *total, uint64_t *available)
{
    (void)ctx;
    uint64_t bytes_total = 0;
    uint64_t bytes_free = 0;

    if (esp_vfs_fat_info(AG_SYS_BASE_PATH, &bytes_total, &bytes_free) !=
        ESP_OK) {
        return -AG_EIO;
    }
    *total = bytes_total;
    *available = bytes_free;
    return AG_OK;
}

/*
 * The internal flash filesystem, mounted at /sys and shown as drive C:.
 *
 * FAT is used because ESP-IDF provides it and its wear levelling out of the
 * box.  littlefs would survive a power cut mid-write, which matters for a
 * system that keeps its configuration and log here; that is a swap of this one
 * call once the dependency is worth taking on.  See docs/04-roadmap.md.
 *
 * A failure here is not fatal: the board still boots to a prompt with /tmp.
 */
static ag_err_t mount_internal_flash(void)
{
    const esp_vfs_fat_mount_config_t cfg = {
        .max_files = AG_SYS_MAX_FILES,
        /* A blank or corrupt partition is formatted rather than left dead. */
        .format_if_mount_failed = true,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .use_one_fat = false,
    };

    if (esp_vfs_fat_spiflash_mount_rw_wl(AG_SYS_BASE_PATH, AG_SYS_PARTITION,
                                         &cfg, &s_sys_wl) != ESP_OK) {
        return -AG_EIO;
    }

    ag_err_t err = ag_idfvfs_create(AG_SYS_BASE_PATH, "fat", sys_space, NULL,
                                    &s_sysfs);
    if (err != AG_OK) {
        esp_vfs_fat_spiflash_unmount_rw_wl(AG_SYS_BASE_PATH, s_sys_wl);
        s_sys_wl = WL_INVALID_HANDLE;
        return err;
    }

    err = ag_vfs_mount("/sys", ag_idfvfs_ops(), s_sysfs, 0);
    if (err != AG_OK) {
        ag_idfvfs_destroy(s_sysfs);
        s_sysfs = NULL;
        esp_vfs_fat_spiflash_unmount_rw_wl(AG_SYS_BASE_PATH, s_sys_wl);
        s_sys_wl = WL_INVALID_HANDLE;
        return err;
    }
    return AG_OK;
}

/* ---------------------------------------------------------------------- */
/* Removable media                                                        */
/* ---------------------------------------------------------------------- */

static ag_err_t sd_space(void *ctx, uint64_t *total, uint64_t *available)
{
    (void)ctx;
    uint64_t bytes_total = 0;
    uint64_t bytes_free = 0;

    if (esp_vfs_fat_info(AG_SD_BASE_PATH, &bytes_total, &bytes_free) != ESP_OK) {
        return -AG_EIO;
    }
    *total = bytes_total;
    *available = bytes_free;
    return AG_OK;
}

/*
 * A card is somebody's data.  Formatting happens only when the operator asks
 * for it by name, never as a way to recover from a failed mount.  That is the
 * opposite of the choice made for internal flash, where a blank partition has
 * nothing on it to lose.
 */
static esp_vfs_fat_mount_config_t sd_mount_config(bool allow_format)
{
    const esp_vfs_fat_mount_config_t cfg = {
        .max_files = AG_SD_MAX_FILES,
        .format_if_mount_failed = allow_format,
        .allocation_unit_size = 16 * 1024,
    };
    return cfg;
}

static esp_err_t mount_sd_sdmmc(const ag_board_sd_t *sd, bool allow_format)
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

    const esp_vfs_fat_mount_config_t cfg = sd_mount_config(allow_format);
    return esp_vfs_fat_sdmmc_mount(AG_SD_BASE_PATH, &host, &slot, &cfg,
                                   &s_sd_card);
}

static esp_err_t mount_sd_spi(const ag_board_sd_t *sd, bool allow_format)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    /* sd.spi_host is the chip's number, as the schematic writes it. */
    host.slot = AG_SPI_HOST_OF((int)sd->spi_host);
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

    const esp_vfs_fat_mount_config_t cfg = sd_mount_config(allow_format);
    return esp_vfs_fat_sdspi_mount(AG_SD_BASE_PATH, &host, &device, &cfg,
                                   &s_sd_card);
}

/* ---------------------------------------------------------------------- */
/* The media as devices                                                   */
/* ---------------------------------------------------------------------- */

/*
 * Both of these are read-only, and that is a decision rather than a gap.  The
 * filesystem above them is mounted and has its own cache; writing underneath it
 * would corrupt what it thinks it wrote, and on the flash partition it would
 * also go behind the wear levelling, which is the one thing keeping the board
 * from wearing a hole in one sector.  Reading is what a device node is for
 * here: looking at what is actually on the media when the filesystem disagrees
 * with what you expect.
 */

static ag_device_t *s_flash_dev;
static ag_device_t *s_sd_dev;
static uint8_t     *s_sd_bounce; /* one sector, DMA capable                */

static uint64_t flash_size(ag_device_t *dev)
{
    return ((const esp_partition_t *)dev->priv)->size;
}

static int32_t flash_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    const esp_partition_t *part = (const esp_partition_t *)dev->priv;

    if (off >= part->size) {
        return 0;
    }
    if (off + len > part->size) {
        len = (size_t)(part->size - off);
    }
    if (esp_partition_read(part, (size_t)off, buf, len) != ESP_OK) {
        return -AG_EIO;
    }
    return (int32_t)len;
}

static ag_err_t flash_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                            size_t arglen)
{
    const esp_partition_t *part = (const esp_partition_t *)dev->priv;

    if (cmd != AG_IOC_GEOMETRY) {
        return -AG_ENOTSUP;
    }
    if (arg == NULL || arglen < sizeof(ag_geometry_t)) {
        return -AG_EINVAL;
    }

    /* The erase block, not the read granularity: it is what a write costs. */
    const uint32_t erase = (part->erase_size > 0) ? part->erase_size : 4096u;

    ag_geometry_t *geo = (ag_geometry_t *)arg;
    geo->sector_size = erase;
    geo->sectors = part->size / erase;
    return AG_OK;
}

static const ag_dev_ops_t k_flash_ops = {
    .read = flash_read,
    .ioctl = flash_ioctl,
    .size = flash_size,
};

static uint64_t sd_sector_size(const sdmmc_card_t *card)
{
    return (card->csd.sector_size > 0) ? (uint64_t)card->csd.sector_size : 512u;
}

static uint64_t sd_dev_size(ag_device_t *dev)
{
    const sdmmc_card_t *card = (const sdmmc_card_t *)dev->priv;
    return (uint64_t)card->csd.capacity * sd_sector_size(card);
}

/*
 * A sector at a time through a bounce buffer, so that a read of sixteen bytes
 * at an odd offset works - which is what a hexdump of the boot sector is.  It
 * is not the way to copy a whole card and does not pretend to be; the card is
 * mounted as A: and copying files off it is what that mount is for.
 */
static int32_t sd_dev_read(ag_device_t *dev, void *buf, size_t len,
                           uint64_t off)
{
    sdmmc_card_t  *card = (sdmmc_card_t *)dev->priv;
    const uint64_t ss = sd_sector_size(card);
    const uint64_t total = (uint64_t)card->csd.capacity * ss;

    if (s_sd_bounce == NULL) {
        return -AG_ENOMEM;
    }
    if (off >= total) {
        return 0;
    }
    if (off + len > total) {
        len = (size_t)(total - off);
    }

    uint8_t *out = (uint8_t *)buf;
    size_t   done = 0;
    while (done < len) {
        const uint64_t here = off + done;
        const uint64_t sector = here / ss;
        const size_t   in_sector = (size_t)(here % ss);
        size_t         chunk = (size_t)ss - in_sector;
        if (chunk > len - done) {
            chunk = len - done;
        }

        if (sdmmc_read_sectors(card, s_sd_bounce, (size_t)sector, 1) !=
            ESP_OK) {
            /* Partial success is still success: say how much arrived. */
            return (done > 0) ? (int32_t)done : -AG_EIO;
        }
        memcpy(out + done, s_sd_bounce + in_sector, chunk);
        done += chunk;
    }
    return (int32_t)done;
}

static ag_err_t sd_dev_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                             size_t arglen)
{
    const sdmmc_card_t *card = (const sdmmc_card_t *)dev->priv;

    if (cmd != AG_IOC_GEOMETRY) {
        return -AG_ENOTSUP;
    }
    if (arg == NULL || arglen < sizeof(ag_geometry_t)) {
        return -AG_EINVAL;
    }

    ag_geometry_t *geo = (ag_geometry_t *)arg;
    geo->sector_size = (uint32_t)sd_sector_size(card);
    geo->sectors = card->csd.capacity;
    return AG_OK;
}

static const ag_dev_ops_t k_sd_ops = {
    .read = sd_dev_read,
    .ioctl = sd_dev_ioctl,
    .size = sd_dev_size,
};

void ag_storage_register_devices(void)
{
    if (s_flash_dev != NULL) {
        return;
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, AG_SYS_PARTITION);
    if (part == NULL) {
        return;
    }

    const ag_dev_desc_t desc = {
        .name = "flash0",
        .driver = "spiflash",
        .cls = AG_DEV_STORAGE,
        .flags = AG_DEVF_READONLY,
        .ops = &k_flash_ops,
        .priv = (void *)part,
    };
    (void)ag_dev_register(&desc, &s_flash_dev);
}

static void sd_register_device(void)
{
    if (s_sd_dev != NULL || s_sd_card == NULL) {
        return;
    }

    if (s_sd_bounce == NULL) {
        s_sd_bounce = heap_caps_malloc((size_t)sd_sector_size(s_sd_card),
                                       MALLOC_CAP_DMA);
        if (s_sd_bounce == NULL) {
            ag_log(AG_LOG_WARN, "dev", "no DMA buffer for sd0");
            return;
        }
    }

    const ag_dev_desc_t desc = {
        .name = "sd0",
        .driver = (ag_board()->sd.kind == AG_SD_SDMMC) ? "sdmmc" : "sdspi",
        .cls = AG_DEV_STORAGE,
        .flags = AG_DEVF_READONLY | AG_DEVF_HOTPLUG,
        .ops = &k_sd_ops,
        .priv = s_sd_card,
    };
    (void)ag_dev_register(&desc, &s_sd_dev);
}

/*
 * The card is going away.  Revoke rather than unregister: somebody may be
 * holding a handle, and they have to be told on their next call instead of
 * being waited for.
 */
static void sd_revoke_device(void)
{
    if (s_sd_dev != NULL) {
        (void)ag_dev_revoke(s_sd_dev);
        s_sd_dev = NULL;
    }
}

/* ---------------------------------------------------------------------- */

static void unmount_media(void)
{
    sd_revoke_device();

    if (s_sdfs != NULL) {
        /* Eject rather than unmount: whatever is open must fail, not block. */
        (void)ag_vfs_eject("/sd");
        ag_idfvfs_destroy(s_sdfs);
        s_sdfs = NULL;
    }
    if (s_sd_card != NULL) {
        (void)esp_vfs_fat_sdcard_unmount(AG_SD_BASE_PATH, s_sd_card);
        s_sd_card = NULL;
    }
}

static ag_err_t mount_media(bool allow_format)
{
    const ag_board_sd_t *sd = &ag_board()->sd;

    if (sd->kind == AG_SD_NONE) {
        return -AG_ENODEV;
    }

    const esp_err_t err = (sd->kind == AG_SD_SDMMC)
                              ? mount_sd_sdmmc(sd, allow_format)
                              : mount_sd_spi(sd, allow_format);
    if (err != ESP_OK) {
        return -AG_EIO;
    }

    ag_err_t rc = ag_idfvfs_create(AG_SD_BASE_PATH, "fat", sd_space, NULL,
                                   &s_sdfs);
    if (rc == AG_OK) {
        rc = ag_vfs_mount("/sd", ag_idfvfs_ops(), s_sdfs, AG_MOUNT_REMOVABLE);
        if (rc != AG_OK) {
            ag_idfvfs_destroy(s_sdfs);
            s_sdfs = NULL;
        }
    }

    if (rc != AG_OK) {
        (void)esp_vfs_fat_sdcard_unmount(AG_SD_BASE_PATH, s_sd_card);
        s_sd_card = NULL;
        return rc;
    }

    /* The card is here and readable, so it is also a device.  A failure to
     * register it costs the device node and nothing else, so the mount stands. */
    sd_register_device();
    return AG_OK;
}

ag_err_t ag_storage_mount_media(void) { return mount_media(false); }

ag_err_t ag_storage_format_media(void)
{
    unmount_media();
    return mount_media(true);
}

bool ag_storage_media_present(void) { return s_sd_card != NULL; }

ag_err_t ag_storage_init(void)
{
    if (s_vfs_mutex == NULL) {
        s_vfs_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_vfs_mutex == NULL) {
            return -AG_ENOMEM;
        }
    }

    const ag_vfs_lock_t lock = {
        .lock = vfs_lock,
        .unlock = vfs_unlock,
        .ctx = NULL,
    };
    ag_err_t err = ag_vfs_init(&lock);
    if (err != AG_OK) {
        return err;
    }

    const bool psram = ag_platform()->psram_present;
    const ag_ramfs_config_t cfg = {
        .budget = psram ? AG_TMP_BUDGET_PSRAM : AG_TMP_BUDGET_SRAM,
        .now_unix = now_unix,
        .alloc = psram ? tmp_alloc : NULL,
        .release = psram ? tmp_free : NULL,
    };

    s_tmpfs = ag_ramfs_create(&cfg);
    if (s_tmpfs == NULL) {
        return -AG_ENOMEM;
    }

    err = ag_vfs_mount("/tmp", ag_ramfs_ops(), s_tmpfs, 0);
    if (err != AG_OK) {
        ag_ramfs_destroy(s_tmpfs);
        s_tmpfs = NULL;
        return err;
    }

    /*
     * Reported but not fatal.  A board whose internal flash will not mount is
     * degraded, not dead, and saying so beats refusing to boot.
     */
    const ag_err_t flash_err = mount_internal_flash();
    if (flash_err != AG_OK) {
        return flash_err;
    }

    return AG_OK;
}
