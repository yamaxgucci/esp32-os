/*
 * ArgonOS - VFS and RAM disk tests.
 *
 * The VFS core and the RAM disk are tested together: the core needs a real
 * backend to be worth testing, and the RAM disk is the one backend that runs
 * anywhere.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/ramfs.h>
#include <argon/vfs.h>

#include <stdlib.h>

#include "test.h"

static ag_ramfs_t *g_tmp;
static ag_ramfs_t *g_sys;

static uint64_t fake_clock(void) { return 1000000; }

static void setup(void)
{
    if (g_tmp != NULL) {
        ag_ramfs_destroy(g_tmp);
    }
    if (g_sys != NULL) {
        ag_ramfs_destroy(g_sys);
    }

    ag_vfs_init(NULL);
    const ag_ramfs_config_t tmp_cfg = {.budget = 64 * 1024,
                                       .now_unix = fake_clock};
    const ag_ramfs_config_t sys_cfg = {.budget = 16 * 1024,
                                       .now_unix = fake_clock};
    g_tmp = ag_ramfs_create(&tmp_cfg);
    g_sys = ag_ramfs_create(&sys_cfg);
    AG_CHECK(g_tmp != NULL && g_sys != NULL);
    AG_CHECK_INT(ag_vfs_mount("/tmp", ag_ramfs_ops(), g_tmp, 0), AG_OK);
    AG_CHECK_INT(ag_vfs_mount("/sys", ag_ramfs_ops(), g_sys, 0), AG_OK);
}

/* Writes a whole file; returns the handle result or the write result. */
static ag_err_t write_file(const char *path, const char *text)
{
    const ag_handle_t h = ag_vfs_open(path, NULL,
                                      AG_O_RDWR | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        return h;
    }
    const int32_t n = ag_vfs_write(h, text, strlen(text));
    ag_vfs_close(h);
    return (n < 0) ? n : AG_OK;
}

static const char *read_file(const char *path)
{
    static char buf[512];
    buf[0] = '\0';

    const ag_handle_t h = ag_vfs_open(path, NULL, AG_O_RDONLY);
    if (h < 0) {
        snprintf(buf, sizeof(buf), "<open err %d>", (int)h);
        return buf;
    }
    const int32_t n = ag_vfs_read(h, buf, sizeof(buf) - 1);
    ag_vfs_close(h);
    if (n < 0) {
        snprintf(buf, sizeof(buf), "<read err %d>", (int)n);
        return buf;
    }
    buf[n] = '\0';
    return buf;
}

/* ---------------------------------------------------------------------- */

static void test_mounting(void)
{
    setup();

    /* The same mount point twice is a mistake, not a stack. */
    AG_CHECK_INT(ag_vfs_mount("/tmp", ag_ramfs_ops(), g_tmp, 0), -AG_EEXIST);

    /* Mount points are canonicalised, so these are the same place. */
    const ag_ramfs_config_t extra_cfg = {.budget = 1024};
    ag_ramfs_t             *extra = ag_ramfs_create(&extra_cfg);
    AG_CHECK_INT(ag_vfs_mount("/tmp/", ag_ramfs_ops(), extra, 0), -AG_EEXIST);
    ag_ramfs_destroy(extra);

    ag_mountinfo_t mi;
    AG_CHECK_INT(ag_vfs_mount_info(0, &mi), AG_OK);
    AG_CHECK_STR(mi.mount, "/tmp");
    AG_CHECK_STR(mi.info.fs, "ram");
    AG_CHECK_INT(mi.info.total, 64 * 1024);
    AG_CHECK_INT(ag_vfs_mount_info(1, &mi), AG_OK);
    AG_CHECK_STR(mi.mount, "/sys");
    AG_CHECK_INT(ag_vfs_mount_info(2, &mi), -AG_ENOENT);

    AG_CHECK_INT(ag_vfs_unmount("/sys"), AG_OK);
    AG_CHECK_INT(ag_vfs_unmount("/sys"), -AG_ENOENT);
    AG_CHECK_INT(ag_vfs_mount("/sys", ag_ramfs_ops(), g_sys, 0), AG_OK);
}

static void test_unmounted_paths(void)
{
    setup();

    /* Nothing is mounted at /sd, so it is not a slow failure but a clear one. */
    AG_CHECK_INT(ag_vfs_open("/sd/x.txt", NULL, AG_O_RDONLY), -AG_ENOENT);
    AG_CHECK_INT(ag_vfs_mkdir("/sd/x", NULL), -AG_ENOENT);

    /* A path that merely starts with the same letters is not on the mount. */
    AG_CHECK_INT(ag_vfs_open("/tmpfile", NULL, AG_O_RDONLY), -AG_ENOENT);
}

static void test_read_write(void)
{
    setup();

    AG_CHECK_INT(write_file("/tmp/hello.txt", "Hello, ArgonOS"), AG_OK);
    AG_CHECK_STR(read_file("/tmp/hello.txt"), "Hello, ArgonOS");

    ag_stat_t st;
    AG_CHECK_INT(ag_vfs_stat("/tmp/hello.txt", NULL, &st), AG_OK);
    AG_CHECK_INT(st.size, 14);
    AG_CHECK_INT(st.attr, 0);
    AG_CHECK_INT(st.mtime, 1000000);

    /* Opening a missing file without CREATE fails. */
    AG_CHECK_INT(ag_vfs_open("/tmp/nope.txt", NULL, AG_O_RDONLY), -AG_ENOENT);

    /* TRUNC empties an existing file. */
    AG_CHECK_INT(write_file("/tmp/hello.txt", "short"), AG_OK);
    AG_CHECK_STR(read_file("/tmp/hello.txt"), "short");

    /* EXCL refuses to overwrite. */
    AG_CHECK_INT(ag_vfs_open("/tmp/hello.txt", NULL,
                             AG_O_CREATE | AG_O_EXCL | AG_O_WRONLY),
                 -AG_EEXIST);
}

static void test_seek_and_append(void)
{
    setup();
    AG_CHECK_INT(write_file("/tmp/f", "0123456789"), AG_OK);

    ag_handle_t h = ag_vfs_open("/tmp/f", NULL, AG_O_RDONLY);
    AG_CHECK(h >= 0);
    AG_CHECK_INT(ag_vfs_seek(h, 4, AG_SEEK_SET), 4);
    char buf[8] = {0};
    AG_CHECK_INT(ag_vfs_read(h, buf, 3), 3);
    AG_CHECK_STR(buf, "456");
    AG_CHECK_INT(ag_vfs_seek(h, -2, AG_SEEK_END), 8);
    AG_CHECK_INT(ag_vfs_read(h, buf, 8), 2);
    /* Reading at the end returns zero, not an error. */
    AG_CHECK_INT(ag_vfs_read(h, buf, 8), 0);
    /* Seeking before the start is refused. */
    AG_CHECK_INT(ag_vfs_seek(h, -1, AG_SEEK_SET), -AG_EINVAL);
    ag_vfs_close(h);

    h = ag_vfs_open("/tmp/f", NULL, AG_O_APPEND | AG_O_WRONLY);
    AG_CHECK(h >= 0);
    AG_CHECK_INT(ag_vfs_write(h, "AB", 2), 2);
    ag_vfs_close(h);
    AG_CHECK_STR(read_file("/tmp/f"), "0123456789AB");

    /* A write past the end leaves a hole, and a hole reads as zeros. */
    h = ag_vfs_open("/tmp/sparse", NULL, AG_O_RDWR | AG_O_CREATE);
    AG_CHECK(h >= 0);
    ag_vfs_seek(h, 4, AG_SEEK_SET);
    AG_CHECK_INT(ag_vfs_write(h, "X", 1), 1);
    ag_vfs_seek(h, 0, AG_SEEK_SET);
    char sparse[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    AG_CHECK_INT(ag_vfs_read(h, sparse, 8), 5);
    AG_CHECK_INT(sparse[0], 0);
    AG_CHECK_INT(sparse[3], 0);
    AG_CHECK_INT(sparse[4], 'X');
    ag_vfs_close(h);
}

static void test_read_only_open(void)
{
    setup();
    AG_CHECK_INT(write_file("/tmp/f", "data"), AG_OK);

    const ag_handle_t h = ag_vfs_open("/tmp/f", NULL, AG_O_RDONLY);
    AG_CHECK(h >= 0);
    AG_CHECK_INT(ag_vfs_write(h, "x", 1), -AG_EACCES);
    ag_vfs_close(h);
}

static void test_directories(void)
{
    setup();

    AG_CHECK_INT(ag_vfs_mkdir("/tmp/apps", NULL), AG_OK);
    AG_CHECK_INT(ag_vfs_mkdir("/tmp/apps", NULL), -AG_EEXIST);
    /* A directory cannot be created inside one that does not exist. */
    AG_CHECK_INT(ag_vfs_mkdir("/tmp/missing/deep", NULL), -AG_ENOENT);

    AG_CHECK_INT(write_file("/tmp/apps/a.axe", "aaa"), AG_OK);
    AG_CHECK_INT(write_file("/tmp/apps/b.axe", "bb"), AG_OK);

    ag_stat_t st;
    AG_CHECK_INT(ag_vfs_stat("/tmp/apps", NULL, &st), AG_OK);
    AG_CHECK_INT(st.attr, AG_A_DIR);

    /* A directory is not a file. */
    AG_CHECK_INT(ag_vfs_open("/tmp/apps", NULL, AG_O_RDONLY), -AG_EISDIR);
    AG_CHECK_INT(ag_vfs_unlink("/tmp/apps", NULL), -AG_EISDIR);
    /* And a non-empty directory is not removed by accident. */
    AG_CHECK_INT(ag_vfs_rmdir("/tmp/apps", NULL), -AG_EBUSY);

    const ag_handle_t d = ag_vfs_opendir("/tmp/apps", NULL);
    AG_CHECK(d >= 0);
    int         seen = 0;
    ag_dirent_t ent;
    while (ag_vfs_readdir(d, &ent) == AG_OK) {
        seen++;
        AG_CHECK(strcmp(ent.name, "a.axe") == 0 ||
                 strcmp(ent.name, "b.axe") == 0);
    }
    AG_CHECK_INT(seen, 2);
    AG_CHECK_INT(ag_vfs_closedir(d), AG_OK);

    AG_CHECK_INT(ag_vfs_unlink("/tmp/apps/a.axe", NULL), AG_OK);
    AG_CHECK_INT(ag_vfs_unlink("/tmp/apps/b.axe", NULL), AG_OK);
    AG_CHECK_INT(ag_vfs_rmdir("/tmp/apps", NULL), AG_OK);
    AG_CHECK_INT(ag_vfs_stat("/tmp/apps", NULL, &st), -AG_ENOENT);
}

static void test_root_lists_mounts(void)
{
    setup();

    /* The root of the namespace is where the drives live. */
    ag_stat_t st;
    AG_CHECK_INT(ag_vfs_stat("/", NULL, &st), AG_OK);
    AG_CHECK_INT(st.attr, AG_A_DIR);

    const ag_handle_t d = ag_vfs_opendir("/", NULL);
    AG_CHECK(d >= 0);

    bool        saw_tmp = false;
    bool        saw_sys = false;
    ag_dirent_t ent;
    while (ag_vfs_readdir(d, &ent) == AG_OK) {
        if (strcmp(ent.name, "tmp") == 0) { saw_tmp = true; }
        if (strcmp(ent.name, "sys") == 0) { saw_sys = true; }
        AG_CHECK_INT(ent.st.attr, AG_A_DIR);
    }
    AG_CHECK(saw_tmp);
    AG_CHECK(saw_sys);
    AG_CHECK_INT(ag_vfs_closedir(d), AG_OK);
}

static void test_relative_paths(void)
{
    setup();
    AG_CHECK_INT(ag_vfs_mkdir("/tmp/sub", NULL), AG_OK);
    AG_CHECK_INT(write_file("/tmp/sub/x", "here"), AG_OK);

    AG_CHECK_STR(read_file("/tmp/sub/x"), "here");

    const ag_handle_t h = ag_vfs_open("x", "/tmp/sub", AG_O_RDONLY);
    AG_CHECK(h >= 0);
    ag_vfs_close(h);

    ag_stat_t st;
    AG_CHECK_INT(ag_vfs_stat("../sub/x", "/tmp/sub", &st), AG_OK);
    /* DOS spelling reaches the same file. */
    AG_CHECK_INT(ag_vfs_stat("T:\\sub\\x", NULL, &st), AG_OK);
}

static void test_rename(void)
{
    setup();
    AG_CHECK_INT(write_file("/tmp/old.txt", "content"), AG_OK);
    AG_CHECK_INT(ag_vfs_mkdir("/tmp/dir", NULL), AG_OK);

    AG_CHECK_INT(ag_vfs_rename("/tmp/old.txt", "/tmp/new.txt", NULL), AG_OK);
    AG_CHECK_STR(read_file("/tmp/new.txt"), "content");
    ag_stat_t st;
    AG_CHECK_INT(ag_vfs_stat("/tmp/old.txt", NULL, &st), -AG_ENOENT);

    /* Into a subdirectory. */
    AG_CHECK_INT(ag_vfs_rename("/tmp/new.txt", "/tmp/dir/moved.txt", NULL),
                 AG_OK);
    AG_CHECK_STR(read_file("/tmp/dir/moved.txt"), "content");

    /* Over an existing name, and onto a missing directory. */
    AG_CHECK_INT(write_file("/tmp/a", "a"), AG_OK);
    AG_CHECK_INT(write_file("/tmp/b", "b"), AG_OK);
    AG_CHECK_INT(ag_vfs_rename("/tmp/a", "/tmp/b", NULL), -AG_EEXIST);
    AG_CHECK_INT(ag_vfs_rename("/tmp/a", "/tmp/gone/c", NULL), -AG_ENOENT);

    /*
     * Across filesystems this would be a copy plus a delete.  Doing that
     * silently would hide the cost, so it is refused.
     */
    AG_CHECK_INT(ag_vfs_rename("/tmp/a", "/sys/a", NULL), -AG_EPERM);

    /* A directory cannot be moved inside itself. */
    AG_CHECK_INT(ag_vfs_mkdir("/tmp/dir/inner", NULL), AG_OK);
    AG_CHECK_INT(ag_vfs_rename("/tmp/dir", "/tmp/dir/inner/self", NULL),
                 -AG_EINVAL);
}

static void test_read_only_mount(void)
{
    setup();
    AG_CHECK_INT(write_file("/sys/config", "x"), AG_OK);

    AG_CHECK_INT(ag_vfs_unmount("/sys"), AG_OK);
    AG_CHECK_INT(ag_vfs_mount("/sys", ag_ramfs_ops(), g_sys,
                              AG_MOUNT_READONLY),
                 AG_OK);

    AG_CHECK_STR(read_file("/sys/config"), "x");
    AG_CHECK_INT(ag_vfs_open("/sys/new", NULL, AG_O_CREATE | AG_O_WRONLY),
                 -AG_EROFS);
    AG_CHECK_INT(ag_vfs_unlink("/sys/config", NULL), -AG_EROFS);
    AG_CHECK_INT(ag_vfs_mkdir("/sys/d", NULL), -AG_EROFS);

    /* Even through a handle opened before the write was attempted. */
    const ag_handle_t h = ag_vfs_open("/sys/config", NULL, AG_O_RDONLY);
    AG_CHECK(h >= 0);
    AG_CHECK_INT(ag_vfs_write(h, "y", 1), -AG_EROFS);
    ag_vfs_close(h);
}

static void test_busy_and_eject(void)
{
    setup();
    AG_CHECK_INT(write_file("/tmp/f", "data"), AG_OK);

    const ag_handle_t h = ag_vfs_open("/tmp/f", NULL, AG_O_RDONLY);
    AG_CHECK(h >= 0);

    /* Unmounting with a file open is refused. */
    AG_CHECK_INT(ag_vfs_unmount("/tmp"), -AG_EBUSY);

    /*
     * Ejecting is what happens when the card is pulled: the mount goes away
     * whether we like it or not, and open handles must fail cleanly rather
     * than call into a backend that is gone.
     */
    AG_CHECK_INT(ag_vfs_eject("/tmp"), AG_OK);

    char buf[8];
    AG_CHECK_INT(ag_vfs_read(h, buf, sizeof(buf)), -AG_EIO);
    AG_CHECK_INT(ag_vfs_seek(h, 0, AG_SEEK_SET), -AG_EIO);
    AG_CHECK_INT(ag_vfs_open("/tmp/f", NULL, AG_O_RDONLY), -AG_EIO);

    /* Closing the last handle releases the slot. */
    AG_CHECK_INT(ag_vfs_close(h), AG_OK);
    AG_CHECK_INT(ag_vfs_open("/tmp/f", NULL, AG_O_RDONLY), -AG_ENOENT);

    ag_mountinfo_t mi;
    AG_CHECK_INT(ag_vfs_mount_info(0, &mi), AG_OK);
    AG_CHECK_STR(mi.mount, "/sys");
}

static ag_pid_t g_owner = 0;
static ag_pid_t owner_fn(void) { return g_owner; }

static void test_handle_ownership(void)
{
    setup();
    ag_vfs_set_owner_fn(owner_fn);

    g_owner = 0;
    AG_CHECK_INT(write_file("/tmp/kernel.txt", "k"), AG_OK);
    const ag_handle_t kernel_handle =
        ag_vfs_open("/tmp/kernel.txt", NULL, AG_O_RDONLY);
    AG_CHECK(kernel_handle >= 0);

    g_owner = 7;
    const ag_handle_t a = ag_vfs_open("/tmp/kernel.txt", NULL, AG_O_RDONLY);
    const ag_handle_t b = ag_vfs_opendir("/tmp", NULL);
    AG_CHECK(a >= 0 && b >= 0);
    AG_CHECK_INT(ag_vfs_open_count(), 3);

    /* Killing a process closes what it opened, and nothing else. */
    AG_CHECK_INT(ag_vfs_close_owned_by(7), 2);
    AG_CHECK_INT(ag_vfs_open_count(), 1);
    char buf[4];
    AG_CHECK_INT(ag_vfs_read(a, buf, 1), -AG_EBADF);
    AG_CHECK_INT(ag_vfs_read(kernel_handle, buf, 1), 1);

    /* And the mount is no longer busy on their account. */
    ag_vfs_close(kernel_handle);
    AG_CHECK_INT(ag_vfs_unmount("/tmp"), AG_OK);

    ag_vfs_set_owner_fn(NULL);
}

static void test_handle_exhaustion(void)
{
    setup();
    AG_CHECK_INT(write_file("/tmp/f", "x"), AG_OK);

    ag_handle_t handles[AG_VFS_MAX_HANDLES + 2];
    int         opened = 0;
    for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); i++) {
        handles[i] = ag_vfs_open("/tmp/f", NULL, AG_O_RDONLY);
        if (handles[i] >= 0) {
            opened++;
        } else {
            AG_CHECK_INT(handles[i], -AG_ENFILE);
        }
    }
    AG_CHECK_INT(opened, AG_VFS_MAX_HANDLES);

    for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); i++) {
        if (handles[i] >= 0) {
            ag_vfs_close(handles[i]);
        }
    }
    AG_CHECK_INT(ag_vfs_open_count(), 0);

    /* Running out of handles must not have leaked the backend's files. */
    AG_CHECK_INT(ag_vfs_unmount("/tmp"), AG_OK);
}

static void test_bad_handles(void)
{
    setup();

    char buf[4];
    AG_CHECK_INT(ag_vfs_read(-1, buf, 1), -AG_EBADF);
    AG_CHECK_INT(ag_vfs_read(999, buf, 1), -AG_EBADF);
    AG_CHECK_INT(ag_vfs_close(0), -AG_EBADF);
    AG_CHECK_INT(ag_vfs_readdir(0, NULL), -AG_EINVAL);

    /* A directory handle is not a file handle and the other way round. */
    AG_CHECK_INT(write_file("/tmp/f", "x"), AG_OK);
    const ag_handle_t file = ag_vfs_open("/tmp/f", NULL, AG_O_RDONLY);
    const ag_handle_t dir = ag_vfs_opendir("/tmp", NULL);
    AG_CHECK(file >= 0 && dir >= 0);

    ag_dirent_t ent;
    AG_CHECK_INT(ag_vfs_readdir(file, &ent), -AG_EBADF);
    AG_CHECK_INT(ag_vfs_read(dir, buf, 1), -AG_EBADF);
    AG_CHECK_INT(ag_vfs_close(dir), -AG_EBADF);
    AG_CHECK_INT(ag_vfs_closedir(file), -AG_EBADF);

    ag_vfs_close(file);
    ag_vfs_closedir(dir);
}

static void test_budget(void)
{
    setup();

    /* A RAM disk with no limit is a way to run the board out of memory. */
    static char chunk[1024];
    memset(chunk, 'x', sizeof(chunk));

    const ag_handle_t h = ag_vfs_open("/sys/big", NULL,
                                      AG_O_RDWR | AG_O_CREATE);
    AG_CHECK(h >= 0);

    int32_t total = 0;
    for (int i = 0; i < 64; i++) {
        const int32_t n = ag_vfs_write(h, chunk, sizeof(chunk));
        if (n < 0) {
            AG_CHECK_INT(n, -AG_ENOSPC);
            break;
        }
        total += n;
    }
    ag_vfs_close(h);

    /* It stopped, and it stopped somewhere below the budget. */
    AG_CHECK(total > 0);
    AG_CHECK(total <= 16 * 1024);
    AG_CHECK(ag_ramfs_used(g_sys) <= 16 * 1024);

    /* And the filesystem is still usable afterwards. */
    ag_stat_t st;
    AG_CHECK_INT(ag_vfs_stat("/sys/big", NULL, &st), AG_OK);
    AG_CHECK_INT(ag_vfs_unlink("/sys/big", NULL), AG_OK);
}

static void test_case_insensitive(void)
{
    setup();
    AG_CHECK_INT(write_file("/tmp/Hello.TXT", "hi"), AG_OK);

    /* FAT does not care about case, so neither do we. */
    AG_CHECK_STR(read_file("/tmp/hello.txt"), "hi");
    AG_CHECK_STR(read_file("/tmp/HELLO.TXT"), "hi");

    /* But the name keeps the spelling it was created with. */
    const ag_handle_t d = ag_vfs_opendir("/tmp", NULL);
    ag_dirent_t       ent;
    AG_CHECK_INT(ag_vfs_readdir(d, &ent), AG_OK);
    AG_CHECK_STR(ent.name, "Hello.TXT");
    ag_vfs_closedir(d);

    /* mkdir must not invent a second spelling of the same name. */
    AG_CHECK_INT(ag_vfs_mkdir("/tmp/Hello.TXT", NULL), -AG_EEXIST);
    AG_CHECK_INT(ag_vfs_mkdir("/tmp/hello.txt", NULL), -AG_EEXIST);

    AG_CHECK_INT(ag_vfs_mkdir("/tmp/Sega", NULL), AG_OK);
    AG_CHECK_INT(write_file("/tmp/Sega/rom.sms", "x"), AG_OK);
    AG_CHECK_INT(ag_vfs_mkdir("/tmp/sega", NULL), -AG_EEXIST);

    char real[AG_PATH_MAX];
    AG_CHECK_INT(ag_vfs_realpath("/tmp/sega", NULL, real, sizeof(real)), AG_OK);
    AG_CHECK_STR(real, "/tmp/Sega");
    AG_CHECK_INT(ag_vfs_realpath("/tmp/SEGA/rom.sms", NULL, real, sizeof(real)),
                 AG_OK);
    AG_CHECK_STR(real, "/tmp/Sega/rom.sms");
}

void run_vfs_tests(void)
{
    test_mounting();
    test_unmounted_paths();
    test_read_write();
    test_seek_and_append();
    test_read_only_open();
    test_directories();
    test_root_lists_mounts();
    test_relative_paths();
    test_rename();
    test_read_only_mount();
    test_busy_and_eject();
    test_handle_ownership();
    test_handle_exhaustion();
    test_bad_handles();
    test_budget();
    test_case_insensitive();

    /* Leave nothing mounted for whatever runs next. */
    ag_vfs_init(NULL);
    ag_ramfs_destroy(g_tmp);
    ag_ramfs_destroy(g_sys);
    g_tmp = NULL;
    g_sys = NULL;
}
