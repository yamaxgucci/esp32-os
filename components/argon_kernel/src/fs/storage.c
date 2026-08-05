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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "boot/platform.h"

/* /tmp is generous when there is PSRAM to spend and modest when there is not. */
#define AG_TMP_BUDGET_PSRAM (1024u * 1024u)
#define AG_TMP_BUDGET_SRAM (32u * 1024u)

static SemaphoreHandle_t s_vfs_mutex;
static ag_ramfs_t       *s_tmpfs;

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

    return AG_OK;
}
