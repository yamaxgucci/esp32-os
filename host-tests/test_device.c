/*
 * ArgonOS - device registry and devfs tests.
 *
 * The registry and the filesystem in front of it are tested together, because
 * the thing worth proving is that they are one mechanism: a handle on a device
 * has to be a file handle, with the ownership and the reclaim that come with
 * it, or the whole reason for putting devices behind /dev is gone.
 *
 * The devices here are fakes, and deliberately so.  What is being tested is the
 * model - naming, exclusivity, position, what happens when the hardware is
 * pulled - and a fake answers those questions without a board.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/devfs.h>
#include <argon/device.h>
#include <argon/vfs.h>

#include <string.h>

#include "test.h"

/* ---------------------------------------------------------------------- */
/* Fakes                                                                  */
/* ---------------------------------------------------------------------- */

/* A stream: reads the same letter forever, counts what is written to it. */
typedef struct {
    char     letter;
    uint32_t written;
    uint32_t opens;
    uint32_t closes;
} fake_stream_t;

static ag_err_t stream_open(ag_device_t *dev, uint32_t flags)
{
    (void)flags;
    ((fake_stream_t *)dev->priv)->opens++;
    return AG_OK;
}

static ag_err_t stream_close(ag_device_t *dev)
{
    ((fake_stream_t *)dev->priv)->closes++;
    return AG_OK;
}

static int32_t stream_read(ag_device_t *dev, void *buf, size_t len,
                           uint64_t off)
{
    (void)off;
    memset(buf, ((fake_stream_t *)dev->priv)->letter, len);
    return (int32_t)len;
}

static int32_t stream_write(ag_device_t *dev, const void *buf, size_t len,
                            uint64_t off)
{
    (void)buf;
    (void)off;
    ((fake_stream_t *)dev->priv)->written += (uint32_t)len;
    return (int32_t)len;
}

static const ag_dev_ops_t k_stream_ops = {
    .open = stream_open,
    .close = stream_close,
    .read = stream_read,
    .write = stream_write,
};

/* A disk: 256 bytes of storage that answers by position. */
typedef struct {
    uint8_t bytes[256];
} fake_disk_t;

static uint64_t disk_size(ag_device_t *dev)
{
    (void)dev;
    return sizeof(((fake_disk_t *)0)->bytes);
}

static int32_t disk_read(ag_device_t *dev, void *buf, size_t len, uint64_t off)
{
    fake_disk_t *d = (fake_disk_t *)dev->priv;
    if (off >= sizeof(d->bytes)) {
        return 0;
    }
    if (off + len > sizeof(d->bytes)) {
        len = (size_t)(sizeof(d->bytes) - off);
    }
    memcpy(buf, d->bytes + off, len);
    return (int32_t)len;
}

static int32_t disk_write(ag_device_t *dev, const void *buf, size_t len,
                          uint64_t off)
{
    fake_disk_t *d = (fake_disk_t *)dev->priv;
    if (off >= sizeof(d->bytes)) {
        return -AG_ENOSPC;
    }
    if (off + len > sizeof(d->bytes)) {
        len = (size_t)(sizeof(d->bytes) - off);
    }
    memcpy(d->bytes + off, buf, len);
    return (int32_t)len;
}

static ag_err_t disk_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                           size_t arglen)
{
    (void)dev;
    if (cmd != AG_IOC_GEOMETRY) {
        return -AG_ENOTSUP;
    }
    if (arg == NULL || arglen < sizeof(ag_geometry_t)) {
        return -AG_EINVAL;
    }
    ag_geometry_t *geo = (ag_geometry_t *)arg;
    geo->sector_size = 64;
    geo->sectors = 4;
    return AG_OK;
}

static const ag_dev_ops_t k_disk_ops = {
    .read = disk_read,
    .write = disk_write,
    .ioctl = disk_ioctl,
    .size = disk_size,
};

/* A device that supplies nothing at all: every call has to be refused. */
static const ag_dev_ops_t k_mute_ops = {0};

/* The class vtable a driver would hand out; only its identity matters here. */
static const int k_class_vtable = 42;

static fake_stream_t g_stream;
static fake_disk_t   g_disk;

/* ---------------------------------------------------------------------- */

static ag_device_t *add_stream(const char *name, uint32_t flags)
{
    const ag_dev_desc_t desc = {
        .name = name,
        .driver = "fake",
        .cls = AG_DEV_CHAR,
        .flags = flags,
        .ops = &k_stream_ops,
        .priv = &g_stream,
    };
    ag_device_t *dev = NULL;
    AG_CHECK_INT(ag_dev_register(&desc, &dev), AG_OK);
    return dev;
}

static ag_device_t *add_disk(const char *name, uint32_t flags,
                             const void *owner)
{
    const ag_dev_desc_t desc = {
        .name = name,
        .driver = "fakedisk",
        .cls = AG_DEV_STORAGE,
        .flags = flags,
        .ops = &k_disk_ops,
        .class_ops = &k_class_vtable,
        .priv = &g_disk,
        .owner = owner,
    };
    ag_device_t *dev = NULL;
    AG_CHECK_INT(ag_dev_register(&desc, &dev), AG_OK);
    return dev;
}

static void setup(void)
{
    ag_vfs_init(NULL);
    ag_dev_init(NULL);
    ag_devfs_reset();
    memset(&g_stream, 0, sizeof(g_stream));
    memset(&g_disk, 0, sizeof(g_disk));
    g_stream.letter = 'A';
    for (size_t i = 0; i < sizeof(g_disk.bytes); i++) {
        g_disk.bytes[i] = (uint8_t)i;
    }
    AG_CHECK_INT(ag_vfs_mount("/dev", ag_devfs_ops(), NULL, 0), AG_OK);
}

/* ---------------------------------------------------------------------- */

static void test_registration(void)
{
    setup();

    AG_CHECK_INT(ag_dev_count(), 0);
    ag_device_t *tty = add_stream("tty", 0);
    ag_device_t *disk = add_disk("disk0", 0, NULL);
    AG_CHECK_INT(ag_dev_count(), 2);

    AG_CHECK(ag_dev_find("tty") == tty);
    /* Names are matched the way paths are, so D:\DISK0 finds it. */
    AG_CHECK(ag_dev_find("DISK0") == disk);
    AG_CHECK(ag_dev_find("nothing") == NULL);

    /* A driver that unregisters what it registered gets the slot back. */
    AG_CHECK_INT(ag_dev_unregister(tty), AG_OK);
    AG_CHECK_INT(ag_dev_count(), 1);
    AG_CHECK(ag_dev_find("tty") == NULL);
}

static void test_names_are_checked(void)
{
    setup();
    add_stream("tty", 0);

    ag_dev_desc_t desc = {
        .name = "tty",
        .cls = AG_DEV_CHAR,
        .ops = &k_stream_ops,
    };
    AG_CHECK_INT(ag_dev_register(&desc, NULL), -AG_EEXIST);
    /* Case does not make it a different device, or D:\TTY would open twice. */
    desc.name = "TTY";
    AG_CHECK_INT(ag_dev_register(&desc, NULL), -AG_EEXIST);

    /* A name that would not survive being a path component is refused here,
     * rather than becoming a device nobody can open. */
    desc.name = "sub/dev";
    AG_CHECK_INT(ag_dev_register(&desc, NULL), -AG_EINVAL);
    desc.name = "a\\b";
    AG_CHECK_INT(ag_dev_register(&desc, NULL), -AG_EINVAL);
    desc.name = "c:";
    AG_CHECK_INT(ag_dev_register(&desc, NULL), -AG_EINVAL);
    desc.name = "two words";
    AG_CHECK_INT(ag_dev_register(&desc, NULL), -AG_EINVAL);
    desc.name = "star*";
    AG_CHECK_INT(ag_dev_register(&desc, NULL), -AG_EINVAL);
    desc.name = "";
    AG_CHECK_INT(ag_dev_register(&desc, NULL), -AG_EINVAL);
    desc.name = "0123456789012345678901234"; /* 25, one over the field */
    AG_CHECK_INT(ag_dev_register(&desc, NULL), -AG_EINVAL);

    /* And a device has to have a class and operations. */
    desc.name = "ok";
    desc.ops = NULL;
    AG_CHECK_INT(ag_dev_register(&desc, NULL), -AG_EINVAL);
    desc.ops = &k_stream_ops;
    desc.cls = AG_DEV_ANY;
    AG_CHECK_INT(ag_dev_register(&desc, NULL), -AG_EINVAL);

    AG_CHECK_INT(ag_dev_count(), 1);
}

static void test_table_fills_up(void)
{
    setup();

    char name[8];
    for (int i = 0; i < AG_DEV_MAX; i++) {
        snprintf(name, sizeof(name), "d%d", i);
        const ag_dev_desc_t desc = {
            .name = name,
            .cls = AG_DEV_CHAR,
            .ops = &k_stream_ops,
            .priv = &g_stream,
        };
        AG_CHECK_INT(ag_dev_register(&desc, NULL), AG_OK);
    }
    AG_CHECK_INT(ag_dev_count(), AG_DEV_MAX);

    const ag_dev_desc_t one_too_many = {
        .name = "extra",
        .cls = AG_DEV_CHAR,
        .ops = &k_stream_ops,
    };
    AG_CHECK_INT(ag_dev_register(&one_too_many, NULL), -AG_ENFILE);
}

static void test_enumerate_and_filter(void)
{
    setup();
    add_stream("tty", 0);
    add_disk("disk0", AG_DEVF_READONLY, NULL);

    ag_devinfo_t info;
    AG_CHECK_INT(ag_dev_info(0, AG_DEV_ANY, &info), AG_OK);
    AG_CHECK_STR(info.name, "tty");
    AG_CHECK_STR(info.driver, "fake");
    AG_CHECK_INT(info.cls, AG_DEV_CHAR);
    AG_CHECK_INT(ag_dev_info(1, AG_DEV_ANY, &info), AG_OK);
    AG_CHECK_STR(info.name, "disk0");
    AG_CHECK_INT(ag_dev_info(2, AG_DEV_ANY, &info), -AG_ENOENT);

    /* Filtering renumbers, so an application asking for storage sees only it. */
    AG_CHECK_INT(ag_dev_info(0, AG_DEV_STORAGE, &info), AG_OK);
    AG_CHECK_STR(info.name, "disk0");
    AG_CHECK(info.flags & AG_DEVF_READONLY);
    AG_CHECK_INT(ag_dev_info(1, AG_DEV_STORAGE, &info), -AG_ENOENT);
    AG_CHECK_INT(ag_dev_info(0, AG_DEV_DISPLAY, &info), -AG_ENOENT);
}

static void test_exclusive_open(void)
{
    setup();
    ag_device_t *bus = add_stream("bus0", AG_DEVF_EXCLUSIVE);

    AG_CHECK_INT(ag_dev_open(bus, AG_O_RDWR), AG_OK);
    AG_CHECK_INT(ag_dev_open(bus, AG_O_RDWR), -AG_EBUSY);
    AG_CHECK_INT(g_stream.opens, 1); /* the refusal never reached the driver */

    /* And it comes back when the holder is done with it. */
    AG_CHECK_INT(ag_dev_close(bus), AG_OK);
    AG_CHECK_INT(g_stream.closes, 1);
    AG_CHECK_INT(ag_dev_open(bus, AG_O_RDWR), AG_OK);
    AG_CHECK_INT(ag_dev_close(bus), AG_OK);

    /* A shared device does not mind company. */
    ag_device_t *tty = add_stream("tty", 0);
    AG_CHECK_INT(ag_dev_open(tty, AG_O_RDONLY), AG_OK);
    AG_CHECK_INT(ag_dev_open(tty, AG_O_RDONLY), AG_OK);
    AG_CHECK_INT(ag_dev_close(tty), AG_OK);
    AG_CHECK_INT(ag_dev_close(tty), AG_OK);
    AG_CHECK_INT(ag_dev_close(tty), -AG_EBADF); /* one close too many */
}

static void test_read_only_device(void)
{
    setup();
    ag_device_t *rom = add_disk("rom0", AG_DEVF_READONLY, NULL);

    AG_CHECK_INT(ag_dev_open(rom, AG_O_RDWR), -AG_EROFS);
    AG_CHECK_INT(ag_dev_open(rom, AG_O_RDONLY), AG_OK);
    AG_CHECK_INT(ag_dev_write(rom, "x", 1, 0), -AG_EROFS);
    AG_CHECK_INT(ag_dev_close(rom), AG_OK);

    /* Through the filesystem it is the same refusal, not a different one. */
    AG_CHECK_INT(ag_vfs_open("/dev/rom0", NULL, AG_O_RDWR), -AG_EROFS);
    const ag_handle_t h = ag_vfs_open("/dev/rom0", NULL, AG_O_RDONLY);
    AG_CHECK(h >= 0);
    AG_CHECK_INT(ag_vfs_write(h, "x", 1), -AG_EROFS);
    AG_CHECK_INT(ag_vfs_close(h), AG_OK);
}

static void test_missing_operations(void)
{
    setup();
    const ag_dev_desc_t desc = {
        .name = "mute",
        .cls = AG_DEV_SENSOR,
        .ops = &k_mute_ops,
    };
    ag_device_t *mute = NULL;
    AG_CHECK_INT(ag_dev_register(&desc, &mute), AG_OK);

    char buf[4];
    AG_CHECK_INT(ag_dev_open(mute, AG_O_RDWR), AG_OK);
    AG_CHECK_INT(ag_dev_read(mute, buf, sizeof(buf), 0), -AG_ENOTSUP);
    AG_CHECK_INT(ag_dev_write(mute, buf, sizeof(buf), 0), -AG_ENOTSUP);
    AG_CHECK_INT(ag_dev_ioctl(mute, AG_IOC_RESET, NULL, 0), -AG_ENOTSUP);
    AG_CHECK_INT(ag_dev_size(mute), 0);
    AG_CHECK_INT(ag_dev_close(mute), AG_OK);
}

static void test_ioctl(void)
{
    setup();
    ag_device_t *disk = add_disk("disk0", 0, NULL);

    /* Every device answers AG_IOC_INFO, whether or not it has an ioctl. */
    ag_devinfo_t info;
    AG_CHECK_INT(ag_dev_ioctl(disk, AG_IOC_INFO, &info, sizeof(info)), AG_OK);
    AG_CHECK_STR(info.name, "disk0");
    AG_CHECK_INT(info.cls, AG_DEV_STORAGE);
    AG_CHECK_INT(ag_dev_ioctl(disk, AG_IOC_INFO, &info, 4), -AG_EINVAL);

    ag_geometry_t geo = {0};
    AG_CHECK_INT(ag_dev_ioctl(disk, AG_IOC_GEOMETRY, &geo, sizeof(geo)), AG_OK);
    AG_CHECK_INT(geo.sector_size, 64);
    AG_CHECK_INT(geo.sectors, 4);

    /* A command from another class is not silently something else here. */
    AG_CHECK_INT(ag_dev_ioctl(disk, AG_IOC(AG_DEV_DISPLAY, 1), &geo,
                              sizeof(geo)),
                 -AG_ENOTSUP);
}

/* ---------------------------------------------------------------------- */
/* Through the filesystem                                                 */
/* ---------------------------------------------------------------------- */

static void test_devfs_listing(void)
{
    setup();
    add_stream("tty", 0);
    add_disk("disk0", AG_DEVF_READONLY, NULL);

    const ag_handle_t d = ag_vfs_opendir("/dev", NULL);
    AG_CHECK(d >= 0);

    ag_dirent_t ent;
    AG_CHECK_INT(ag_vfs_readdir(d, &ent), AG_OK);
    AG_CHECK_STR(ent.name, "tty");
    AG_CHECK(ent.st.attr & AG_A_SYSTEM);
    AG_CHECK_INT(ent.st.size, 0); /* a stream has no length */

    AG_CHECK_INT(ag_vfs_readdir(d, &ent), AG_OK);
    AG_CHECK_STR(ent.name, "disk0");
    AG_CHECK_INT(ent.st.size, 256);
    AG_CHECK(ent.st.attr & AG_A_READONLY);

    AG_CHECK_INT(ag_vfs_readdir(d, &ent), -AG_ENOENT);
    AG_CHECK_INT(ag_vfs_closedir(d), AG_OK);

    /* And the drive letter is the same door. */
    const ag_handle_t viadrive = ag_vfs_opendir("D:\\", NULL);
    AG_CHECK(viadrive >= 0);
    AG_CHECK_INT(ag_vfs_readdir(viadrive, &ent), AG_OK);
    AG_CHECK_STR(ent.name, "tty");
    AG_CHECK_INT(ag_vfs_closedir(viadrive), AG_OK);
}

static void test_devfs_stat(void)
{
    setup();
    add_disk("disk0", 0, NULL);

    ag_stat_t st;
    AG_CHECK_INT(ag_vfs_stat("/dev", NULL, &st), AG_OK);
    AG_CHECK(st.attr & AG_A_DIR);

    AG_CHECK_INT(ag_vfs_stat("d:\\disk0", NULL, &st), AG_OK);
    AG_CHECK_INT(st.size, 256);
    AG_CHECK(!(st.attr & AG_A_DIR));

    AG_CHECK_INT(ag_vfs_stat("/dev/nothing", NULL, &st), -AG_ENOENT);
    /* /dev is flat: there is no such thing as a device inside a device. */
    AG_CHECK_INT(ag_vfs_stat("/dev/disk0/part1", NULL, &st), -AG_ENOTDIR);
}

static void test_devfs_position(void)
{
    setup();
    add_disk("disk0", 0, NULL);

    const ag_handle_t h = ag_vfs_open("d:\\disk0", NULL, AG_O_RDWR);
    AG_CHECK(h >= 0);

    /* The position belongs to the handle: reading advances it. */
    uint8_t buf[8];
    AG_CHECK_INT(ag_vfs_read(h, buf, 4), 4);
    AG_CHECK_INT(buf[0], 0);
    AG_CHECK_INT(buf[3], 3);
    AG_CHECK_INT(ag_vfs_read(h, buf, 4), 4);
    AG_CHECK_INT(buf[0], 4);

    AG_CHECK_INT(ag_vfs_seek(h, 100, AG_SEEK_SET), 100);
    AG_CHECK_INT(ag_vfs_read(h, buf, 2), 2);
    AG_CHECK_INT(buf[0], 100);
    AG_CHECK_INT(ag_vfs_seek(h, -2, AG_SEEK_CUR), 100);
    AG_CHECK_INT(ag_vfs_seek(h, 0, AG_SEEK_END), 256);
    /* At the end there is nothing left, which is end of file and not an error. */
    AG_CHECK_INT(ag_vfs_read(h, buf, 4), 0);
    AG_CHECK_INT(ag_vfs_seek(h, -1, AG_SEEK_SET), -AG_EINVAL);

    /* Writing lands where the position says, not at the start. */
    AG_CHECK_INT(ag_vfs_seek(h, 16, AG_SEEK_SET), 16);
    AG_CHECK_INT(ag_vfs_write(h, "hello", 5), 5);
    AG_CHECK_INT(g_disk.bytes[16], 'h');
    AG_CHECK_INT(g_disk.bytes[20], 'o');
    AG_CHECK_INT(g_disk.bytes[21], 21); /* untouched */

    /* Two handles on one device do not share a position. */
    const ag_handle_t other = ag_vfs_open("d:\\disk0", NULL, AG_O_RDONLY);
    AG_CHECK(other >= 0);
    AG_CHECK_INT(ag_vfs_read(other, buf, 1), 1);
    AG_CHECK_INT(buf[0], 0);

    AG_CHECK_INT(ag_vfs_close(h), AG_OK);
    AG_CHECK_INT(ag_vfs_close(other), AG_OK);
}

static void test_devfs_refusals(void)
{
    setup();
    add_stream("tty", 0);

    /* Creating a device is not something a caller does by opening a name. */
    AG_CHECK_INT(ag_vfs_open("/dev/newthing", NULL, AG_O_RDWR | AG_O_CREATE),
                 -AG_ENOENT);
    /* Nor is removing one, because nothing put it there. */
    AG_CHECK_INT(ag_vfs_unlink("/dev/tty", NULL), -AG_ENOTSUP);
    AG_CHECK_INT(ag_vfs_mkdir("/dev/sub", NULL), -AG_ENOTSUP);
    AG_CHECK_INT(ag_vfs_rename("/dev/tty", "/dev/tty2", NULL), -AG_ENOTSUP);
    /* A device is not a directory. */
    AG_CHECK_INT(ag_vfs_opendir("/dev/tty", NULL), -AG_ENOTDIR);
}

static void test_devfs_pool(void)
{
    setup();
    add_stream("tty", 0);

    ag_handle_t held[AG_DEVFS_MAX_OPEN];
    for (int i = 0; i < AG_DEVFS_MAX_OPEN; i++) {
        held[i] = ag_vfs_open("/dev/tty", NULL, AG_O_RDONLY);
        AG_CHECK(held[i] >= 0);
    }
    AG_CHECK_INT(ag_vfs_open("/dev/tty", NULL, AG_O_RDONLY), -AG_ENFILE);
    /* The refusal must not have counted as an open on the device itself. */
    AG_CHECK_INT(g_stream.opens, AG_DEVFS_MAX_OPEN);

    for (int i = 0; i < AG_DEVFS_MAX_OPEN; i++) {
        AG_CHECK_INT(ag_vfs_close(held[i]), AG_OK);
    }
    AG_CHECK_INT(g_stream.closes, AG_DEVFS_MAX_OPEN);
    AG_CHECK(ag_vfs_open("/dev/tty", NULL, AG_O_RDONLY) >= 0);
}

static void test_handle_carries_the_device(void)
{
    setup();
    ag_device_t *disk = add_disk("disk0", 0, NULL);

    const ag_handle_t h = ag_vfs_open("/dev/disk0", NULL, AG_O_RDWR);
    AG_CHECK(h >= 0);
    AG_CHECK(ag_devfs_device_of(h) == disk);
    /* Which is how ioctl and the class vtable are reached from a handle. */
    AG_CHECK(ag_devfs_device_of(h)->class_ops == &k_class_vtable);
    AG_CHECK_INT(ag_vfs_close(h), AG_OK);

    /* A closed handle, and a handle that is not a device, carry nothing. */
    AG_CHECK(ag_devfs_device_of(h) == NULL);
    AG_CHECK(ag_devfs_device_of(-1) == NULL);
}

static ag_err_t fake_disp_info(ag_handle_t h, ag_gfxinfo_t *out)
{
    (void)h;
    if (out == NULL) {
        return -AG_EINVAL;
    }
    out->width = 320;
    out->height = 240;
    out->fmt = AG_PIX_RGB565;
    out->stride = 640;
    out->fb = NULL;
    out->double_buf = false;
    out->direct = true;
    return AG_OK;
}

static const ag_display_ops_t k_fake_display_ops = {
    .size = sizeof(ag_display_ops_t),
    .info = fake_disp_info,
    .acquire = NULL,
    .release = NULL,
    .flush = NULL,
    .swap = NULL,
};

static void test_display_class_ops(void)
{
    setup();
    const ag_dev_desc_t desc = {
        .name = "fb0",
        .driver = "soft",
        .cls = AG_DEV_DISPLAY,
        .flags = 0,
        .ops = &k_stream_ops,
        .class_ops = &k_fake_display_ops,
        .priv = &g_stream,
    };
    ag_device_t *dev = NULL;
    AG_CHECK_INT(ag_dev_register(&desc, &dev), AG_OK);

    const ag_handle_t h = ag_vfs_open("/dev/fb0", NULL, AG_O_RDWR);
    AG_CHECK(h >= 0);
    const ag_display_ops_t *ops =
        (const ag_display_ops_t *)ag_devfs_device_of(h)->class_ops;
    AG_CHECK(ops == &k_fake_display_ops);
    AG_CHECK(ops->size == sizeof(ag_display_ops_t));
    AG_CHECK(ops->info != NULL);

    ag_gfxinfo_t info;
    memset(&info, 0, sizeof(info));
    AG_CHECK_INT(ops->info(h, &info), AG_OK);
    AG_CHECK_INT(info.width, 320);
    AG_CHECK_INT(info.height, 240);
    AG_CHECK(info.fb == NULL);
    AG_CHECK_INT(ag_vfs_close(h), AG_OK);
}

/* ---------------------------------------------------------------------- */
/* Going away                                                             */
/* ---------------------------------------------------------------------- */

static void test_unregister_is_refused_while_open(void)
{
    setup();
    ag_device_t *tty = add_stream("tty", 0);

    const ag_handle_t h = ag_vfs_open("/dev/tty", NULL, AG_O_RDWR);
    AG_CHECK(h >= 0);
    /* A driver unloading itself has to be told no, not obeyed. */
    AG_CHECK_INT(ag_dev_unregister(tty), -AG_EBUSY);
    AG_CHECK_INT(ag_vfs_close(h), AG_OK);
    AG_CHECK_INT(ag_dev_unregister(tty), AG_OK);
}

static void test_revoke_with_handles_open(void)
{
    setup();
    ag_device_t *card = add_disk("sd0", AG_DEVF_HOTPLUG, NULL);

    const ag_handle_t h = ag_vfs_open("/dev/sd0", NULL, AG_O_RDWR);
    AG_CHECK(h >= 0);

    uint8_t buf[4];
    AG_CHECK_INT(ag_vfs_read(h, buf, 4), 4);

    /* The card is pulled while somebody is reading it. */
    AG_CHECK_INT(ag_dev_revoke(card), 1);

    /* It is gone from the registry at once... */
    AG_CHECK(ag_dev_find("sd0") == NULL);
    AG_CHECK_INT(ag_dev_count(), 0);
    AG_CHECK_INT(ag_vfs_open("/dev/sd0", NULL, AG_O_RDONLY), -AG_ENOENT);

    /* ...and the holder learns about it on its next call, rather than
     * reaching a driver whose hardware is no longer there. */
    AG_CHECK_INT(ag_vfs_read(h, buf, 4), -AG_ENODEV);
    AG_CHECK_INT(ag_vfs_write(h, buf, 4), -AG_ENODEV);
    AG_CHECK_INT(ag_vfs_close(h), AG_OK);

    /* The name is free again only once the last handle has gone, so that a new
     * card cannot be reached through a handle on the old one. */
    AG_CHECK(add_disk("sd0", AG_DEVF_HOTPLUG, NULL) != NULL);
}

static void test_revoke_keeps_the_name_until_the_last_close(void)
{
    setup();
    ag_device_t *card = add_disk("sd0", AG_DEVF_HOTPLUG, NULL);

    const ag_handle_t h = ag_vfs_open("/dev/sd0", NULL, AG_O_RDONLY);
    AG_CHECK(h >= 0);
    AG_CHECK_INT(ag_dev_revoke(card), 1);

    const ag_dev_desc_t same_name = {
        .name = "sd0",
        .cls = AG_DEV_STORAGE,
        .ops = &k_disk_ops,
        .priv = &g_disk,
    };
    AG_CHECK_INT(ag_dev_register(&same_name, NULL), -AG_EEXIST);

    AG_CHECK_INT(ag_vfs_close(h), AG_OK);
    AG_CHECK_INT(ag_dev_register(&same_name, NULL), AG_OK);
}

static void test_revoke_by_owner(void)
{
    setup();
    static const int module_a = 0;
    static const int module_b = 0;

    add_disk("a0", 0, &module_a);
    add_disk("a1", 0, &module_a);
    add_disk("b0", 0, &module_b);
    add_stream("tty", 0); /* the kernel's own, owner NULL */
    AG_CHECK_INT(ag_dev_count(), 4);

    /* Unloading a module takes its devices with it, and only its own. */
    AG_CHECK_INT(ag_dev_revoke_owner(&module_a), 2);
    AG_CHECK_INT(ag_dev_count(), 2);
    AG_CHECK(ag_dev_find("a0") == NULL);
    AG_CHECK(ag_dev_find("b0") != NULL);
    AG_CHECK(ag_dev_find("tty") != NULL);
}

static void test_process_reclaim(void)
{
    setup();
    ag_device_t *tty = add_stream("tty", 0);

    /* A device opened by a process is closed when the process ends, because it
     * is a file handle and the VFS already does that. */
    AG_CHECK(ag_vfs_open("/dev/tty", NULL, AG_O_RDWR) >= 0);
    AG_CHECK(ag_vfs_open("/dev/tty", NULL, AG_O_RDONLY) >= 0);
    AG_CHECK_INT(tty->open_count, 2);

    AG_CHECK_INT(ag_vfs_close_owned_by(0), 2);
    AG_CHECK_INT(tty->open_count, 0);
    AG_CHECK_INT(g_stream.closes, 2);
    /* And with nobody holding it, the driver can now be unloaded. */
    AG_CHECK_INT(ag_dev_unregister(tty), AG_OK);
}

void run_device_tests(void)
{
    test_registration();
    test_names_are_checked();
    test_table_fills_up();
    test_enumerate_and_filter();
    test_exclusive_open();
    test_read_only_device();
    test_missing_operations();
    test_ioctl();

    test_devfs_listing();
    test_devfs_stat();
    test_devfs_position();
    test_devfs_refusals();
    test_devfs_pool();
    test_handle_carries_the_device();
    test_display_class_ops();

    test_unregister_is_refused_while_open();
    test_revoke_with_handles_open();
    test_revoke_keeps_the_name_until_the_last_close();
    test_revoke_by_owner();
    test_process_reclaim();

    /* Leave nothing mounted or registered for whatever runs next. */
    ag_vfs_init(NULL);
    ag_dev_init(NULL);
    ag_devfs_reset();
}
