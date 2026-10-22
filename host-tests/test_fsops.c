/*
 * ArgonOS - file operations, checked without a filesystem.
 *
 * The interesting half of copying a directory is not the bytes, it is the
 * walk: the order things happen in, what is left behind when it stops
 * halfway, and whether a directory that answers an error partway through is
 * mistaken for a directory that has ended.  None of that is visible from
 * outside a real copy, and all of it is visible here - the tree below is a
 * flat array of paths, so a test can say exactly what should be left.
 *
 * The fake also does what a real filesystem does and code tends not to expect:
 * it hands out a handle per open and refuses when they run out, it reports no
 * size in readdir (HostFS does not), and it can be told to fail one named call.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include "../apps/common/fsops/fsops.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* A filesystem in an array                                              */
/* ---------------------------------------------------------------------- */

#define FAKE_MAX_NODES 64
#define FAKE_MAX_DATA 512
#define FAKE_MAX_HANDLES 8 /* fewer than the real 24, so tests can exhaust it */

typedef struct {
    bool     used;
    char     path[128];
    bool     is_dir;
    uint32_t size;
    char     data[FAKE_MAX_DATA];
} node_t;

typedef struct {
    bool used;
    bool is_dir;
    int  node;     /* index into nodes[]           */
    int  pos;      /* read offset, or dir entry    */
    bool writable;
} handle_t;

typedef struct {
    node_t   nodes[FAKE_MAX_NODES];
    handle_t handles[FAKE_MAX_HANDLES];

    /* Counters, so a test can say "and it cost one reopen, not a hundred". */
    int opens, opendirs, readdirs, writes, unlinks, rmdirs, mkdirs, renames;

    /* Fail the Nth call of one kind, to see what is left behind. */
    const char *fail_call;  /* "write", "readdir", "mkdir", NULL */
    int         fail_after; /* let this many through, then fail   */
    ag_err_t    fail_with;

    /* Renaming refused, the way a cross-filesystem rename is. */
    bool no_rename;

    /* Stop the operation after this many ticks; 0 = never. */
    int tick_stop;
    int ticks;

    /* The last progress seen, for the assertions about the bar. */
    fsops_progress_t last;
} fake_t;

static fake_t g_fs;

static bool fail_now(const char *what)
{
    if (g_fs.fail_call == NULL || strcmp(g_fs.fail_call, what) != 0) {
        return false;
    }
    if (g_fs.fail_after > 0) {
        g_fs.fail_after--;
        return false;
    }
    return true;
}

static int find_node(const char *path)
{
    for (int i = 0; i < FAKE_MAX_NODES; i++) {
        if (g_fs.nodes[i].used && strcmp(g_fs.nodes[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static int add_node(const char *path, bool is_dir, const char *data)
{
    for (int i = 0; i < FAKE_MAX_NODES; i++) {
        node_t *n = &g_fs.nodes[i];
        if (n->used) {
            continue;
        }
        memset(n, 0, sizeof(*n));
        n->used = true;
        n->is_dir = is_dir;
        snprintf(n->path, sizeof(n->path), "%s", path);
        if (data != NULL) {
            n->size = (uint32_t)strlen(data);
            memcpy(n->data, data, n->size);
        }
        return i;
    }
    AG_CHECK(!"the fake filesystem is full");
    return -1;
}

/* Is `path` a direct child of `dir`?  Returns the name, or NULL. */
static const char *child_of(const char *dir, const char *path)
{
    const size_t dl = strlen(dir);
    if (strncmp(path, dir, dl) != 0 || path[dl] != '/') {
        return NULL;
    }
    const char *name = &path[dl + 1];
    return (strchr(name, '/') == NULL) ? name : NULL;
}

static ag_handle_t take_handle(bool is_dir, int node, bool writable)
{
    for (int i = 0; i < FAKE_MAX_HANDLES; i++) {
        if (g_fs.handles[i].used) {
            continue;
        }
        g_fs.handles[i].used = true;
        g_fs.handles[i].is_dir = is_dir;
        g_fs.handles[i].node = node;
        g_fs.handles[i].pos = 0;
        g_fs.handles[i].writable = writable;
        return (ag_handle_t)i;
    }
    return -AG_ENFILE;
}

static handle_t *held(ag_handle_t h)
{
    if (h < 0 || h >= FAKE_MAX_HANDLES || !g_fs.handles[h].used) {
        return NULL;
    }
    return &g_fs.handles[h];
}

static ag_handle_t f_open(void *ctx, const char *path, uint32_t flags)
{
    (void)ctx;
    g_fs.opens++;
    int at = find_node(path);
    if (at < 0) {
        if ((flags & AG_O_CREATE) == 0) {
            return -AG_ENOENT;
        }
        at = add_node(path, false, NULL);
    } else if ((flags & AG_O_TRUNC) != 0) {
        g_fs.nodes[at].size = 0;
    }
    return take_handle(false, at, (flags & AG_O_WRONLY) != 0);
}

static int32_t f_read(void *ctx, ag_handle_t h, void *buf, uint32_t len)
{
    (void)ctx;
    handle_t *s = held(h);
    if (s == NULL || s->is_dir) {
        return -AG_EBADF;
    }
    node_t        *n = &g_fs.nodes[s->node];
    const uint32_t left = n->size - (uint32_t)s->pos;
    const uint32_t take = (len < left) ? len : left;
    memcpy(buf, &n->data[s->pos], take);
    s->pos += (int)take;
    return (int32_t)take;
}

static int32_t f_write(void *ctx, ag_handle_t h, const void *buf, uint32_t len)
{
    (void)ctx;
    g_fs.writes++;
    if (fail_now("write")) {
        return g_fs.fail_with;
    }
    handle_t *s = held(h);
    if (s == NULL || s->is_dir || !s->writable) {
        return -AG_EBADF;
    }
    node_t *n = &g_fs.nodes[s->node];
    if (s->pos + (int)len > FAKE_MAX_DATA) {
        return -AG_ENOSPC;
    }
    memcpy(&n->data[s->pos], buf, len);
    s->pos += (int)len;
    if ((uint32_t)s->pos > n->size) {
        n->size = (uint32_t)s->pos;
    }
    return (int32_t)len;
}

static ag_err_t f_sync(void *ctx, ag_handle_t h)
{
    (void)ctx;
    return (held(h) != NULL) ? AG_OK : -AG_EBADF;
}

static ag_err_t f_close(void *ctx, ag_handle_t h)
{
    (void)ctx;
    handle_t *s = held(h);
    if (s == NULL) {
        return -AG_EBADF;
    }
    s->used = false;
    return AG_OK;
}

static ag_err_t f_stat(void *ctx, const char *path, ag_stat_t *out)
{
    (void)ctx;
    const int at = find_node(path);
    if (at < 0) {
        return -AG_ENOENT;
    }
    memset(out, 0, sizeof(*out));
    out->size = g_fs.nodes[at].size;
    out->attr = g_fs.nodes[at].is_dir ? AG_A_DIR : 0;
    return AG_OK;
}

static ag_handle_t f_opendir(void *ctx, const char *path)
{
    (void)ctx;
    g_fs.opendirs++;
    const int at = find_node(path);
    if (at < 0) {
        return -AG_ENOENT;
    }
    if (!g_fs.nodes[at].is_dir) {
        return -AG_ENOTDIR;
    }
    return take_handle(true, at, false);
}

/*
 * Entries in array order, and NO SIZE - which is what HostFS does, and the
 * reason a progress bar built from a listing shows nothing until something
 * stats each file.
 */
static ag_err_t f_readdir(void *ctx, ag_handle_t h, ag_dirent_t *out)
{
    (void)ctx;
    g_fs.readdirs++;
    if (fail_now("readdir")) {
        return g_fs.fail_with;
    }
    handle_t *s = held(h);
    if (s == NULL || !s->is_dir) {
        return -AG_EBADF;
    }
    const char *dir = g_fs.nodes[s->node].path;

    for (int i = s->pos; i < FAKE_MAX_NODES; i++) {
        if (!g_fs.nodes[i].used) {
            continue;
        }
        const char *name = child_of(dir, g_fs.nodes[i].path);
        if (name == NULL) {
            continue;
        }
        s->pos = i + 1;
        memset(out, 0, sizeof(*out));
        snprintf(out->name, sizeof(out->name), "%s", name);
        out->st.attr = g_fs.nodes[i].is_dir ? AG_A_DIR : 0;
        return AG_OK;
    }
    s->pos = FAKE_MAX_NODES;
    return -AG_ENOENT;
}

static ag_err_t f_closedir(void *ctx, ag_handle_t h) { return f_close(ctx, h); }

static ag_err_t f_mkdir(void *ctx, const char *path)
{
    (void)ctx;
    g_fs.mkdirs++;
    if (fail_now("mkdir")) {
        return g_fs.fail_with;
    }
    if (find_node(path) >= 0) {
        return -AG_EEXIST;
    }
    (void)add_node(path, true, NULL);
    return AG_OK;
}

static ag_err_t f_unlink(void *ctx, const char *path)
{
    (void)ctx;
    g_fs.unlinks++;
    const int at = find_node(path);
    if (at < 0) {
        return -AG_ENOENT;
    }
    if (g_fs.nodes[at].is_dir) {
        return -AG_EISDIR;
    }
    g_fs.nodes[at].used = false;
    return AG_OK;
}

static ag_err_t f_rmdir(void *ctx, const char *path)
{
    (void)ctx;
    g_fs.rmdirs++;
    const int at = find_node(path);
    if (at < 0) {
        return -AG_ENOENT;
    }
    if (!g_fs.nodes[at].is_dir) {
        return -AG_ENOTDIR;
    }
    /* Refuses a directory with anything in it, like the real ones. */
    for (int i = 0; i < FAKE_MAX_NODES; i++) {
        if (g_fs.nodes[i].used && child_of(path, g_fs.nodes[i].path) != NULL) {
            return -AG_EBUSY;
        }
    }
    g_fs.nodes[at].used = false;
    return AG_OK;
}

static ag_err_t f_rename(void *ctx, const char *from, const char *to)
{
    (void)ctx;
    g_fs.renames++;
    if (g_fs.no_rename) {
        return -AG_EPERM; /* what a cross-filesystem rename comes back as */
    }
    const int at = find_node(from);
    if (at < 0) {
        return -AG_ENOENT;
    }
    snprintf(g_fs.nodes[at].path, sizeof(g_fs.nodes[at].path), "%s", to);
    return AG_OK;
}

static const fsops_fs_t k_fake = {
    .open = f_open,         .read = f_read,       .write = f_write,
    .sync = f_sync,         .close = f_close,     .stat = f_stat,
    .opendir = f_opendir,   .readdir = f_readdir, .closedir = f_closedir,
    .mkdir = f_mkdir,       .unlink = f_unlink,   .rmdir = f_rmdir,
    .rename = f_rename,
};

/* ---------------------------------------------------------------------- */
/* Harness                                                               */
/* ---------------------------------------------------------------------- */

static char g_chunk[8]; /* deliberately tiny: several reads per file */

static bool tick(void *ctx, const fsops_progress_t *p)
{
    (void)ctx;
    g_fs.ticks++;
    g_fs.last = *p;
    return !(g_fs.tick_stop > 0 && g_fs.ticks >= g_fs.tick_stop);
}

static fsops_req_t g_req;

static fsops_req_t *req(void)
{
    memset(&g_req, 0, sizeof(g_req));
    g_req.fs = &k_fake;
    g_req.tick = tick;
    g_req.chunk = g_chunk;
    g_req.chunk_len = (uint32_t)sizeof(g_chunk);
    return &g_req;
}

/* The tree every test starts from:
 *
 *   /a            /a/one.txt  /a/two.txt  /a/sub/  /a/sub/three.txt
 *   /b                                                              */
static void tree(void)
{
    memset(&g_fs, 0, sizeof(g_fs));
    (void)add_node("/a", true, NULL);
    (void)add_node("/a/one.txt", false, "one");
    (void)add_node("/a/two.txt", false, "two-two-two-two");
    (void)add_node("/a/sub", true, NULL);
    (void)add_node("/a/sub/three.txt", false, "three");
    (void)add_node("/b", true, NULL);
}

static bool exists(const char *path) { return find_node(path) >= 0; }

static const char *contents(const char *path)
{
    static char buf[FAKE_MAX_DATA + 1];
    const int   at = find_node(path);
    if (at < 0) {
        return "<no such file>";
    }
    memcpy(buf, g_fs.nodes[at].data, g_fs.nodes[at].size);
    buf[g_fs.nodes[at].size] = '\0';
    return buf;
}

static int open_handles(void)
{
    int n = 0;
    for (int i = 0; i < FAKE_MAX_HANDLES; i++) {
        if (g_fs.handles[i].used) {
            n++;
        }
    }
    return n;
}

/* ---------------------------------------------------------------------- */
/* Paths                                                                 */
/* ---------------------------------------------------------------------- */

static void test_join(void)
{
    char out[64];

    fsops_join("/a", "b", out, sizeof(out));
    AG_CHECK_STR(out, "/a/b");

    /* One separator, whichever end brought it - or both. */
    fsops_join("/a/", "b", out, sizeof(out));
    AG_CHECK_STR(out, "/a/b");
    fsops_join("/a", "/b", out, sizeof(out));
    AG_CHECK_STR(out, "/a/b");
    fsops_join("/a/", "/b", out, sizeof(out));
    AG_CHECK_STR(out, "/a/b");

    /* The root keeps its slash, because "" is not a path. */
    fsops_join("/", "b", out, sizeof(out));
    AG_CHECK_STR(out, "/b");

    /* Too long truncates rather than running over, and the caller finds out
     * by the result not being what it asked for. */
    char small[6];
    fsops_join("/aaa", "bbbb", small, sizeof(small));
    AG_CHECK_INT((int)strlen(small), 5);
}

static void test_is_dot(void)
{
    AG_CHECK(fsops_is_dot("."));
    AG_CHECK(fsops_is_dot(".."));
    AG_CHECK(!fsops_is_dot(".config"));
    AG_CHECK(!fsops_is_dot("..a"));
    AG_CHECK(!fsops_is_dot("a"));
}

/* ---------------------------------------------------------------------- */
/* Measuring                                                             */
/* ---------------------------------------------------------------------- */

static void test_measure(void)
{
    tree();
    uint64_t bytes = 0;
    uint32_t files = 0;
    AG_CHECK_INT(fsops_measure(req(), "/a", &bytes, &files), AG_OK);
    /* "one" 3 + "two-two-two-two" 15 + "three" 5 */
    AG_CHECK_INT((int)bytes, 23);
    AG_CHECK_INT((int)files, 3);

    /* A single file measures as itself, not as nothing. */
    AG_CHECK_INT(fsops_measure(req(), "/a/two.txt", &bytes, &files), AG_OK);
    AG_CHECK_INT((int)bytes, 15);
    AG_CHECK_INT((int)files, 1);

    AG_CHECK_INT(fsops_measure(req(), "/nope", &bytes, &files), -AG_ENOENT);
}

/* ---------------------------------------------------------------------- */
/* Copying                                                              */
/* ---------------------------------------------------------------------- */

static void test_copy_one_file(void)
{
    tree();
    AG_CHECK_INT(fsops_copy(req(), "/a/two.txt", "/b/two.txt"), AG_OK);
    AG_CHECK_STR(contents("/b/two.txt"), "two-two-two-two");
    /* The source is still there: this is a copy. */
    AG_CHECK_STR(contents("/a/two.txt"), "two-two-two-two");
    /* Nothing left open, whatever happened inside. */
    AG_CHECK_INT(open_handles(), 0);
}

static void test_copy_tree(void)
{
    tree();
    AG_CHECK_INT(fsops_copy(req(), "/a", "/b/a"), AG_OK);

    AG_CHECK(exists("/b/a"));
    AG_CHECK(exists("/b/a/sub"));
    AG_CHECK_STR(contents("/b/a/one.txt"), "one");
    AG_CHECK_STR(contents("/b/a/two.txt"), "two-two-two-two");
    AG_CHECK_STR(contents("/b/a/sub/three.txt"), "three");
    /* And the original is untouched. */
    AG_CHECK_STR(contents("/a/sub/three.txt"), "three");
    AG_CHECK_INT(open_handles(), 0);
}

/*
 * The whole point of one handle at a time.  A tree three deep must not need
 * three directory handles, because the system has a couple of dozen in total
 * and running out arrives in the middle of a copy.
 */
static void test_copy_holds_one_dir_handle(void)
{
    tree();
    (void)add_node("/a/sub/deep", true, NULL);
    (void)add_node("/a/sub/deep/four.txt", false, "four");

    AG_CHECK_INT(fsops_copy(req(), "/a", "/b/a"), AG_OK);
    AG_CHECK_STR(contents("/b/a/sub/deep/four.txt"), "four");

    /*
     * Reopening a parent on the way back is the price of that, and it is
     * bounded: one per directory returned from, not one per entry.  Four
     * directories are walked (/a, /a/sub, /a/sub/deep, and /a reopened twice
     * on the way back up) - the assertion is that it is a handful, not a
     * hundred.
     */
    AG_CHECK(g_fs.opendirs <= 8);
    AG_CHECK_INT(open_handles(), 0);
}

/* A directory into itself, or into a directory inside itself, would copy for
 * ever.  Both are refused, and the second is the one people actually type. */
static void test_copy_into_itself_is_refused(void)
{
    tree();
    AG_CHECK_INT(fsops_copy(req(), "/a", "/a"), -AG_EINVAL);
    AG_CHECK_INT(fsops_copy(req(), "/a", "/a/sub/a"), -AG_EINVAL);
    /* "/ab" is not inside "/a", and the prefix test must not think it is. */
    AG_CHECK_INT(fsops_copy(req(), "/a", "/ab"), AG_OK);
}

/*
 * A failed write must not leave a file that looks like a copy.
 *
 * This is the failure worth a test of its own: everything about a truncated
 * file is right except its contents, and nothing later will notice.
 */
static void test_failed_copy_leaves_no_half_file(void)
{
    tree();
    fsops_req_t *r = req();
    g_fs.fail_call = "write";
    g_fs.fail_after = 1; /* the first chunk lands, the second does not */
    g_fs.fail_with = -AG_ENOSPC;

    AG_CHECK_INT(fsops_copy(r, "/a/two.txt", "/b/two.txt"), -AG_ENOSPC);
    AG_CHECK(!exists("/b/two.txt"));
    AG_CHECK_INT(open_handles(), 0);
}

/* Stopping a tree copy leaves what was already copied, and no half file. */
static void test_cancelled_copy_keeps_what_it_finished(void)
{
    tree();
    fsops_req_t *r = req();
    g_fs.tick_stop = 3; /* somewhere inside the first files */

    AG_CHECK_INT(fsops_copy(r, "/a", "/b/a"), -AG_EKILLED);
    AG_CHECK(exists("/b/a"));
    /* Whatever it managed, no file may exist with the wrong contents. */
    for (int i = 0; i < FAKE_MAX_NODES; i++) {
        const node_t *n = &g_fs.nodes[i];
        if (!n->used || n->is_dir) {
            continue;
        }
        if (strncmp(n->path, "/b/a/", 5) != 0) {
            continue;
        }
        char src[128];
        snprintf(src, sizeof(src), "/a/%s", &n->path[5]);
        AG_CHECK_STR(contents(n->path), contents(src));
    }
    AG_CHECK_INT(open_handles(), 0);
}

/*
 * A directory that answers an error partway is not a directory that has
 * ended.  Everything that reads a listing in this tree treats any non-OK as
 * the end, and for a listing that is only a short list; for a copy it is a
 * copy that stops at entry two of three and says it worked.
 */
static void test_readdir_error_is_not_the_end(void)
{
    tree();
    fsops_req_t *r = req();
    g_fs.fail_call = "readdir";
    g_fs.fail_after = 2;
    g_fs.fail_with = -AG_EIO;

    AG_CHECK_INT(fsops_copy(r, "/a", "/b/a"), -AG_EIO);
    AG_CHECK_INT(open_handles(), 0);
}

/* Copying onto a tree that partly exists is a normal request, not an error. */
static void test_copy_over_existing_directories(void)
{
    tree();
    (void)add_node("/b/a", true, NULL);
    (void)add_node("/b/a/sub", true, NULL);

    AG_CHECK_INT(fsops_copy(req(), "/a", "/b/a"), AG_OK);
    AG_CHECK_STR(contents("/b/a/sub/three.txt"), "three");
}

/* The bar: a listing with no sizes in it still gets a total per file, because
 * something asks.  And the totals from the request come back untouched. */
static void test_progress_has_a_size_even_without_readdir_sizes(void)
{
    tree();
    fsops_req_t *r = req();
    r->total_bytes = 23;
    r->files_total = 3;

    AG_CHECK_INT(fsops_copy(r, "/a", "/b/a"), AG_OK);
    AG_CHECK_INT((int)g_fs.last.total_bytes, 23);
    AG_CHECK_INT((int)g_fs.last.files_total, 3);
    AG_CHECK_INT((int)g_fs.last.total_done, 23);
    AG_CHECK_INT((int)g_fs.last.files_done, 3);
}

/* ---------------------------------------------------------------------- */
/* Deleting                                                              */
/* ---------------------------------------------------------------------- */

static void test_delete_file_and_tree(void)
{
    tree();
    AG_CHECK_INT(fsops_delete(req(), "/a/one.txt"), AG_OK);
    AG_CHECK(!exists("/a/one.txt"));
    AG_CHECK(exists("/a/two.txt"));

    AG_CHECK_INT(fsops_delete(req(), "/a"), AG_OK);
    AG_CHECK(!exists("/a"));
    AG_CHECK(!exists("/a/sub"));
    AG_CHECK(!exists("/a/sub/three.txt"));
    /* And nothing outside it. */
    AG_CHECK(exists("/b"));
    AG_CHECK_INT(open_handles(), 0);
}

/*
 * Children before the directory.  The fake refuses to remove a directory with
 * anything in it, exactly as littlefs and FAT do, so an order that removed the
 * parent first would fail here rather than passing quietly and failing on a
 * board.
 */
static void test_delete_is_children_first(void)
{
    tree();
    AG_CHECK_INT(fsops_delete(req(), "/a"), AG_OK);
    AG_CHECK_INT(g_fs.rmdirs, 2); /* /a/sub and /a, and nothing retried */
}

/* A stop halfway leaves a smaller tree, not a broken one. */
static void test_cancelled_delete_leaves_a_smaller_tree(void)
{
    tree();
    fsops_req_t *r = req();
    g_fs.tick_stop = 1;

    AG_CHECK_INT(fsops_delete(r, "/a"), -AG_EKILLED);
    AG_CHECK(exists("/a"));
    AG_CHECK_INT(open_handles(), 0);
}

/* ---------------------------------------------------------------------- */
/* Moving                                                                */
/* ---------------------------------------------------------------------- */

static void test_move_renames_when_it_can(void)
{
    tree();
    AG_CHECK_INT(fsops_move(req(), "/a/one.txt", "/b/one.txt"), AG_OK);
    AG_CHECK_STR(contents("/b/one.txt"), "one");
    AG_CHECK(!exists("/a/one.txt"));
    /* One rename and no copying at all. */
    AG_CHECK_INT(g_fs.renames, 1);
    AG_CHECK_INT(g_fs.writes, 0);
}

static void test_move_across_filesystems_copies_then_deletes(void)
{
    tree();
    fsops_req_t *r = req();
    g_fs.no_rename = true;

    AG_CHECK_INT(fsops_move(r, "/a", "/b/a"), AG_OK);
    AG_CHECK_STR(contents("/b/a/sub/three.txt"), "three");
    AG_CHECK(!exists("/a"));
    AG_CHECK(!exists("/a/sub/three.txt"));
}

/*
 * A move that could not finish must leave the source alone.  This is what
 * makes a move safe to interrupt, and the opposite - delete first, or delete
 * what was copied - loses the file.
 */
static void test_failed_move_keeps_the_source(void)
{
    tree();
    fsops_req_t *r = req();
    g_fs.no_rename = true;
    g_fs.tick_stop = 2;

    AG_CHECK_INT(fsops_move(r, "/a", "/b/a"), -AG_EKILLED);
    AG_CHECK(exists("/a"));
    AG_CHECK_STR(contents("/a/two.txt"), "two-two-two-two");
    AG_CHECK_STR(contents("/a/sub/three.txt"), "three");
}

/* ---------------------------------------------------------------------- */
/* Bounds                                                                */
/* ---------------------------------------------------------------------- */

/* A tree deeper than the walk allows stops with an error, not with a
 * corrupted path or a smashed stack. */
static void test_too_deep_is_an_error(void)
{
    memset(&g_fs, 0, sizeof(g_fs));
    (void)add_node("/d", true, NULL);
    char path[128] = "/d";
    for (int i = 0; i < FSOPS_MAX_DEPTH + 2; i++) {
        snprintf(path + strlen(path), sizeof(path) - strlen(path), "/x");
        (void)add_node(path, true, NULL);
    }
    (void)add_node("/t", true, NULL);

    AG_CHECK_INT(fsops_copy(req(), "/d", "/t/d"), -AG_ERANGE);
    AG_CHECK_INT(open_handles(), 0);
}

/* A request with nothing to copy with is refused before it opens anything. */
static void test_no_buffer_is_refused(void)
{
    tree();
    fsops_req_t *r = req();
    r->chunk = NULL;
    AG_CHECK_INT(fsops_copy(r, "/a/one.txt", "/b/one.txt"), -AG_EINVAL);
    AG_CHECK_INT(g_fs.opens, 0);
}

void run_fsops_tests(void)
{
    test_join();
    test_is_dot();
    test_measure();
    test_copy_one_file();
    test_copy_tree();
    test_copy_holds_one_dir_handle();
    test_copy_into_itself_is_refused();
    test_failed_copy_leaves_no_half_file();
    test_cancelled_copy_keeps_what_it_finished();
    test_readdir_error_is_not_the_end();
    test_copy_over_existing_directories();
    test_progress_has_a_size_even_without_readdir_sizes();
    test_delete_file_and_tree();
    test_delete_is_children_first();
    test_cancelled_delete_leaves_a_smaller_tree();
    test_move_renames_when_it_can();
    test_move_across_filesystems_copies_then_deletes();
    test_failed_move_keeps_the_source();
    test_too_deep_is_an_error();
    test_no_buffer_is_refused();
}
