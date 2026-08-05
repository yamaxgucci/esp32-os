/*
 * ArgonOS - storage bring-up.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "fs/storage.h"

#include <time.h>

#include <argon/ramfs.h>
#include <argon/vfs.h>

#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "boot/platform.h"
#include "fs/idfvfs.h"

/* /tmp is generous when there is PSRAM to spend and modest when there is not. */
#define AG_TMP_BUDGET_PSRAM (1024u * 1024u)
#define AG_TMP_BUDGET_SRAM (32u * 1024u)

/* Where ESP-IDF mounts the internal flash filesystem, before we re-expose it. */
#define AG_SYS_BASE_PATH "/flash"
#define AG_SYS_PARTITION "sysfs"
#define AG_SYS_MAX_FILES 6

static SemaphoreHandle_t s_vfs_mutex;
static ag_ramfs_t       *s_tmpfs;
static ag_idfvfs_t      *s_sysfs;
static wl_handle_t       s_sys_wl = WL_INVALID_HANDLE;

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
