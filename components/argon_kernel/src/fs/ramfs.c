/*
 * ArgonOS - in-memory filesystem.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/ramfs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ram_node {
    char             name[AG_NAME_MAX];
    struct ram_node *parent;
    struct ram_node *sibling;
    struct ram_node *child; /* directories only */
    bool             is_dir;

    uint8_t *data;
    size_t   size;
    size_t   capacity;
    uint64_t mtime;
} ram_node_t;

struct ag_ramfs {
    ram_node_t root;
    size_t     budget;
    size_t     used;
    uint64_t (*now)(void);
    void *(*alloc)(size_t);
    void (*release)(void *);
};

/* Allocation goes through the filesystem so /tmp can live in PSRAM. */
static void *fs_alloc(ag_ramfs_t *fs, size_t bytes)
{
    void *p = (fs->alloc != NULL) ? fs->alloc(bytes) : malloc(bytes);
    if (p != NULL) {
        memset(p, 0, bytes);
    }
    return p;
}

static void fs_free(ag_ramfs_t *fs, void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    if (fs->release != NULL) {
        fs->release(ptr);
    } else {
        free(ptr);
    }
}

/*
 * Growing a buffer is a fresh allocation plus a copy rather than realloc,
 * because a custom allocator only has to provide alloc and free.
 */
static void *fs_regrow(ag_ramfs_t *fs, void *old, size_t old_size,
                       size_t new_size)
{
    void *p = fs_alloc(fs, new_size);
    if (p == NULL) {
        return NULL;
    }
    if (old != NULL && old_size > 0) {
        memcpy(p, old, old_size);
    }
    fs_free(fs, old);
    return p;
}

typedef struct {
    ram_node_t *node;
    size_t      pos;
    bool        writable;
    bool        append;
} ram_file_t;

typedef struct {
    ram_node_t *next;
} ram_dir_t;

/* ---------------------------------------------------------------------- */
/* Budget                                                                 */
/* ---------------------------------------------------------------------- */

static bool charge(ag_ramfs_t *fs, size_t bytes)
{
    if (fs->used + bytes > fs->budget) {
        return false;
    }
    fs->used += bytes;
    return true;
}

static void refund(ag_ramfs_t *fs, size_t bytes)
{
    fs->used = (fs->used > bytes) ? (fs->used - bytes) : 0;
}

/* ---------------------------------------------------------------------- */
/* Tree                                                                   */
/* ---------------------------------------------------------------------- */

static ram_node_t *find_child(ram_node_t *dir, const char *name, size_t len)
{
    for (ram_node_t *n = dir->child; n != NULL; n = n->sibling) {
        if (strlen(n->name) != len) {
            continue;
        }
        /* Case insensitive, to behave the same way FAT does. */
        bool same = true;
        for (size_t i = 0; i < len; i++) {
            char a = n->name[i];
            char b = name[i];
            if (a >= 'A' && a <= 'Z') { a = (char)(a + 32); }
            if (b >= 'A' && b <= 'Z') { b = (char)(b + 32); }
            if (a != b) {
                same = false;
                break;
            }
        }
        if (same) {
            return n;
        }
    }
    return NULL;
}

/*
 * Walks a path relative to the mount root.  When `parent_of` is set, stops one
 * component short and reports the final name, which is what create, mkdir and
 * rename need.
 */
static ram_node_t *walk(ag_ramfs_t *fs, const char *path, bool parent_of,
                        const char **leaf, size_t *leaf_len)
{
    ram_node_t *node = &fs->root;
    const char *p = path;

    if (leaf != NULL) {
        *leaf = NULL;
        *leaf_len = 0;
    }

    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        const char *start = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        const size_t len = (size_t)(p - start);

        /* Is this the last component? */
        const char *rest = p;
        while (*rest == '/') {
            rest++;
        }
        const bool last = (*rest == '\0');

        if (last && parent_of) {
            if (leaf != NULL) {
                *leaf = start;
                *leaf_len = len;
            }
            return node->is_dir ? node : NULL;
        }

        if (!node->is_dir) {
            return NULL;
        }
        node = find_child(node, start, len);
        if (node == NULL) {
            return NULL;
        }
    }

    /* The path was the root itself. */
    if (parent_of && leaf != NULL && *leaf == NULL) {
        return NULL;
    }
    return node;
}

static ram_node_t *create_node(ag_ramfs_t *fs, ram_node_t *parent,
                               const char *name, size_t len, bool is_dir)
{
    if (len == 0 || len >= AG_NAME_MAX) {
        return NULL;
    }
    if (!charge(fs, sizeof(ram_node_t))) {
        return NULL;
    }

    ram_node_t *n = (ram_node_t *)fs_alloc(fs, sizeof(ram_node_t));
    if (n == NULL) {
        refund(fs, sizeof(ram_node_t));
        return NULL;
    }

    memcpy(n->name, name, len);
    n->name[len] = '\0';
    n->is_dir = is_dir;
    n->parent = parent;
    n->sibling = parent->child;
    n->mtime = (fs->now != NULL) ? fs->now() : 0;
    parent->child = n;
    return n;
}

static void unlink_node(ag_ramfs_t *fs, ram_node_t *node)
{
    ram_node_t *parent = node->parent;
    if (parent != NULL) {
        ram_node_t **link = &parent->child;
        while (*link != NULL && *link != node) {
            link = &(*link)->sibling;
        }
        if (*link == node) {
            *link = node->sibling;
        }
    }

    if (node->data != NULL) {
        refund(fs, node->capacity);
        fs_free(fs, node->data);
    }
    refund(fs, sizeof(ram_node_t));
    fs_free(fs, node);
}

static void destroy_subtree(ag_ramfs_t *fs, ram_node_t *dir)
{
    ram_node_t *n = dir->child;
    while (n != NULL) {
        ram_node_t *next = n->sibling;
        if (n->is_dir) {
            destroy_subtree(fs, n);
        }
        fs_free(fs, n->data);
        fs_free(fs, n);
        n = next;
    }
    dir->child = NULL;
}

/* ---------------------------------------------------------------------- */

ag_ramfs_t *ag_ramfs_create(const ag_ramfs_config_t *config)
{
    if (config == NULL || config->budget == 0) {
        return NULL;
    }

    ag_ramfs_t *fs = (ag_ramfs_t *)((config->alloc != NULL)
                                        ? config->alloc(sizeof(ag_ramfs_t))
                                        : malloc(sizeof(ag_ramfs_t)));
    if (fs == NULL) {
        return NULL;
    }

    memset(fs, 0, sizeof(*fs));
    fs->budget = config->budget;
    fs->now = config->now_unix;
    fs->alloc = config->alloc;
    fs->release = config->release;
    fs->root.is_dir = true;
    strcpy(fs->root.name, "/");
    return fs;
}

void ag_ramfs_destroy(ag_ramfs_t *fs)
{
    if (fs != NULL) {
        destroy_subtree(fs, &fs->root);
        /* The struct is freed through its own allocator, so copy first. */
        void (*release)(void *) = fs->release;
        if (release != NULL) {
            release(fs);
        } else {
            free(fs);
        }
    }
}

size_t ag_ramfs_used(const ag_ramfs_t *fs) { return (fs != NULL) ? fs->used : 0; }

/* ---------------------------------------------------------------------- */
/* Backend operations                                                     */
/* ---------------------------------------------------------------------- */

static ag_err_t ramfs_open(void *ctx, const char *rel, uint32_t flags,
                           void **out)
{
    ag_ramfs_t *fs = (ag_ramfs_t *)ctx;
    ram_node_t *node = walk(fs, rel, false, NULL, NULL);

    if (node != NULL && node->is_dir) {
        return -AG_EISDIR;
    }

    if (node == NULL) {
        if (!(flags & AG_O_CREATE)) {
            return -AG_ENOENT;
        }
        const char *leaf = NULL;
        size_t      leaf_len = 0;
        ram_node_t *parent = walk(fs, rel, true, &leaf, &leaf_len);
        if (parent == NULL || leaf == NULL) {
            return -AG_ENOENT;
        }
        node = create_node(fs, parent, leaf, leaf_len, false);
        if (node == NULL) {
            return -AG_ENOSPC;
        }
    } else if ((flags & AG_O_CREATE) && (flags & AG_O_EXCL)) {
        return -AG_EEXIST;
    }

    if (flags & AG_O_TRUNC) {
        node->size = 0;
    }

    ram_file_t *f = (ram_file_t *)fs_alloc(fs, sizeof(ram_file_t));
    if (f == NULL) {
        return -AG_ENOMEM;
    }
    f->node = node;
    f->writable = (flags & (AG_O_WRONLY | AG_O_RDWR | AG_O_CREATE |
                            AG_O_TRUNC | AG_O_APPEND)) != 0;
    f->append = (flags & AG_O_APPEND) != 0;
    f->pos = f->append ? node->size : 0;

    *out = f;
    return AG_OK;
}

static ag_err_t ramfs_close(void *ctx, void *file)
{
    fs_free((ag_ramfs_t *)ctx, file);
    return AG_OK;
}

static int32_t ramfs_read(void *ctx, void *file, void *buf, size_t len)
{
    (void)ctx;
    ram_file_t *f = (ram_file_t *)file;

    if (f->pos >= f->node->size) {
        return 0;
    }
    size_t available = f->node->size - f->pos;
    if (len > available) {
        len = available;
    }
    memcpy(buf, f->node->data + f->pos, len);
    f->pos += len;
    return (int32_t)len;
}

static bool grow(ag_ramfs_t *fs, ram_node_t *node, size_t needed)
{
    if (needed <= node->capacity) {
        return true;
    }

    size_t capacity = (node->capacity == 0) ? 64 : node->capacity;
    while (capacity < needed) {
        capacity *= 2;
    }

    if (!charge(fs, capacity - node->capacity)) {
        return false;
    }

    uint8_t *data = (uint8_t *)fs_regrow(fs, node->data, node->size, capacity);
    if (data == NULL) {
        refund(fs, capacity - node->capacity);
        return false;
    }

    node->data = data;
    node->capacity = capacity;
    return true;
}

static int32_t ramfs_write(void *ctx, void *file, const void *buf, size_t len)
{
    ag_ramfs_t *fs = (ag_ramfs_t *)ctx;
    ram_file_t *f = (ram_file_t *)file;

    if (!f->writable) {
        return -AG_EACCES;
    }
    if (len == 0) {
        return 0;
    }
    if (f->append) {
        f->pos = f->node->size;
    }

    const size_t end = f->pos + len;
    if (!grow(fs, f->node, end)) {
        return -AG_ENOSPC;
    }

    /* A seek past the end then a write leaves a hole; it must read as zero. */
    if (f->pos > f->node->size) {
        memset(f->node->data + f->node->size, 0, f->pos - f->node->size);
    }

    memcpy(f->node->data + f->pos, buf, len);
    f->pos = end;
    if (end > f->node->size) {
        f->node->size = end;
    }
    f->node->mtime = (fs->now != NULL) ? fs->now() : f->node->mtime;
    return (int32_t)len;
}

static int64_t ramfs_seek(void *ctx, void *file, int64_t off, int whence)
{
    (void)ctx;
    ram_file_t *f = (ram_file_t *)file;

    int64_t base = 0;
    switch (whence) {
    case AG_SEEK_SET: base = 0; break;
    case AG_SEEK_CUR: base = (int64_t)f->pos; break;
    case AG_SEEK_END: base = (int64_t)f->node->size; break;
    default: return -AG_EINVAL;
    }

    const int64_t pos = base + off;
    if (pos < 0) {
        return -AG_EINVAL;
    }
    f->pos = (size_t)pos;
    return pos;
}

static ag_err_t ramfs_truncate(void *ctx, void *file, uint64_t len)
{
    ag_ramfs_t *fs = (ag_ramfs_t *)ctx;
    ram_file_t *f = (ram_file_t *)file;

    if (!f->writable) {
        return -AG_EACCES;
    }
    if (len > f->node->size) {
        if (!grow(fs, f->node, (size_t)len)) {
            return -AG_ENOSPC;
        }
        memset(f->node->data + f->node->size, 0,
               (size_t)len - f->node->size);
    }
    f->node->size = (size_t)len;
    return AG_OK;
}

static void fill_stat(const ram_node_t *node, ag_stat_t *out)
{
    memset(out, 0, sizeof(*out));
    out->size = node->is_dir ? 0 : node->size;
    out->mtime = node->mtime;
    out->attr = node->is_dir ? AG_A_DIR : 0;
}

static ag_err_t ramfs_stat(void *ctx, const char *rel, ag_stat_t *out)
{
    ag_ramfs_t *fs = (ag_ramfs_t *)ctx;
    const ram_node_t *node = walk(fs, rel, false, NULL, NULL);

    if (node == NULL) {
        return -AG_ENOENT;
    }
    fill_stat(node, out);
    return AG_OK;
}

static ag_err_t ramfs_unlink(void *ctx, const char *rel)
{
    ag_ramfs_t *fs = (ag_ramfs_t *)ctx;
    ram_node_t *node = walk(fs, rel, false, NULL, NULL);

    if (node == NULL) {
        return -AG_ENOENT;
    }
    if (node->is_dir) {
        return -AG_EISDIR;
    }
    unlink_node(fs, node);
    return AG_OK;
}

static ag_err_t ramfs_mkdir(void *ctx, const char *rel)
{
    ag_ramfs_t *fs = (ag_ramfs_t *)ctx;

    if (walk(fs, rel, false, NULL, NULL) != NULL) {
        return -AG_EEXIST;
    }

    const char *leaf = NULL;
    size_t      leaf_len = 0;
    ram_node_t *parent = walk(fs, rel, true, &leaf, &leaf_len);
    if (parent == NULL || leaf == NULL) {
        return -AG_ENOENT;
    }
    if (create_node(fs, parent, leaf, leaf_len, true) == NULL) {
        return -AG_ENOSPC;
    }
    return AG_OK;
}

static ag_err_t ramfs_rmdir(void *ctx, const char *rel)
{
    ag_ramfs_t *fs = (ag_ramfs_t *)ctx;
    ram_node_t *node = walk(fs, rel, false, NULL, NULL);

    if (node == NULL) {
        return -AG_ENOENT;
    }
    if (!node->is_dir) {
        return -AG_ENOTDIR;
    }
    if (node == &fs->root) {
        return -AG_EBUSY;
    }
    if (node->child != NULL) {
        return -AG_EBUSY; /* refuse to delete a directory with contents */
    }
    unlink_node(fs, node);
    return AG_OK;
}

static ag_err_t ramfs_rename(void *ctx, const char *from, const char *to)
{
    ag_ramfs_t *fs = (ag_ramfs_t *)ctx;
    ram_node_t *node = walk(fs, from, false, NULL, NULL);

    if (node == NULL) {
        return -AG_ENOENT;
    }
    if (node == &fs->root) {
        return -AG_EBUSY;
    }
    if (walk(fs, to, false, NULL, NULL) != NULL) {
        return -AG_EEXIST;
    }

    const char *leaf = NULL;
    size_t      leaf_len = 0;
    ram_node_t *parent = walk(fs, to, true, &leaf, &leaf_len);
    if (parent == NULL || leaf == NULL || leaf_len >= AG_NAME_MAX) {
        return -AG_ENOENT;
    }

    /* Moving a directory into itself would detach the whole subtree. */
    for (ram_node_t *p = parent; p != NULL; p = p->parent) {
        if (p == node) {
            return -AG_EINVAL;
        }
    }

    /* Detach without freeing, then reattach under the new parent. */
    ram_node_t **link = &node->parent->child;
    while (*link != NULL && *link != node) {
        link = &(*link)->sibling;
    }
    if (*link == node) {
        *link = node->sibling;
    }

    memcpy(node->name, leaf, leaf_len);
    node->name[leaf_len] = '\0';
    node->parent = parent;
    node->sibling = parent->child;
    parent->child = node;
    return AG_OK;
}

static ag_err_t ramfs_opendir(void *ctx, const char *rel, void **out)
{
    ag_ramfs_t *fs = (ag_ramfs_t *)ctx;
    ram_node_t *node = walk(fs, rel, false, NULL, NULL);

    if (node == NULL) {
        return -AG_ENOENT;
    }
    if (!node->is_dir) {
        return -AG_ENOTDIR;
    }

    ram_dir_t *d = (ram_dir_t *)fs_alloc(fs, sizeof(ram_dir_t));
    if (d == NULL) {
        return -AG_ENOMEM;
    }
    d->next = node->child;
    *out = d;
    return AG_OK;
}

static ag_err_t ramfs_readdir(void *ctx, void *dir, ag_dirent_t *out)
{
    (void)ctx;
    ram_dir_t *d = (ram_dir_t *)dir;

    if (d->next == NULL) {
        return -AG_ENOENT;
    }
    memset(out, 0, sizeof(*out));
    strncpy(out->name, d->next->name, sizeof(out->name) - 1);
    fill_stat(d->next, &out->st);
    d->next = d->next->sibling;
    return AG_OK;
}

static ag_err_t ramfs_closedir(void *ctx, void *dir)
{
    fs_free((ag_ramfs_t *)ctx, dir);
    return AG_OK;
}

static ag_err_t ramfs_info(void *ctx, ag_fsinfo_t *out)
{
    const ag_ramfs_t *fs = (const ag_ramfs_t *)ctx;

    memset(out, 0, sizeof(*out));
    strcpy(out->fs, "ram");
    out->total = fs->budget;
    out->free = (fs->budget > fs->used) ? (fs->budget - fs->used) : 0;
    return AG_OK;
}

static ag_err_t ramfs_canonicalize(void *ctx, char *rel, size_t rel_len)
{
    ag_ramfs_t *fs = (ag_ramfs_t *)ctx;

    if (rel == NULL || rel_len == 0) {
        return -AG_EINVAL;
    }

    char        out[AG_PATH_MAX];
    size_t      at = 0;
    ram_node_t *node = &fs->root;
    const char *p = rel;

    out[0] = '\0';
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *start = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        const size_t len = (size_t)(p - start);
        if (!node->is_dir) {
            return -AG_ENOTDIR;
        }
        ram_node_t *child = find_child(node, start, len);
        if (child == NULL) {
            return -AG_ENOENT;
        }
        const int n =
            snprintf(out + at, sizeof(out) - at, "/%s", child->name);
        if (n < 0 || (size_t)n >= sizeof(out) - at) {
            return -AG_ERANGE;
        }
        at += (size_t)n;
        node = child;
    }

    if (at == 0) {
        if (rel_len < 2) {
            return -AG_ERANGE;
        }
        rel[0] = '/';
        rel[1] = '\0';
        return AG_OK;
    }
    if (at >= rel_len) {
        return -AG_ERANGE;
    }
    memcpy(rel, out, at + 1);
    return AG_OK;
}

static const ag_fs_ops_t k_ramfs_ops = {
    .name = "ram",
    .open = ramfs_open,
    .close = ramfs_close,
    .read = ramfs_read,
    .write = ramfs_write,
    .seek = ramfs_seek,
    .sync = NULL, /* nothing to flush; memory is the storage */
    .truncate = ramfs_truncate,
    .stat = ramfs_stat,
    .unlink = ramfs_unlink,
    .rename = ramfs_rename,
    .mkdir = ramfs_mkdir,
    .rmdir = ramfs_rmdir,
    .opendir = ramfs_opendir,
    .readdir = ramfs_readdir,
    .closedir = ramfs_closedir,
    .info = ramfs_info,
    .canonicalize = ramfs_canonicalize,
};

const ag_fs_ops_t *ag_ramfs_ops(void) { return &k_ramfs_ops; }
