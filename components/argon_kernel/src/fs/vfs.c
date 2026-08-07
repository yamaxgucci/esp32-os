/*
 * ArgonOS - virtual filesystem core.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/vfs.h>

#include <string.h>

typedef struct {
    char               point[32];
    size_t             point_len;
    const ag_fs_ops_t *ops;
    void              *ctx;
    uint32_t           flags;
    uint32_t           open_handles;
    bool               used;
    /*
     * The media went away while files were still open.  The slot stays
     * reserved so those handles keep resolving to something, and every
     * operation on them fails cleanly instead of calling into a backend that
     * no longer exists.
     */
    bool ejected;
} ag_mount_t;

typedef struct {
    bool     used;
    bool     is_dir;
    bool     is_root; /* the synthetic directory that lists the mounts */
    uint8_t  mount;
    void    *obj;
    ag_pid_t owner;
    uint32_t root_index;
} ag_vfs_handle_slot_t;

static ag_mount_t           s_mounts[AG_VFS_MAX_MOUNTS];
static ag_vfs_handle_slot_t s_handles[AG_VFS_MAX_HANDLES];
static ag_vfs_lock_t        s_lock;
static ag_pid_t (*s_owner_fn)(void);
static bool s_initialised;

/* ---------------------------------------------------------------------- */

static void lock(void)
{
    if (s_lock.lock != NULL) {
        s_lock.lock(s_lock.ctx);
    }
}

static void unlock(void)
{
    if (s_lock.unlock != NULL) {
        s_lock.unlock(s_lock.ctx);
    }
}

static ag_pid_t current_owner(void)
{
    return (s_owner_fn != NULL) ? s_owner_fn() : 0;
}

ag_err_t ag_vfs_init(const ag_vfs_lock_t *lock_iface)
{
    memset(s_mounts, 0, sizeof(s_mounts));
    memset(s_handles, 0, sizeof(s_handles));
    memset(&s_lock, 0, sizeof(s_lock));
    s_owner_fn = NULL;

    if (lock_iface != NULL) {
        s_lock = *lock_iface;
    }
    s_initialised = true;
    return AG_OK;
}

void ag_vfs_set_owner_fn(ag_pid_t (*fn)(void)) { s_owner_fn = fn; }

/* ---------------------------------------------------------------------- */
/* Mount table                                                            */
/* ---------------------------------------------------------------------- */

/*
 * Longest prefix wins, so mounting /sd/apps over /sd does the obvious thing.
 * Returns -1 when nothing matches, which for "/" means the synthetic root.
 */
static int find_mount(const char *path, const char **rel)
{
    int    best = -1;
    size_t best_len = 0;

    for (int i = 0; i < AG_VFS_MAX_MOUNTS; i++) {
        const ag_mount_t *m = &s_mounts[i];
        if (!m->used) {
            continue;
        }
        if (strncmp(path, m->point, m->point_len) != 0) {
            continue;
        }
        /* "/sdcard" must not match a mount at "/sd". */
        const char after = path[m->point_len];
        if (after != '\0' && after != '/') {
            continue;
        }
        if (m->point_len >= best_len) {
            best = i;
            best_len = m->point_len;
        }
    }

    if (best >= 0 && rel != NULL) {
        const char *tail = path + best_len;
        *rel = (*tail == '\0') ? "/" : tail;
    }
    return best;
}

ag_err_t ag_vfs_mount(const char *mountpoint, const ag_fs_ops_t *ops,
                      void *ctx, uint32_t flags)
{
    if (!s_initialised || mountpoint == NULL || ops == NULL) {
        return -AG_EINVAL;
    }

    char canonical[AG_PATH_MAX];
    ag_err_t err = ag_path_resolve(mountpoint, NULL, canonical,
                                   sizeof(canonical));
    if (err != AG_OK) {
        return err;
    }
    if (strlen(canonical) >= sizeof(s_mounts[0].point)) {
        return -AG_ERANGE;
    }

    lock();

    for (int i = 0; i < AG_VFS_MAX_MOUNTS; i++) {
        if (s_mounts[i].used && strcmp(s_mounts[i].point, canonical) == 0) {
            unlock();
            return -AG_EEXIST;
        }
    }

    for (int i = 0; i < AG_VFS_MAX_MOUNTS; i++) {
        ag_mount_t *m = &s_mounts[i];
        if (m->used) {
            continue;
        }
        memset(m, 0, sizeof(*m));
        strcpy(m->point, canonical);
        /* The root mount is "/" but matching must not count its slash. */
        m->point_len = (strcmp(canonical, "/") == 0) ? 0 : strlen(canonical);
        m->ops = ops;
        m->ctx = ctx;
        m->flags = flags;
        m->used = true;
        unlock();
        return AG_OK;
    }

    unlock();
    return -AG_ENFILE;
}

ag_err_t ag_vfs_unmount(const char *mountpoint)
{
    char canonical[AG_PATH_MAX];
    ag_err_t err = ag_path_resolve(mountpoint, NULL, canonical,
                                   sizeof(canonical));
    if (err != AG_OK) {
        return err;
    }

    lock();
    for (int i = 0; i < AG_VFS_MAX_MOUNTS; i++) {
        ag_mount_t *m = &s_mounts[i];
        if (!m->used || strcmp(m->point, canonical) != 0) {
            continue;
        }
        if (m->open_handles > 0) {
            unlock();
            return -AG_EBUSY;
        }
        memset(m, 0, sizeof(*m));
        unlock();
        return AG_OK;
    }
    unlock();
    return -AG_ENOENT;
}

ag_err_t ag_vfs_eject(const char *mountpoint)
{
    char canonical[AG_PATH_MAX];
    ag_err_t err = ag_path_resolve(mountpoint, NULL, canonical,
                                   sizeof(canonical));
    if (err != AG_OK) {
        return err;
    }

    lock();
    for (int i = 0; i < AG_VFS_MAX_MOUNTS; i++) {
        ag_mount_t *m = &s_mounts[i];
        if (!m->used || strcmp(m->point, canonical) != 0) {
            continue;
        }
        if (m->open_handles == 0) {
            memset(m, 0, sizeof(*m));
        } else {
            m->ejected = true;
            m->ops = NULL;
            m->ctx = NULL;
        }
        unlock();
        return AG_OK;
    }
    unlock();
    return -AG_ENOENT;
}

ag_err_t ag_vfs_mount_info(uint32_t index, ag_mountinfo_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }

    lock();
    uint32_t seen = 0;
    for (int i = 0; i < AG_VFS_MAX_MOUNTS; i++) {
        const ag_mount_t *m = &s_mounts[i];
        if (!m->used) {
            continue;
        }
        if (seen++ != index) {
            continue;
        }

        memset(out, 0, sizeof(*out));
        strncpy(out->mount, m->point, sizeof(out->mount) - 1);
        out->flags = m->flags;
        out->open_handles = m->open_handles;

        /*
         * The backend's own name comes first as a default, then info() may
         * replace it: an adapter serves several filesystems and only the
         * instance knows which one this is.
         */
        if (m->ops != NULL && m->ops->name != NULL) {
            strncpy(out->info.fs, m->ops->name, sizeof(out->info.fs) - 1);
        }
        if (!m->ejected && m->ops != NULL && m->ops->info != NULL) {
            m->ops->info(m->ctx, &out->info);
        }
        out->info.removable = (m->flags & AG_MOUNT_REMOVABLE) != 0;
        out->info.read_only = (m->flags & AG_MOUNT_READONLY) != 0;
        unlock();
        return AG_OK;
    }
    unlock();
    return -AG_ENOENT;
}

/* ---------------------------------------------------------------------- */
/* Handles                                                                */
/* ---------------------------------------------------------------------- */

static ag_vfs_handle_slot_t *slot_of(ag_handle_t h)
{
    if (h < 0 || h >= AG_VFS_MAX_HANDLES || !s_handles[h].used) {
        return NULL;
    }
    return &s_handles[h];
}

static ag_handle_t alloc_slot(void)
{
    for (int i = 0; i < AG_VFS_MAX_HANDLES; i++) {
        if (!s_handles[i].used) {
            memset(&s_handles[i], 0, sizeof(s_handles[i]));
            s_handles[i].used = true;
            s_handles[i].owner = current_owner();
            return (ag_handle_t)i;
        }
    }
    return -AG_ENFILE;
}

static void free_slot(ag_handle_t h)
{
    ag_vfs_handle_slot_t *s = &s_handles[h];
    if (!s->is_root && s->mount < AG_VFS_MAX_MOUNTS) {
        ag_mount_t *m = &s_mounts[s->mount];
        if (m->used && m->open_handles > 0) {
            m->open_handles--;
            /* A mount ejected while busy disappears with its last handle. */
            if (m->ejected && m->open_handles == 0) {
                memset(m, 0, sizeof(*m));
            }
        }
    }
    memset(s, 0, sizeof(*s));
}

uint32_t ag_vfs_open_count(void)
{
    uint32_t n = 0;
    lock();
    for (int i = 0; i < AG_VFS_MAX_HANDLES; i++) {
        if (s_handles[i].used) {
            n++;
        }
    }
    unlock();
    return n;
}

void *ag_vfs_backend_object(ag_handle_t h, const ag_fs_ops_t *ops)
{
    void *obj = NULL;

    lock();
    const ag_vfs_handle_slot_t *s = slot_of(h);
    if (s != NULL && !s->is_root && !s->is_dir) {
        const ag_mount_t *m = &s_mounts[s->mount];
        if (m->used && !m->ejected && m->ops == ops) {
            obj = s->obj;
        }
    }
    unlock();
    return obj;
}

uint32_t ag_vfs_close_owned_by(ag_pid_t pid)
{
    uint32_t closed = 0;

    lock();
    for (int i = 0; i < AG_VFS_MAX_HANDLES; i++) {
        ag_vfs_handle_slot_t *s = &s_handles[i];
        if (!s->used || s->owner != pid) {
            continue;
        }
        if (!s->is_root) {
            const ag_mount_t *m = &s_mounts[s->mount];
            if (!m->ejected && m->ops != NULL) {
                if (s->is_dir && m->ops->closedir != NULL) {
                    m->ops->closedir(m->ctx, s->obj);
                } else if (!s->is_dir && m->ops->close != NULL) {
                    m->ops->close(m->ctx, s->obj);
                }
            }
        }
        free_slot((ag_handle_t)i);
        closed++;
    }
    unlock();
    return closed;
}

/* ---------------------------------------------------------------------- */
/* Path plumbing                                                          */
/* ---------------------------------------------------------------------- */

typedef struct {
    ag_mount_t *mount;
    int         index;
    const char *rel;
    char        canonical[AG_PATH_MAX];
} resolved_t;

static ag_err_t resolve(const char *path, const char *cwd, resolved_t *out)
{
    const ag_err_t err = ag_path_resolve(path, cwd, out->canonical,
                                         sizeof(out->canonical));
    if (err != AG_OK) {
        return err;
    }

    out->index = find_mount(out->canonical, &out->rel);
    if (out->index < 0) {
        out->mount = NULL;
        return -AG_ENOENT;
    }
    out->mount = &s_mounts[out->index];
    if (out->mount->ejected) {
        return -AG_EIO;
    }
    return AG_OK;
}

static bool is_write_flag(uint32_t flags)
{
    return (flags & (AG_O_WRONLY | AG_O_RDWR | AG_O_CREATE | AG_O_TRUNC |
                     AG_O_APPEND)) != 0;
}

/* ---------------------------------------------------------------------- */
/* Files                                                                  */
/* ---------------------------------------------------------------------- */

ag_handle_t ag_vfs_open(const char *path, const char *cwd, uint32_t flags)
{
    resolved_t r;

    lock();
    ag_err_t err = resolve(path, cwd, &r);
    if (err != AG_OK) {
        unlock();
        return err;
    }
    if (is_write_flag(flags) && (r.mount->flags & AG_MOUNT_READONLY)) {
        unlock();
        return -AG_EROFS;
    }
    if (r.mount->ops->open == NULL) {
        unlock();
        return -AG_ENOTSUP;
    }

    void *file = NULL;
    err = r.mount->ops->open(r.mount->ctx, r.rel, flags, &file);
    if (err != AG_OK) {
        unlock();
        return err;
    }

    const ag_handle_t h = alloc_slot();
    if (h < 0) {
        if (r.mount->ops->close != NULL) {
            r.mount->ops->close(r.mount->ctx, file);
        }
        unlock();
        return h;
    }

    s_handles[h].mount = (uint8_t)r.index;
    s_handles[h].obj = file;
    s_handles[h].is_dir = false;
    r.mount->open_handles++;

    unlock();
    return h;
}

/*
 * Every operation on an open handle needs the same checks, and getting one of
 * them wrong is how a stale or ejected mount turns into a crash.  Caller holds
 * the lock.
 */
typedef struct {
    ag_vfs_handle_slot_t *slot;
    ag_mount_t           *mount;
} open_ref_t;

static ag_err_t handle_ref(ag_handle_t h, bool want_dir, open_ref_t *out)
{
    ag_vfs_handle_slot_t *s = slot_of(h);
    if (s == NULL || s->is_dir != want_dir) {
        return -AG_EBADF;
    }
    if (s->is_root) {
        return -AG_EISDIR;
    }

    ag_mount_t *m = &s_mounts[s->mount];
    if (m->ejected || m->ops == NULL) {
        return -AG_EIO;
    }

    out->slot = s;
    out->mount = m;
    return AG_OK;
}

ag_err_t ag_vfs_close(ag_handle_t h)
{
    lock();
    ag_vfs_handle_slot_t *s = slot_of(h);
    if (s == NULL || s->is_dir) {
        unlock();
        return -AG_EBADF;
    }

    ag_err_t err = AG_OK;
    ag_mount_t *m = &s_mounts[s->mount];
    if (!m->ejected && m->ops != NULL && m->ops->close != NULL) {
        err = m->ops->close(m->ctx, s->obj);
    }
    free_slot(h);
    unlock();
    return err;
}

int32_t ag_vfs_read(ag_handle_t h, void *buf, size_t len)
{
    open_ref_t ref;

    lock();
    ag_err_t err = handle_ref(h, false, &ref);
    if (err != AG_OK) {
        unlock();
        return err;
    }
    if (ref.mount->ops->read == NULL) {
        unlock();
        return -AG_ENOTSUP;
    }

    const int32_t n = ref.mount->ops->read(ref.mount->ctx, ref.slot->obj, buf,
                                           len);
    unlock();
    return n;
}

int32_t ag_vfs_write(ag_handle_t h, const void *buf, size_t len)
{
    open_ref_t ref;

    lock();
    ag_err_t err = handle_ref(h, false, &ref);
    if (err != AG_OK) {
        unlock();
        return err;
    }
    if (ref.mount->flags & AG_MOUNT_READONLY) {
        unlock();
        return -AG_EROFS;
    }
    if (ref.mount->ops->write == NULL) {
        unlock();
        return -AG_ENOTSUP;
    }

    const int32_t n = ref.mount->ops->write(ref.mount->ctx, ref.slot->obj, buf,
                                            len);
    unlock();
    return n;
}

int64_t ag_vfs_seek(ag_handle_t h, int64_t off, int whence)
{
    open_ref_t ref;

    lock();
    ag_err_t err = handle_ref(h, false, &ref);
    if (err != AG_OK) {
        unlock();
        return err;
    }
    if (ref.mount->ops->seek == NULL) {
        unlock();
        return -AG_ENOTSUP;
    }

    const int64_t pos = ref.mount->ops->seek(ref.mount->ctx, ref.slot->obj, off,
                                             whence);
    unlock();
    return pos;
}

ag_err_t ag_vfs_sync(ag_handle_t h)
{
    open_ref_t ref;

    lock();
    ag_err_t err = handle_ref(h, false, &ref);
    if (err != AG_OK) {
        unlock();
        return err;
    }

    err = (ref.mount->ops->sync != NULL)
              ? ref.mount->ops->sync(ref.mount->ctx, ref.slot->obj)
              : AG_OK;
    unlock();
    return err;
}

ag_err_t ag_vfs_truncate(ag_handle_t h, uint64_t len)
{
    open_ref_t ref;

    lock();
    ag_err_t err = handle_ref(h, false, &ref);
    if (err != AG_OK) {
        unlock();
        return err;
    }
    if (ref.mount->flags & AG_MOUNT_READONLY) {
        unlock();
        return -AG_EROFS;
    }
    if (ref.mount->ops->truncate == NULL) {
        unlock();
        return -AG_ENOTSUP;
    }

    err = ref.mount->ops->truncate(ref.mount->ctx, ref.slot->obj, len);
    unlock();
    return err;
}

/* ---------------------------------------------------------------------- */
/* Metadata                                                               */
/* ---------------------------------------------------------------------- */

ag_err_t ag_vfs_stat(const char *path, const char *cwd, ag_stat_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }

    resolved_t r;
    lock();
    ag_err_t err = resolve(path, cwd, &r);

    /*
     * The root of the namespace is a directory even when nothing is mounted
     * at "/": it is where the mount points live.
     */
    if (err == -AG_ENOENT && strcmp(r.canonical, "/") == 0) {
        memset(out, 0, sizeof(*out));
        out->attr = AG_A_DIR;
        unlock();
        return AG_OK;
    }
    if (err != AG_OK) {
        unlock();
        return err;
    }
    if (r.mount->ops->stat == NULL) {
        unlock();
        return -AG_ENOTSUP;
    }

    err = r.mount->ops->stat(r.mount->ctx, r.rel, out);
    unlock();
    return err;
}

/* The three single-path mutating operations differ only in which op to call. */
typedef enum { MUTATE_UNLINK, MUTATE_MKDIR, MUTATE_RMDIR } mutate_op_t;

static ag_err_t mutate(const char *path, const char *cwd, mutate_op_t which)
{
    resolved_t r;

    lock();
    ag_err_t err = resolve(path, cwd, &r);
    if (err != AG_OK) {
        unlock();
        return err;
    }
    if (r.mount->flags & AG_MOUNT_READONLY) {
        unlock();
        return -AG_EROFS;
    }

    ag_err_t (*fn)(void *, const char *) = NULL;
    switch (which) {
    case MUTATE_UNLINK: fn = r.mount->ops->unlink; break;
    case MUTATE_MKDIR:  fn = r.mount->ops->mkdir; break;
    case MUTATE_RMDIR:  fn = r.mount->ops->rmdir; break;
    }
    if (fn == NULL) {
        unlock();
        return -AG_ENOTSUP;
    }

    err = fn(r.mount->ctx, r.rel);
    unlock();
    return err;
}

ag_err_t ag_vfs_unlink(const char *path, const char *cwd)
{
    return mutate(path, cwd, MUTATE_UNLINK);
}

ag_err_t ag_vfs_mkdir(const char *path, const char *cwd)
{
    return mutate(path, cwd, MUTATE_MKDIR);
}

ag_err_t ag_vfs_rmdir(const char *path, const char *cwd)
{
    return mutate(path, cwd, MUTATE_RMDIR);
}

ag_err_t ag_vfs_rename(const char *from, const char *to, const char *cwd)
{
    resolved_t rf;
    resolved_t rt;

    lock();
    ag_err_t err = resolve(from, cwd, &rf);
    if (err != AG_OK) {
        unlock();
        return err;
    }
    err = resolve(to, cwd, &rt);
    if (err != AG_OK) {
        unlock();
        return err;
    }
    /*
     * Renaming across filesystems is a copy followed by a delete, which the
     * shell can do explicitly; silently doing it here would hide the cost.
     */
    if (rf.index != rt.index) {
        unlock();
        return -AG_EPERM;
    }
    if (rf.mount->flags & AG_MOUNT_READONLY) {
        unlock();
        return -AG_EROFS;
    }
    if (rf.mount->ops->rename == NULL) {
        unlock();
        return -AG_ENOTSUP;
    }

    /* resolve() reuses its own buffer, so the source path needs a copy. */
    char from_rel[AG_PATH_MAX];
    strncpy(from_rel, rf.rel, sizeof(from_rel) - 1);
    from_rel[sizeof(from_rel) - 1] = '\0';

    err = rf.mount->ops->rename(rf.mount->ctx, from_rel, rt.rel);
    unlock();
    return err;
}

/* ---------------------------------------------------------------------- */
/* Directories                                                            */
/* ---------------------------------------------------------------------- */

ag_handle_t ag_vfs_opendir(const char *path, const char *cwd)
{
    resolved_t r;

    lock();
    ag_err_t err = resolve(path, cwd, &r);

    if (err == -AG_ENOENT && strcmp(r.canonical, "/") == 0) {
        /* Listing the root lists the mount points, like drives in DOS. */
        const ag_handle_t h = alloc_slot();
        if (h >= 0) {
            s_handles[h].is_dir = true;
            s_handles[h].is_root = true;
        }
        unlock();
        return h;
    }
    if (err != AG_OK) {
        unlock();
        return err;
    }
    if (r.mount->ops->opendir == NULL) {
        unlock();
        return -AG_ENOTSUP;
    }

    void *dir = NULL;
    err = r.mount->ops->opendir(r.mount->ctx, r.rel, &dir);
    if (err != AG_OK) {
        unlock();
        return err;
    }

    const ag_handle_t h = alloc_slot();
    if (h < 0) {
        if (r.mount->ops->closedir != NULL) {
            r.mount->ops->closedir(r.mount->ctx, dir);
        }
        unlock();
        return h;
    }

    s_handles[h].mount = (uint8_t)r.index;
    s_handles[h].obj = dir;
    s_handles[h].is_dir = true;
    r.mount->open_handles++;

    unlock();
    return h;
}

ag_err_t ag_vfs_readdir(ag_handle_t h, ag_dirent_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }

    lock();
    ag_vfs_handle_slot_t *s = slot_of(h);
    if (s == NULL || !s->is_dir) {
        unlock();
        return -AG_EBADF;
    }

    if (s->is_root) {
        /* Each mount point appears as a directory entry. */
        uint32_t seen = 0;
        for (int i = 0; i < AG_VFS_MAX_MOUNTS; i++) {
            if (!s_mounts[i].used) {
                continue;
            }
            if (seen++ != s->root_index) {
                continue;
            }
            memset(out, 0, sizeof(*out));
            /* Skip the leading slash: "/tmp" lists as "tmp". */
            strncpy(out->name, s_mounts[i].point + 1, sizeof(out->name) - 1);
            out->st.attr = AG_A_DIR;
            s->root_index++;
            unlock();
            return AG_OK;
        }
        unlock();
        return -AG_ENOENT;
    }

    ag_mount_t *m = &s_mounts[s->mount];
    if (m->ejected || m->ops == NULL) {
        unlock();
        return -AG_EIO;
    }
    if (m->ops->readdir == NULL) {
        unlock();
        return -AG_ENOTSUP;
    }

    const ag_err_t err = m->ops->readdir(m->ctx, s->obj, out);
    unlock();
    return err;
}

ag_err_t ag_vfs_closedir(ag_handle_t h)
{
    lock();
    ag_vfs_handle_slot_t *s = slot_of(h);
    if (s == NULL || !s->is_dir) {
        unlock();
        return -AG_EBADF;
    }

    ag_err_t err = AG_OK;
    if (!s->is_root) {
        ag_mount_t *m = &s_mounts[s->mount];
        if (!m->ejected && m->ops != NULL && m->ops->closedir != NULL) {
            err = m->ops->closedir(m->ctx, s->obj);
        }
    }
    free_slot(h);
    unlock();
    return err;
}
