/*
 * ArgonOS - VFS backend over an ESP-IDF filesystem.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "fs/idfvfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct ag_idfvfs {
    char               base[24];
    size_t             base_len;
    char               fs_name[8];
    ag_idfvfs_space_fn space;
    void              *space_ctx;
};

typedef struct {
    int fd;
} idf_file_t;

typedef struct {
    DIR *dir;
    /* Kept so each entry can be stat'ed: readdir does not report sizes. */
    char path[AG_PATH_MAX];
} idf_dir_t;

/* ---------------------------------------------------------------------- */

static ag_err_t from_errno(int e)
{
    switch (e) {
    case 0:       return AG_OK;
    case ENOENT:  return -AG_ENOENT;
    case EEXIST:  return -AG_EEXIST;
    case EACCES:  return -AG_EACCES;
    case EPERM:   return -AG_EPERM;
    case EISDIR:  return -AG_EISDIR;
    case ENOTDIR: return -AG_ENOTDIR;
    case ENOSPC:  return -AG_ENOSPC;
    case EROFS:   return -AG_EROFS;
    case EBADF:   return -AG_EBADF;
    case EINVAL:  return -AG_EINVAL;
    case ENFILE:
    case EMFILE:  return -AG_ENFILE;
    case ENOTEMPTY: return -AG_EBUSY;
    case ENOMEM:  return -AG_ENOMEM;
    default:      return -AG_EIO;
    }
}

/*
 * Joins the mount's base path with a path relative to it.  The mount root
 * arrives as "/", which must become the base path with nothing after it: FAT
 * under ESP-IDF will not open "/flash/".
 */
static ag_err_t full_path(const ag_idfvfs_t *fs, const char *rel, char *out,
                          size_t outlen)
{
    const bool root = (rel == NULL) || (rel[0] == '\0') ||
                      (rel[0] == '/' && rel[1] == '\0');

    const int n = root ? snprintf(out, outlen, "%s", fs->base)
                       : snprintf(out, outlen, "%s%s", fs->base, rel);

    if (n < 0 || (size_t)n >= outlen) {
        return -AG_ERANGE;
    }
    return AG_OK;
}

static int to_posix_flags(uint32_t flags)
{
    int out;

    if (flags & AG_O_RDWR) {
        out = O_RDWR;
    } else if (flags & AG_O_WRONLY) {
        out = O_WRONLY;
    } else if (flags & (AG_O_CREATE | AG_O_TRUNC | AG_O_APPEND)) {
        /* Asking to create or extend without saying how is a write. */
        out = O_WRONLY;
    } else {
        out = O_RDONLY;
    }

    if (flags & AG_O_CREATE) { out |= O_CREAT; }
    if (flags & AG_O_TRUNC)  { out |= O_TRUNC; }
    if (flags & AG_O_APPEND) { out |= O_APPEND; }
    if (flags & AG_O_EXCL)   { out |= O_EXCL; }
    return out;
}

static void fill_stat(const struct stat *st, ag_stat_t *out)
{
    memset(out, 0, sizeof(*out));
    out->size = (uint64_t)st->st_size;
    out->mtime = (uint64_t)st->st_mtime;
    out->attr = S_ISDIR(st->st_mode) ? AG_A_DIR : 0;
    if ((st->st_mode & S_IWUSR) == 0) {
        out->attr |= AG_A_READONLY;
    }
}

/* ---------------------------------------------------------------------- */

ag_err_t ag_idfvfs_create(const char *base_path, const char *fs_name,
                          ag_idfvfs_space_fn space, void *space_ctx,
                          ag_idfvfs_t **out)
{
    if (base_path == NULL || out == NULL) {
        return -AG_EINVAL;
    }
    if (strlen(base_path) >= sizeof(((ag_idfvfs_t *)0)->base)) {
        return -AG_ERANGE;
    }

    ag_idfvfs_t *fs = (ag_idfvfs_t *)calloc(1, sizeof(ag_idfvfs_t));
    if (fs == NULL) {
        return -AG_ENOMEM;
    }

    strcpy(fs->base, base_path);
    fs->base_len = strlen(base_path);
    snprintf(fs->fs_name, sizeof(fs->fs_name), "%s",
             (fs_name != NULL) ? fs_name : "idf");
    fs->space = space;
    fs->space_ctx = space_ctx;

    *out = fs;
    return AG_OK;
}

void ag_idfvfs_destroy(ag_idfvfs_t *fs) { free(fs); }

/* ---------------------------------------------------------------------- */

static ag_err_t idf_open(void *ctx, const char *rel, uint32_t flags, void **out)
{
    ag_idfvfs_t *fs = (ag_idfvfs_t *)ctx;
    char         path[AG_PATH_MAX];

    ag_err_t err = full_path(fs, rel, path, sizeof(path));
    if (err != AG_OK) {
        return err;
    }

    /*
     * FAT will happily open a directory, and then every read fails in a
     * confusing way.  Reporting it up front is what the RAM disk does too.
     */
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return -AG_EISDIR;
    }

    errno = 0;
    const int fd = open(path, to_posix_flags(flags), 0666);
    if (fd < 0) {
        return from_errno(errno);
    }

    idf_file_t *f = (idf_file_t *)malloc(sizeof(idf_file_t));
    if (f == NULL) {
        close(fd);
        return -AG_ENOMEM;
    }
    f->fd = fd;
    *out = f;
    return AG_OK;
}

static ag_err_t idf_close(void *ctx, void *file)
{
    (void)ctx;
    idf_file_t *f = (idf_file_t *)file;

    errno = 0;
    const int rc = close(f->fd);
    free(f);
    return (rc == 0) ? AG_OK : from_errno(errno);
}

static int32_t idf_read(void *ctx, void *file, void *buf, size_t len)
{
    (void)ctx;
    errno = 0;
    const ssize_t n = read(((idf_file_t *)file)->fd, buf, len);
    return (n < 0) ? from_errno(errno) : (int32_t)n;
}

static int32_t idf_write(void *ctx, void *file, const void *buf, size_t len)
{
    (void)ctx;
    errno = 0;
    const ssize_t n = write(((idf_file_t *)file)->fd, buf, len);
    return (n < 0) ? from_errno(errno) : (int32_t)n;
}

static int64_t idf_seek(void *ctx, void *file, int64_t off, int whence)
{
    (void)ctx;

    int posix_whence;
    switch (whence) {
    case AG_SEEK_SET: posix_whence = SEEK_SET; break;
    case AG_SEEK_CUR: posix_whence = SEEK_CUR; break;
    case AG_SEEK_END: posix_whence = SEEK_END; break;
    default: return -AG_EINVAL;
    }

    errno = 0;
    const off_t pos = lseek(((idf_file_t *)file)->fd, (off_t)off, posix_whence);
    return (pos < 0) ? from_errno(errno) : (int64_t)pos;
}

static ag_err_t idf_sync(void *ctx, void *file)
{
    (void)ctx;
    errno = 0;
    return (fsync(((idf_file_t *)file)->fd) == 0) ? AG_OK : from_errno(errno);
}

static ag_err_t idf_truncate(void *ctx, void *file, uint64_t len)
{
    (void)ctx;
    errno = 0;
    return (ftruncate(((idf_file_t *)file)->fd, (off_t)len) == 0)
               ? AG_OK
               : from_errno(errno);
}

static ag_err_t idf_stat(void *ctx, const char *rel, ag_stat_t *out)
{
    ag_idfvfs_t *fs = (ag_idfvfs_t *)ctx;
    char         path[AG_PATH_MAX];

    ag_err_t err = full_path(fs, rel, path, sizeof(path));
    if (err != AG_OK) {
        return err;
    }

    /*
     * The mount root has no directory entry of its own on FAT, so stat fails
     * on it.  It is nonetheless a directory, and the shell needs to be told so
     * before it will cd into the drive.
     */
    struct stat st;
    errno = 0;
    if (stat(path, &st) != 0) {
        if (strcmp(path, fs->base) == 0) {
            memset(out, 0, sizeof(*out));
            out->attr = AG_A_DIR;
            return AG_OK;
        }
        return from_errno(errno);
    }

    fill_stat(&st, out);
    return AG_OK;
}

static ag_err_t idf_unlink(void *ctx, const char *rel)
{
    char     path[AG_PATH_MAX];
    ag_err_t err = full_path((ag_idfvfs_t *)ctx, rel, path, sizeof(path));
    if (err != AG_OK) {
        return err;
    }

    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return -AG_EISDIR;
    }

    errno = 0;
    return (unlink(path) == 0) ? AG_OK : from_errno(errno);
}

static ag_err_t idf_rename(void *ctx, const char *from, const char *to)
{
    char from_path[AG_PATH_MAX];
    char to_path[AG_PATH_MAX];

    ag_err_t err = full_path((ag_idfvfs_t *)ctx, from, from_path,
                             sizeof(from_path));
    if (err != AG_OK) {
        return err;
    }
    err = full_path((ag_idfvfs_t *)ctx, to, to_path, sizeof(to_path));
    if (err != AG_OK) {
        return err;
    }

    /*
     * POSIX rename replaces the destination silently; the RAM disk refuses.
     * Matching the stricter behaviour keeps the two backends interchangeable,
     * which is the whole point of having a backend interface.
     */
    struct stat st;
    if (stat(to_path, &st) == 0) {
        return -AG_EEXIST;
    }

    errno = 0;
    return (rename(from_path, to_path) == 0) ? AG_OK : from_errno(errno);
}

static ag_err_t idf_mkdir(void *ctx, const char *rel)
{
    char     path[AG_PATH_MAX];
    ag_err_t err = full_path((ag_idfvfs_t *)ctx, rel, path, sizeof(path));
    if (err != AG_OK) {
        return err;
    }

    errno = 0;
    return (mkdir(path, 0777) == 0) ? AG_OK : from_errno(errno);
}

static ag_err_t idf_rmdir(void *ctx, const char *rel)
{
    char     path[AG_PATH_MAX];
    ag_err_t err = full_path((ag_idfvfs_t *)ctx, rel, path, sizeof(path));
    if (err != AG_OK) {
        return err;
    }

    struct stat st;
    if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
        return -AG_ENOTDIR;
    }

    errno = 0;
    return (rmdir(path) == 0) ? AG_OK : from_errno(errno);
}

static ag_err_t idf_opendir(void *ctx, const char *rel, void **out)
{
    ag_idfvfs_t *fs = (ag_idfvfs_t *)ctx;

    idf_dir_t *d = (idf_dir_t *)calloc(1, sizeof(idf_dir_t));
    if (d == NULL) {
        return -AG_ENOMEM;
    }

    ag_err_t err = full_path(fs, rel, d->path, sizeof(d->path));
    if (err != AG_OK) {
        free(d);
        return err;
    }

    errno = 0;
    d->dir = opendir(d->path);
    if (d->dir == NULL) {
        const ag_err_t e = from_errno(errno);
        free(d);
        return e;
    }

    *out = d;
    return AG_OK;
}

static ag_err_t idf_readdir(void *ctx, void *dir, ag_dirent_t *out)
{
    (void)ctx;
    idf_dir_t *d = (idf_dir_t *)dir;

    errno = 0;
    const struct dirent *ent = readdir(d->dir);
    if (ent == NULL) {
        return (errno == 0) ? -AG_ENOENT : from_errno(errno);
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", ent->d_name);
    out->st.attr = (ent->d_type == DT_DIR) ? AG_A_DIR : 0;

    /* readdir reports no size, and a directory listing without sizes is a
     * poor listing, so each entry is stat'ed. */
    char full[AG_PATH_MAX];
    if (snprintf(full, sizeof(full), "%s/%s", d->path, ent->d_name) <
        (int)sizeof(full)) {
        struct stat st;
        if (stat(full, &st) == 0) {
            fill_stat(&st, &out->st);
        }
    }
    return AG_OK;
}

static ag_err_t idf_closedir(void *ctx, void *dir)
{
    (void)ctx;
    idf_dir_t *d = (idf_dir_t *)dir;

    errno = 0;
    const int rc = closedir(d->dir);
    free(d);
    return (rc == 0) ? AG_OK : from_errno(errno);
}

static ag_err_t idf_info(void *ctx, ag_fsinfo_t *out)
{
    ag_idfvfs_t *fs = (ag_idfvfs_t *)ctx;

    memset(out, 0, sizeof(*out));
    snprintf(out->fs, sizeof(out->fs), "%s", fs->fs_name);

    if (fs->space != NULL) {
        uint64_t total = 0;
        uint64_t available = 0;
        if (fs->space(fs->space_ctx, &total, &available) == AG_OK) {
            out->total = total;
            out->free = available;
        }
    }
    return AG_OK;
}

static const ag_fs_ops_t k_idfvfs_ops = {
    .name = "idf",
    .open = idf_open,
    .close = idf_close,
    .read = idf_read,
    .write = idf_write,
    .seek = idf_seek,
    .sync = idf_sync,
    .truncate = idf_truncate,
    .stat = idf_stat,
    .unlink = idf_unlink,
    .rename = idf_rename,
    .mkdir = idf_mkdir,
    .rmdir = idf_rmdir,
    .opendir = idf_opendir,
    .readdir = idf_readdir,
    .closedir = idf_closedir,
    .info = idf_info,
};

const ag_fs_ops_t *ag_idfvfs_ops(void) { return &k_idfvfs_ops; }
