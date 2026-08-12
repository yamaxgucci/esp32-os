/*
 * ArgonOS - the device registry as a filesystem.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/devfs.h>

#include <string.h>

/*
 * The position lives here rather than in the device, because two holders of the
 * same disk read from two different places and a position in the device would
 * make them one.  A stream device ignores it; the registry passes it through.
 */
typedef struct {
    bool         used;
    ag_device_t *dev;
    uint64_t     pos;
    void        *session; /* from open_session; NULL for classic devices */
} devfs_file_t;

typedef struct {
    bool     used;
    uint32_t index;
} devfs_dir_t;

static devfs_file_t s_files[AG_DEVFS_MAX_OPEN];
static devfs_dir_t  s_dirs[AG_DEVFS_MAX_DIRS];

/* ---------------------------------------------------------------------- */

/*
 * /dev is flat.  A device name cannot contain a separator - the registry
 * refuses one - so anything with a second component is not a device, and saying
 * so beats searching for a name that cannot exist.
 */
static ag_err_t device_name(const char *rel, const char **name_out)
{
    if (rel == NULL || rel[0] != '/') {
        return -AG_EINVAL;
    }
    const char *name = rel + 1;
    if (name[0] == '\0') {
        return -AG_EISDIR;
    }
    if (strchr(name, '/') != NULL) {
        return -AG_ENOTDIR;
    }
    *name_out = name;
    return AG_OK;
}

static void fill_stat(const ag_device_t *dev, ag_stat_t *out)
{
    memset(out, 0, sizeof(*out));
    out->size = ag_dev_size((ag_device_t *)dev);
    out->attr = AG_A_SYSTEM;
    if (dev->flags & AG_DEVF_READONLY) {
        out->attr |= AG_A_READONLY;
    }
}

/* ---------------------------------------------------------------------- */
/* Files                                                                  */
/* ---------------------------------------------------------------------- */

static ag_err_t devfs_open(void *ctx, const char *rel, uint32_t flags,
                           void **file)
{
    (void)ctx;

    const char *name = NULL;
    ag_err_t    err = device_name(rel, &name);
    if (err != AG_OK) {
        return err;
    }

    ag_device_t *dev = ag_dev_find(name);
    if (dev == NULL) {
        /*
         * AG_O_CREATE is not a way to make a device.  Devices come from
         * drivers, and a caller that asked to create one has a typo, not a
         * plan - answering -AG_ENOENT says so.
         */
        return -AG_ENOENT;
    }

    devfs_file_t *slot = NULL;
    for (int i = 0; i < AG_DEVFS_MAX_OPEN; i++) {
        if (!s_files[i].used) {
            slot = &s_files[i];
            break;
        }
    }
    if (slot == NULL) {
        return -AG_ENFILE;
    }

    {
        void *session = NULL;
        err = ag_dev_open2(dev, flags, &session);
        if (err != AG_OK) {
            return err;
        }

        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->dev = dev;
        slot->session = session;
        *file = slot;
        return AG_OK;
    }
}

static ag_err_t devfs_close(void *ctx, void *file)
{
    (void)ctx;
    devfs_file_t *f = (devfs_file_t *)file;
    if (f == NULL || !f->used) {
        return -AG_EBADF;
    }

    const ag_err_t err = ag_dev_close2(f->dev, f->session);
    memset(f, 0, sizeof(*f));
    return err;
}

static int32_t devfs_read(void *ctx, void *file, void *buf, size_t len)
{
    (void)ctx;
    devfs_file_t *f = (devfs_file_t *)file;
    if (f == NULL || !f->used) {
        return -AG_EBADF;
    }

    const int32_t n = ag_dev_read2(f->dev, f->session, buf, len, f->pos);
    if (n > 0) {
        f->pos += (uint64_t)n;
    }
    return n;
}

static int32_t devfs_write(void *ctx, void *file, const void *buf, size_t len)
{
    (void)ctx;
    devfs_file_t *f = (devfs_file_t *)file;
    if (f == NULL || !f->used) {
        return -AG_EBADF;
    }

    const int32_t n = ag_dev_write2(f->dev, f->session, buf, len, f->pos);
    if (n > 0) {
        f->pos += (uint64_t)n;
    }
    return n;
}

static int64_t devfs_seek(void *ctx, void *file, int64_t off, int whence)
{
    (void)ctx;
    devfs_file_t *f = (devfs_file_t *)file;
    if (f == NULL || !f->used) {
        return -AG_EBADF;
    }

    int64_t base;
    switch (whence) {
    case AG_SEEK_SET: base = 0; break;
    case AG_SEEK_CUR: base = (int64_t)f->pos; break;
    /*
     * The end of a stream is where it is now: a console has no length, and
     * pretending it has one would put the position somewhere that means
     * nothing.  For a disk this is the capacity, which is what it should be.
     */
    case AG_SEEK_END: base = (int64_t)ag_dev_size(f->dev); break;
    default: return -AG_EINVAL;
    }

    const int64_t target = base + off;
    if (target < 0) {
        return -AG_EINVAL;
    }
    f->pos = (uint64_t)target;
    return target;
}

static ag_err_t devfs_sync(void *ctx, void *file)
{
    (void)ctx;
    devfs_file_t *f = (devfs_file_t *)file;
    if (f == NULL || !f->used) {
        return -AG_EBADF;
    }
    /* A device that has nothing to flush says so by not implementing it. */
    const ag_err_t err =
        ag_dev_ioctl2(f->dev, f->session, AG_IOC_FLUSH, NULL, 0);
    return (err == -AG_ENOTSUP) ? AG_OK : err;
}

/* ---------------------------------------------------------------------- */
/* Metadata                                                               */
/* ---------------------------------------------------------------------- */

static ag_err_t devfs_stat(void *ctx, const char *rel, ag_stat_t *out)
{
    (void)ctx;

    const char *name = NULL;
    ag_err_t    err = device_name(rel, &name);
    if (err == -AG_EISDIR) {
        memset(out, 0, sizeof(*out));
        out->attr = AG_A_DIR;
        return AG_OK;
    }
    if (err != AG_OK) {
        return err;
    }

    const ag_device_t *dev = ag_dev_find(name);
    if (dev == NULL) {
        return -AG_ENOENT;
    }
    fill_stat(dev, out);
    return AG_OK;
}

/* ---------------------------------------------------------------------- */
/* Directory                                                              */
/* ---------------------------------------------------------------------- */

static ag_err_t devfs_opendir(void *ctx, const char *rel, void **dir)
{
    (void)ctx;

    const char *name = NULL;
    const ag_err_t err = device_name(rel, &name);
    if (err != -AG_EISDIR) {
        /* Only the root of /dev is a directory; a device is not one. */
        return (err == AG_OK) ? -AG_ENOTDIR : err;
    }

    for (int i = 0; i < AG_DEVFS_MAX_DIRS; i++) {
        if (!s_dirs[i].used) {
            s_dirs[i].used = true;
            s_dirs[i].index = 0;
            *dir = &s_dirs[i];
            return AG_OK;
        }
    }
    return -AG_ENFILE;
}

static ag_err_t devfs_readdir(void *ctx, void *dir, ag_dirent_t *out)
{
    (void)ctx;
    devfs_dir_t *d = (devfs_dir_t *)dir;
    if (d == NULL || !d->used) {
        return -AG_EBADF;
    }

    ag_devinfo_t info;
    const ag_err_t err = ag_dev_info(d->index, AG_DEV_ANY, &info);
    if (err != AG_OK) {
        return err;
    }
    d->index++;

    memset(out, 0, sizeof(*out));
    strncpy(out->name, info.name, sizeof(out->name) - 1);

    const ag_device_t *dev = ag_dev_find(info.name);
    if (dev != NULL) {
        fill_stat(dev, &out->st);
    } else {
        out->st.attr = AG_A_SYSTEM;
    }
    return AG_OK;
}

static ag_err_t devfs_closedir(void *ctx, void *dir)
{
    (void)ctx;
    devfs_dir_t *d = (devfs_dir_t *)dir;
    if (d == NULL || !d->used) {
        return -AG_EBADF;
    }
    d->used = false;
    return AG_OK;
}

static ag_err_t devfs_info(void *ctx, ag_fsinfo_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    strncpy(out->fs, "dev", sizeof(out->fs) - 1);
    /*
     * No total and no free, and that is not a gap: /dev holds no bytes.  A
     * number here would be a number about nothing.
     */
    return AG_OK;
}

/* ---------------------------------------------------------------------- */

/*
 * unlink, rename, mkdir, rmdir and truncate are absent rather than refusing.
 * The VFS answers -AG_ENOTSUP for a NULL entry, which is the true answer: a
 * device node is not a file that was put there, so there is nothing to remove.
 */
static const ag_fs_ops_t k_devfs_ops = {
    .name = "dev",
    .open = devfs_open,
    .close = devfs_close,
    .read = devfs_read,
    .write = devfs_write,
    .seek = devfs_seek,
    .sync = devfs_sync,
    .stat = devfs_stat,
    .opendir = devfs_opendir,
    .readdir = devfs_readdir,
    .closedir = devfs_closedir,
    .info = devfs_info,
};

const ag_fs_ops_t *ag_devfs_ops(void) { return &k_devfs_ops; }

void ag_devfs_reset(void)
{
    memset(s_files, 0, sizeof(s_files));
    memset(s_dirs, 0, sizeof(s_dirs));
}

ag_device_t *ag_devfs_device_of(ag_handle_t h)
{
    devfs_file_t *f = (devfs_file_t *)ag_vfs_backend_object(h, &k_devfs_ops);
    return (f != NULL && f->used) ? f->dev : NULL;
}

void *ag_devfs_session_of(ag_handle_t h)
{
    devfs_file_t *f = (devfs_file_t *)ag_vfs_backend_object(h, &k_devfs_ops);
    return (f != NULL && f->used) ? f->session : NULL;
}
