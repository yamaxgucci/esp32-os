/*
 * ArgonOS - the real filesystem behind fsops.
 *
 * The whole of the machine-facing half: fifteen calls that add a context
 * argument and forward.  It exists so that fsops.c contains no argon.h, and
 * therefore runs on the host over a tree held in memory - the walk is where
 * the bugs live, and this is where they cannot.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "fsops.h"

#include <argon/argon.h>

static ag_handle_t fs_open(void *ctx, const char *path, uint32_t flags)
{
    (void)ctx;
    return ag_open(path, flags);
}

static int32_t fs_read(void *ctx, ag_handle_t h, void *buf, uint32_t len)
{
    (void)ctx;
    return ag_read(h, buf, len);
}

static int32_t fs_write(void *ctx, ag_handle_t h, const void *buf, uint32_t len)
{
    (void)ctx;
    return ag_write(h, buf, len);
}

static ag_err_t fs_sync(void *ctx, ag_handle_t h)
{
    (void)ctx;
    return ag_sync(h);
}

static ag_err_t fs_close(void *ctx, ag_handle_t h)
{
    (void)ctx;
    return ag_close(h);
}

static ag_err_t fs_stat(void *ctx, const char *path, ag_stat_t *out)
{
    (void)ctx;
    return ag_stat(path, out);
}

static ag_handle_t fs_opendir(void *ctx, const char *path)
{
    (void)ctx;
    return ag_opendir(path);
}

static ag_err_t fs_readdir(void *ctx, ag_handle_t h, ag_dirent_t *out)
{
    (void)ctx;
    return ag_readdir(h, out);
}

static ag_err_t fs_closedir(void *ctx, ag_handle_t h)
{
    (void)ctx;
    return ag_closedir(h);
}

static ag_err_t fs_mkdir(void *ctx, const char *path)
{
    (void)ctx;
    return ag_mkdir(path);
}

static ag_err_t fs_unlink(void *ctx, const char *path)
{
    (void)ctx;
    return ag_unlink(path);
}

static ag_err_t fs_rmdir(void *ctx, const char *path)
{
    (void)ctx;
    return ag_rmdir(path);
}

static ag_err_t fs_rename(void *ctx, const char *from, const char *to)
{
    (void)ctx;
    return ag_rename(from, to);
}

static const fsops_fs_t k_argon = {
    .open = fs_open,
    .read = fs_read,
    .write = fs_write,
    .sync = fs_sync,
    .close = fs_close,
    .stat = fs_stat,
    .opendir = fs_opendir,
    .readdir = fs_readdir,
    .closedir = fs_closedir,
    .mkdir = fs_mkdir,
    .unlink = fs_unlink,
    .rmdir = fs_rmdir,
    .rename = fs_rename,
};

const fsops_fs_t *fsops_argon_fs(void) { return &k_argon; }
