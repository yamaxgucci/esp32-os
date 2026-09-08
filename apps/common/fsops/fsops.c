/*
 * ArgonOS - file operations: the walk and the decisions.
 *
 * No console, no allocation, and nothing from argon.h beyond types: everything
 * this touches arrives in fsops_fs_t, which is what lets the host tests run it
 * over a tree that only exists in memory.  fsops_argon.c is the other half.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "fsops.h"

#include <argon/libc.h>

/* ---------------------------------------------------------------------- */
/* Paths                                                                  */
/* ---------------------------------------------------------------------- */

void fsops_join(const char *dir, const char *name, char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return;
    }
    out[0] = '\0';
    if (dir == NULL) {
        dir = "";
    }
    if (name == NULL) {
        name = "";
    }

    ag_strlcpy(out, dir, len);
    size_t at = 0;
    while (out[at] != '\0') {
        at++;
    }

    /* Exactly one separator, whichever end brought one. */
    while (at > 1 && (out[at - 1] == '/' || out[at - 1] == '\\')) {
        out[--at] = '\0';
    }
    while (*name == '/' || *name == '\\') {
        name++;
    }
    if (at > 0 && out[at - 1] != '/' && out[at - 1] != '\\') {
        ag_strlcat(out, "/", len);
    }
    ag_strlcat(out, name, len);
}

/*
 * Copies and says whether all of it fit.
 *
 * ag_strlcpy returns what it WROTE, not what it was handed, so it cannot
 * answer this itself - and a path silently cut at 255 characters is a path
 * that names a different file.  Worth its own function because getting this
 * wrong is not visible at the call site.
 */
static bool copy_fits(char *dst, const char *src, size_t size)
{
    if (dst == NULL || src == NULL || size == 0) {
        return false;
    }
    if (strlen(src) + 1u > size) {
        return false;
    }
    ag_strlcpy(dst, src, size);
    return true;
}

bool fsops_is_dot(const char *name)
{
    if (name == NULL) {
        return true; /* nothing is safer to skip than a name that is not there */
    }
    if (name[0] != '.') {
        return false;
    }
    return name[1] == '\0' || (name[1] == '.' && name[2] == '\0');
}

/* ---------------------------------------------------------------------- */
/* The walk                                                              */
/* ---------------------------------------------------------------------- */

/*
 * One directory handle open at any moment, and the walk pays for it.
 *
 * Descending closes the parent's handle and remembers how many of its entries
 * have been dealt with; coming back reopens it and skips that many.  So a
 * return from a subdirectory costs one reopen and one readdir per entry
 * already passed, and a directory holding a hundred subdirectories among a
 * hundred files pays a few thousand extra readdir calls - noticeable over
 * HostFS, invisible on flash, and paid only in that shape.
 *
 * The alternative is a handle per level, which is cheaper and can fail: there
 * are twenty-four handles in the whole system, they are shared with everything
 * else running, and running out arrives in the MIDDLE of a copy, where the
 * only honest thing left to do is stop.  A copy must not be able to fail for a
 * reason that has nothing to do with the files being copied.
 */
typedef struct {
    ag_handle_t dir;      /* open while this level is being read, else -1 */
    uint32_t    skip;     /* entries of this level already dealt with     */
    uint16_t    from_len; /* the source path, as it stands at this level  */
    uint16_t    to_len;   /* and the destination                          */
} level_t;

typedef struct walk_s {
    fsops_req_t *req;

    char from[AG_PATH_MAX];
    char to[AG_PATH_MAX];
    bool want_to; /* false where there is no destination: delete, measure */

    level_t     level[FSOPS_MAX_DEPTH];
    int         depth;
    ag_dirent_t ent;

    fsops_progress_t p;

    /* What fsops_measure counts. */
    uint64_t bytes;
    uint32_t files;
} walk_t;

/*
 * What to do at each kind of node.  `enter` runs on the way in with the paths
 * already pointing at the directory, `leave` on the way out with them still
 * pointing at it, `file` with them pointing at the file.
 */
typedef struct {
    ag_err_t (*enter)(walk_t *w);
    ag_err_t (*leave)(walk_t *w);
    ag_err_t (*file)(walk_t *w, const ag_stat_t *st);
} actions_t;

static bool ticked(walk_t *w, const char *path)
{
    if (w->req->tick == NULL) {
        return true;
    }
    w->p.path = path;
    w->p.total_bytes = w->req->total_bytes;
    w->p.files_total = w->req->files_total;
    return w->req->tick(w->req->tick_ctx, &w->p);
}

static void cut(char *s, uint16_t len) { s[len] = '\0'; }

static bool append(walk_t *w, const char *name)
{
    char joined[AG_PATH_MAX];

    fsops_join(w->from, name, joined, sizeof(joined));
    if (joined[0] == '\0' || !copy_fits(w->from, joined, sizeof(w->from))) {
        return false;
    }
    if (!w->want_to) {
        return true;
    }
    fsops_join(w->to, name, joined, sizeof(joined));
    return copy_fits(w->to, joined, sizeof(w->to));
}

/*
 * Walks the directory the paths already name, depth first.
 *
 * The top level's enter/leave are the caller's business, not this function's:
 * a copy has to make its destination directory before it can walk into it, and
 * a delete has to remove the top directory after everything under it - both of
 * which the callers do around this call, where the error can be reported
 * against the name the user actually gave.
 */
static ag_err_t walk_tree(walk_t *w, const actions_t *a)
{
    const fsops_fs_t *fs = w->req->fs;
    void             *c = w->req->fs_ctx;
    ag_err_t          err = AG_OK;

    w->depth = 0;
    w->level[0].dir = -1;
    w->level[0].skip = 0;
    w->level[0].from_len = (uint16_t)strlen(w->from);
    w->level[0].to_len = (uint16_t)strlen(w->to);

    while (w->depth >= 0) {
        level_t *lv = &w->level[w->depth];

        if (lv->dir < 0) {
            const ag_handle_t h = fs->opendir(c, w->from);
            if (h < 0) {
                err = (ag_err_t)h;
                break;
            }
            lv->dir = h;
            /* Back where we were: everything up to `skip` is already done. */
            for (uint32_t i = 0; i < lv->skip; i++) {
                if (fs->readdir(c, h, &w->ent) != AG_OK) {
                    break;
                }
            }
        }

        const ag_err_t r = fs->readdir(c, lv->dir, &w->ent);
        if (r != AG_OK) {
            (void)fs->closedir(c, lv->dir);
            lv->dir = -1;

            /*
             * -AG_ENOENT is the end of the directory.  Anything else is a
             * directory that stopped answering partway, and it must not be
             * mistaken for the end: a copy that quietly stops at entry nine of
             * eleven and reports success is worse than one that fails.
             */
            if (r != -AG_ENOENT) {
                err = r;
                break;
            }

            /*
             * Not at depth 0: the top of the request belongs to the caller,
             * which has to report an error against the name the user gave -
             * and a delete that removed it here as well would then remove it
             * twice and answer -AG_ENOENT for a tree it had just emptied.
             */
            if (a->leave != NULL && w->depth > 0) {
                err = a->leave(w);
                if (err != AG_OK) {
                    break;
                }
            }
            w->depth--;
            if (w->depth >= 0) {
                cut(w->from, w->level[w->depth].from_len);
                if (w->want_to) {
                    cut(w->to, w->level[w->depth].to_len);
                }
            }
            continue;
        }

        lv->skip++;
        if (fsops_is_dot(w->ent.name)) {
            continue;
        }

        const bool is_dir = (w->ent.st.attr & AG_A_DIR) != 0;

        if (!append(w, w->ent.name)) {
            err = -AG_ERANGE;
            break;
        }

        if (!is_dir) {
            err = (a->file != NULL) ? a->file(w, &w->ent.st) : AG_OK;
            cut(w->from, lv->from_len);
            if (w->want_to) {
                cut(w->to, lv->to_len);
            }
            if (err != AG_OK) {
                break;
            }
            continue;
        }

        if (w->depth + 1 >= FSOPS_MAX_DEPTH) {
            /* Named, so the person knows which branch to look at. */
            err = -AG_ERANGE;
            break;
        }

        /* Descending: let go of the parent's handle first, on purpose. */
        (void)fs->closedir(c, lv->dir);
        lv->dir = -1;

        w->depth++;
        level_t *child = &w->level[w->depth];
        child->dir = -1;
        child->skip = 0;
        child->from_len = (uint16_t)strlen(w->from);
        child->to_len = w->want_to ? (uint16_t)strlen(w->to) : 0;

        if (a->enter != NULL) {
            err = a->enter(w);
            if (err != AG_OK) {
                break;
            }
        }
    }

    /* However it ended, no handle of ours is left open. */
    for (int i = 0; i <= w->depth && i < FSOPS_MAX_DEPTH; i++) {
        if (w->level[i].dir >= 0) {
            (void)fs->closedir(c, w->level[i].dir);
            w->level[i].dir = -1;
        }
    }
    return err;
}

/* ---------------------------------------------------------------------- */
/* Measuring                                                              */
/* ---------------------------------------------------------------------- */

/*
 * The size, asked for again when the listing did not carry one.
 *
 * HostFS fills no size in readdir, so anything that trusts the listing
 * measures a tree as zero bytes and draws a bar that never moves.  Both the
 * measure and the copy hit this, and both have to ask.
 */
static uint64_t size_of(walk_t *w, uint64_t from_listing)
{
    if (from_listing != 0) {
        return from_listing;
    }
    ag_stat_t st;
    if (w->req->fs->stat(w->req->fs_ctx, w->from, &st) != AG_OK ||
        (st.attr & AG_A_DIR) != 0) {
        return 0;
    }
    return st.size;
}

static ag_err_t measure_file(walk_t *w, const ag_stat_t *st)
{
    w->bytes += size_of(w, st->size);
    w->files++;
    return ticked(w, w->from) ? AG_OK : -AG_EKILLED;
}

ag_err_t fsops_measure(fsops_req_t *req, const char *path, uint64_t *bytes,
                       uint32_t *files)
{
    if (req == NULL || req->fs == NULL || path == NULL) {
        return -AG_EINVAL;
    }

    walk_t w;
    memset(&w, 0, sizeof(w));
    w.req = req;
    w.want_to = false;
    if (!copy_fits(w.from, path, sizeof(w.from))) {
        return -AG_ERANGE;
    }

    ag_stat_t st;
    ag_err_t  err = req->fs->stat(req->fs_ctx, path, &st);
    if (err != AG_OK) {
        return err;
    }

    if ((st.attr & AG_A_DIR) == 0) {
        w.bytes = st.size;
        w.files = 1;
    } else {
        static const actions_t k_measure = {NULL, NULL, measure_file};
        err = walk_tree(&w, &k_measure);
    }

    if (bytes != NULL) {
        *bytes = w.bytes;
    }
    if (files != NULL) {
        *files = w.files;
    }
    return err;
}

/* ---------------------------------------------------------------------- */
/* Copying                                                                */
/* ---------------------------------------------------------------------- */

/*
 * One file.  The error comes back rather than being reported: the caller knows
 * which of the two names to put in front of it, and this does not know whether
 * it is a copy or the copy half of a move.
 */
static ag_err_t copy_one(walk_t *w, uint64_t known_size)
{
    const fsops_fs_t *fs = w->req->fs;
    void             *c = w->req->fs_ctx;

    const uint64_t total = size_of(w, known_size);

    const ag_handle_t src = fs->open(c, w->from, AG_O_RDONLY);
    if (src < 0) {
        return (ag_err_t)src;
    }
    const ag_handle_t dst =
        fs->open(c, w->to, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (dst < 0) {
        (void)fs->close(c, src);
        return (ag_err_t)dst;
    }

    ag_err_t err = AG_OK;
    uint64_t done = 0;
    uint64_t since_tick = 0;

    w->p.file_done = 0;
    w->p.file_total = total;
    if (!ticked(w, w->from)) {
        err = -AG_EKILLED;
    }

    while (err == AG_OK) {
        const int32_t n = fs->read(c, src, w->req->chunk, w->req->chunk_len);
        if (n < 0) {
            err = (ag_err_t)n;
            break;
        }
        if (n == 0) {
            break;
        }
        const int32_t wrote = fs->write(c, dst, w->req->chunk, (uint32_t)n);
        if (wrote != n) {
            /* A short write with no error of its own is a full disk. */
            err = (wrote < 0) ? (ag_err_t)wrote : -AG_ENOSPC;
            break;
        }

        done += (uint64_t)n;
        since_tick += (uint64_t)n;
        w->p.file_done = done;
        w->p.total_done += (uint64_t)n;

        if (since_tick >= FSOPS_TICK_BYTES || (total > 0 && done >= total)) {
            since_tick = 0;
            if (!ticked(w, w->from)) {
                err = -AG_EKILLED;
            }
        }
    }

    if (err == AG_OK) {
        err = fs->sync(c, dst);
    } else {
        (void)fs->sync(c, dst);
    }
    (void)fs->close(c, dst);
    (void)fs->close(c, src);

    if (err != AG_OK) {
        /*
         * A half-written file is worse than no file: it has a plausible name
         * and the wrong contents, and nothing later will say so.  The unlink
         * may itself fail (a read-only card), and there is nothing better to
         * do about that than leave the first error standing.
         */
        (void)fs->unlink(c, w->to);
    } else {
        /*
         * Counted and reported.  Without a tick here the last thing anyone
         * sees is "2 of 3": the count goes up after the copy loop has stopped
         * ticking, and a bar that never reaches its own total looks like a
         * copy that did not finish.
         */
        w->p.files_done++;
        if (!ticked(w, w->from)) {
            err = -AG_EKILLED;
        }
    }
    return err;
}

static ag_err_t copy_enter(walk_t *w)
{
    const ag_err_t err = w->req->fs->mkdir(w->req->fs_ctx, w->to);
    /* Copying onto a tree that partly exists is a normal thing to ask for. */
    if (err != AG_OK && err != -AG_EEXIST) {
        return err;
    }
    return ticked(w, w->to) ? AG_OK : -AG_EKILLED;
}

static ag_err_t copy_file_node(walk_t *w, const ag_stat_t *st)
{
    return copy_one(w, st->size);
}

ag_err_t fsops_copy(fsops_req_t *req, const char *from, const char *to)
{
    if (req == NULL || req->fs == NULL || from == NULL || to == NULL ||
        req->chunk == NULL || req->chunk_len == 0) {
        return -AG_EINVAL;
    }

    walk_t w;
    memset(&w, 0, sizeof(w));
    w.req = req;
    w.want_to = true;
    if (!copy_fits(w.from, from, sizeof(w.from)) ||
        !copy_fits(w.to, to, sizeof(w.to))) {
        return -AG_ERANGE;
    }

    ag_stat_t st;
    ag_err_t  err = req->fs->stat(req->fs_ctx, from, &st);
    if (err != AG_OK) {
        return err;
    }

    if ((st.attr & AG_A_DIR) == 0) {
        return copy_one(&w, st.size);
    }

    /*
     * A directory into itself would copy for ever, and the check is a prefix
     * one rather than an equality: "a" into "a/b" is the same trap wearing a
     * different name, and it is the one someone actually types.
     */
    const size_t from_len = strlen(from);
    if (memcmp(to, from, from_len) == 0 &&
        (to[from_len] == '\0' || to[from_len] == '/' || to[from_len] == '\\')) {
        return -AG_EINVAL;
    }

    err = req->fs->mkdir(req->fs_ctx, to);
    if (err != AG_OK && err != -AG_EEXIST) {
        return err;
    }

    static const actions_t k_copy = {copy_enter, NULL, copy_file_node};
    return walk_tree(&w, &k_copy);
}

/* ---------------------------------------------------------------------- */
/* Deleting                                                               */
/* ---------------------------------------------------------------------- */

static ag_err_t delete_file_node(walk_t *w, const ag_stat_t *st)
{
    (void)st;
    const ag_err_t err = w->req->fs->unlink(w->req->fs_ctx, w->from);
    if (err != AG_OK) {
        return err;
    }
    w->p.files_done++;
    return ticked(w, w->from) ? AG_OK : -AG_EKILLED;
}

/* Children first, then the directory - so a stop halfway leaves a smaller
 * tree rather than a broken one. */
static ag_err_t delete_leave(walk_t *w)
{
    return w->req->fs->rmdir(w->req->fs_ctx, w->from);
}

ag_err_t fsops_delete(fsops_req_t *req, const char *path)
{
    if (req == NULL || req->fs == NULL || path == NULL) {
        return -AG_EINVAL;
    }

    walk_t w;
    memset(&w, 0, sizeof(w));
    w.req = req;
    w.want_to = false;
    if (!copy_fits(w.from, path, sizeof(w.from))) {
        return -AG_ERANGE;
    }

    ag_stat_t st;
    ag_err_t  err = req->fs->stat(req->fs_ctx, path, &st);
    if (err != AG_OK) {
        return err;
    }
    if ((st.attr & AG_A_DIR) == 0) {
        return delete_file_node(&w, &st);
    }

    static const actions_t k_delete = {NULL, delete_leave, delete_file_node};
    err = walk_tree(&w, &k_delete);
    if (err != AG_OK) {
        return err;
    }
    return req->fs->rmdir(req->fs_ctx, path);
}

/* ---------------------------------------------------------------------- */
/* Moving                                                                 */
/* ---------------------------------------------------------------------- */

ag_err_t fsops_move(fsops_req_t *req, const char *from, const char *to)
{
    if (req == NULL || req->fs == NULL || from == NULL || to == NULL) {
        return -AG_EINVAL;
    }

    /*
     * Rename first, always.  It is instant, it works across a directory but
     * not across a filesystem, and there is no way to ask which case this is
     * that is more reliable than trying.
     */
    const ag_err_t renamed = req->fs->rename(req->fs_ctx, from, to);
    if (renamed == AG_OK) {
        return AG_OK;
    }

    /*
     * Copy and delete instead.  The delete happens only after a copy that
     * returned AG_OK, so a cancelled or failed move leaves the source where it
     * was - which is the property that makes a move safe to interrupt.
     */
    const ag_err_t copied = fsops_copy(req, from, to);
    if (copied != AG_OK) {
        return copied;
    }
    return fsops_delete(req, from);
}
