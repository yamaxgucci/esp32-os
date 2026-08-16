/*
 * ArgonOS - storage bring-up.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fs/storage.h"

#include <string.h>
#include <time.h>

#include <argon/board.h>
#include <argon/device.h>
#include <argon/hostfs.h>
#include <argon/log.h>
#include <argon/ramfs.h>
#include <argon/vfs.h>

#include <argon/port/flash.h>
#include <argon/port/mem.h>
#include <argon/port/storage.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>

#include "boot/platform.h"
#include "fs/idfvfs.h"

/*
 * /tmp is generous when there is PSRAM to spend and modest when there is not.
 * SMS WAV capture (~1 MB PCM) needs headroom: ramfs grows by doubling, so a
 * ~880 KB file holds a 1 MB capacity block plus node overhead.
 */
#define AG_TMP_BUDGET_PSRAM (4u * 1024u * 1024u)
#define AG_TMP_BUDGET_SRAM (32u * 1024u)

/* Where the port mounts each filesystem, before we re-expose it. */
#define AG_SYS_BASE_PATH "/flash"
#define AG_SYS_PARTITION "sysfs"

#define AG_SD_BASE_PATH "/sdcard"

static ag_port_mutex_t s_vfs_mutex;
static ag_ramfs_t     *s_tmpfs;
static ag_idfvfs_t    *s_sysfs;
static ag_idfvfs_t    *s_sdfs;
static ag_port_sd_t    s_sd_card;

static void vfs_lock(void *ctx)
{
    (void)ctx;
    ag_port_mutex_take_recursive(s_vfs_mutex, AG_PORT_FOREVER);
}

static void vfs_unlock(void *ctx)
{
    (void)ctx;
    ag_port_mutex_give_recursive(s_vfs_mutex);
}

void *ag_storage_vfs_lock_holder(void)
{
    if (s_vfs_mutex == NULL) {
        return NULL;
    }
    return (void *)ag_port_mutex_holder(s_vfs_mutex);
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
    void *p = ag_port_alloc(bytes, AG_MEM_SLOW | AG_MEM_BYTE);
    if (p == NULL) {
        p = ag_port_alloc(bytes, AG_MEM_FAST | AG_MEM_BYTE);
    }
    return p;
}

static void tmp_free(void *ptr) { ag_port_free(ptr); }

static ag_err_t sys_space(void *ctx, uint64_t *total, uint64_t *available)
{
    (void)ctx;
    uint64_t bytes_total = 0;
    uint64_t bytes_used = 0;

    const ag_err_t err =
        ag_port_sysfs_space(AG_SYS_PARTITION, &bytes_total, &bytes_used);
    if (err != AG_OK) {
        return err;
    }
    *total = bytes_total;
    *available = (bytes_used < bytes_total) ? (bytes_total - bytes_used) : 0;
    return AG_OK;
}

/*
 * The internal flash filesystem, mounted at /sys and shown as drive C:.
 *
 * littlefs survives a power cut mid-write, which matters for configuration and
 * the crash log.  FAT remains for the SD card (A:).  A blank or foreign
 * partition is formatted on first mount.  See docs/04-roadmap.md.
 *
 * A failure here is not fatal: the board still boots to a prompt with /tmp.
 */
static ag_err_t mount_internal_flash(void)
{
    ag_err_t err =
        ag_port_sysfs_mount(AG_SYS_PARTITION, AG_SYS_BASE_PATH, true);
    if (err != AG_OK) {
        return err;
    }

    err = ag_idfvfs_create(AG_SYS_BASE_PATH, "lfs", sys_space, NULL, &s_sysfs);
    if (err != AG_OK) {
        (void)ag_port_sysfs_unmount(AG_SYS_PARTITION);
        return err;
    }

    err = ag_vfs_mount("/sys", ag_idfvfs_ops(), s_sysfs, 0);
    if (err != AG_OK) {
        ag_idfvfs_destroy(s_sysfs);
        s_sysfs = NULL;
        (void)ag_port_sysfs_unmount(AG_SYS_PARTITION);
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
    return ag_port_media_space(AG_SD_BASE_PATH, total, available);
}

/*
 * The board file describes the card the way the schematic does; the port wants
 * the same facts in its own struct.  Ten lines of copying is the price of the
 * kernel not knowing which chip it is on.
 */
static void sd_cfg_from_board(const ag_board_sd_t *sd, ag_port_sd_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = (sd->kind == AG_SD_SDMMC) ? AG_PORT_SD_NATIVE
                : (sd->kind == AG_SD_SPI) ? AG_PORT_SD_SPI
                                          : AG_PORT_SD_NONE;
    out->width = sd->width;
    out->max_khz = sd->max_khz;
    out->clk = sd->clk;
    out->cmd = sd->cmd;
    out->d0 = sd->d0;
    out->d1 = sd->d1;
    out->d2 = sd->d2;
    out->d3 = sd->d3;
    out->sck = sd->sck;
    out->mosi = sd->mosi;
    out->miso = sd->miso;
    out->cs = sd->cs;
    out->spi_host = (uint8_t)sd->spi_host;
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
    return ag_port_part_size((ag_port_part_t)dev->priv);
}

static int32_t flash_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    const ag_port_part_t part = (ag_port_part_t)dev->priv;
    const uint64_t       size = ag_port_part_size(part);

    if (off >= size) {
        return 0;
    }
    if (off + len > size) {
        len = (size_t)(size - off);
    }
    if (ag_port_part_read(part, off, buf, len) != AG_OK) {
        return -AG_EIO;
    }
    return (int32_t)len;
}

static ag_err_t flash_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                            size_t arglen)
{
    const ag_port_part_t part = (ag_port_part_t)dev->priv;

    if (cmd != AG_IOC_GEOMETRY) {
        return -AG_ENOTSUP;
    }
    if (arg == NULL || arglen < sizeof(ag_geometry_t)) {
        return -AG_EINVAL;
    }

    /* The erase block, not the read granularity: it is what a write costs. */
    const uint32_t erase = ag_port_part_erase_size(part);

    ag_geometry_t *geo = (ag_geometry_t *)arg;
    geo->sector_size = erase;
    geo->sectors = ag_port_part_size(part) / erase;
    return AG_OK;
}

static const ag_dev_ops_t k_flash_ops = {
    .read = flash_read,
    .ioctl = flash_ioctl,
    .size = flash_size,
};

static uint64_t sd_dev_size(ag_device_t *dev)
{
    const ag_port_sd_t card = (ag_port_sd_t)dev->priv;
    return ag_port_sd_sectors(card) * ag_port_sd_sector_size(card);
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
    const ag_port_sd_t card = (ag_port_sd_t)dev->priv;
    const uint64_t     ss = ag_port_sd_sector_size(card);
    const uint64_t     total = ag_port_sd_sectors(card) * ss;

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

        if (ag_port_sd_read_sectors(card, sector, s_sd_bounce, 1) != AG_OK) {
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
    const ag_port_sd_t card = (ag_port_sd_t)dev->priv;

    if (cmd != AG_IOC_GEOMETRY) {
        return -AG_ENOTSUP;
    }
    if (arg == NULL || arglen < sizeof(ag_geometry_t)) {
        return -AG_EINVAL;
    }

    ag_geometry_t *geo = (ag_geometry_t *)arg;
    geo->sector_size = ag_port_sd_sector_size(card);
    geo->sectors = (uint32_t)ag_port_sd_sectors(card);
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

    const ag_port_part_t part = ag_port_part_find(AG_SYS_PARTITION);
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
        s_sd_bounce = ag_port_alloc(ag_port_sd_sector_size(s_sd_card),
                                    AG_MEM_DMA);
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
        (void)ag_port_sd_unmount(AG_SD_BASE_PATH, s_sd_card);
        s_sd_card = NULL;
    }
}

/*
 * A card is somebody's data.  allow_format is true only when the operator asked
 * for it by name, never as a way to recover from a failed mount.  That is the
 * opposite of the choice made for internal flash, where a blank partition has
 * nothing on it to lose.
 */
static ag_err_t mount_media(bool allow_format)
{
    const ag_board_sd_t *sd = &ag_board()->sd;

    if (sd->kind == AG_SD_NONE) {
        return -AG_ENODEV;
    }

    ag_port_sd_cfg_t cfg;
    sd_cfg_from_board(sd, &cfg);

    const ag_err_t err =
        ag_port_sd_mount(&cfg, AG_SD_BASE_PATH, allow_format, &s_sd_card);
    if (err != AG_OK) {
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
        (void)ag_port_sd_unmount(AG_SD_BASE_PATH, s_sd_card);
        s_sd_card = NULL;
        return rc;
    }

    /* The card is here and readable, so it is also a device.  A failure to
     * register it costs the device node and nothing else, so the mount stands. */
    sd_register_device();
    return AG_OK;
}

ag_err_t ag_storage_mount_hostfs(void) { return ag_hostfs_try_mount(); }

ag_err_t ag_storage_mount_media(void)
{
    const ag_err_t sd = mount_media(false);
    /* Host helper optional: no answer → no H:, boot continues. */
    (void)ag_hostfs_try_mount();
    return sd;
}

ag_err_t ag_storage_format_media(void)
{
    unmount_media();
    return mount_media(true);
}

bool ag_storage_media_present(void) { return s_sd_card != NULL; }

ag_err_t ag_storage_init(void)
{
    if (s_vfs_mutex == NULL) {
        s_vfs_mutex = ag_port_mutex_new_recursive();
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
