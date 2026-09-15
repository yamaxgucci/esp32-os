/*
 * ArgonOS - file operations: copy, move, delete, and measuring first.
 *
 * One copy of the answers to "what does copying a directory mean", shared by
 * the file manager and the desktop shell.  Two managers with two answers is
 * two behaviours on "the file is open", "the disk is full" and "half of it
 * went" - and the half-written file is the one nobody notices until later.
 *
 * There is no user interface here and no console.  Everything the caller sees
 * comes through the progress callback, and everything this reaches for goes
 * through the table below - so the walk, which is the part with the bugs in
 * it, compiles and is tested on the host against a directory tree that only
 * exists in memory.  Same split as the desktop's painter, for the same reason.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_FSOPS_H
#define ARGON_FSOPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Types and error codes only - no g_ag_api, so this half builds on a host. */
#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How deep a tree may be.
 *
 * Not a guess about filesystems: it is the bound that makes the walk take a
 * fixed, small amount of stack, and something has to stop a link loop or a
 * path that has stopped making sense.  A tree deeper than this stops with
 * -AG_ERANGE naming the directory it stopped at, which is a thing a person can
 * act on.
 */
#define FSOPS_MAX_DEPTH 16

/*
 * Everything the operations reach for.
 *
 * These are the ArgonOS calls with a context added, and nothing else - no
 * allocation (the caller lends the copy buffer), no console, no clock.  The
 * real one is fsops_argon_fs(); the tests pass a tree held in memory.
 */
typedef struct {
    ag_handle_t (*open)(void *ctx, const char *path, uint32_t flags);
    int32_t (*read)(void *ctx, ag_handle_t h, void *buf, uint32_t len);
    int32_t (*write)(void *ctx, ag_handle_t h, const void *buf, uint32_t len);
    ag_err_t (*sync)(void *ctx, ag_handle_t h);
    ag_err_t (*close)(void *ctx, ag_handle_t h);

    ag_err_t (*stat)(void *ctx, const char *path, ag_stat_t *out);

    ag_handle_t (*opendir)(void *ctx, const char *path);
    /* AG_OK with an entry, or an error; -AG_ENOENT means the end. */
    ag_err_t (*readdir)(void *ctx, ag_handle_t h, ag_dirent_t *out);
    ag_err_t (*closedir)(void *ctx, ag_handle_t h);

    ag_err_t (*mkdir)(void *ctx, const char *path);
    ag_err_t (*unlink)(void *ctx, const char *path);
    ag_err_t (*rmdir)(void *ctx, const char *path);
    ag_err_t (*rename)(void *ctx, const char *from, const char *to);
} fsops_fs_t;

/* The real table, over argon.h.  Defined in fsops_argon.c. */
const fsops_fs_t *fsops_argon_fs(void);

/*
 * What the caller is told while an operation runs.
 *
 * Both a per-file and a whole-operation figure, because a copy of one big file
 * and a copy of a thousand small ones want different bars and the caller knows
 * which it is looking at.  A total of zero means "not measured": the walk does
 * not measure on its own, because on HostFS walking the tree twice is a
 * second's wait before anything appears to happen.
 */
typedef struct {
    const char *path; /* what is being worked on right now */
    uint64_t    file_done;
    uint64_t    file_total; /* 0 when the filesystem would not say */
    uint64_t    total_done;
    uint64_t    total_bytes; /* what was put in the request, or 0 */
    uint32_t    files_done;
    uint32_t    files_total; /* what was put in the request, or 0 */
} fsops_progress_t;

/*
 * Called as the work goes on.  Returning false stops the operation, which then
 * returns -AG_EKILLED - so this is both the progress bar and the Esc key.
 *
 * Called at least once per file and every FSOPS_TICK_BYTES within a big one;
 * it must be cheap and must not touch the filesystem.
 */
typedef bool (*fsops_tick_fn)(void *ctx, const fsops_progress_t *p);

#define FSOPS_TICK_BYTES (64u * 1024u)

/*
 * One request.  `chunk` is the caller's copy buffer: the size decides how many
 * calls a copy costs, the caller decides where the memory comes from, and this
 * module never allocates.
 */
typedef struct {
    const fsops_fs_t *fs;
    void             *fs_ctx;
    fsops_tick_fn     tick; /* may be NULL */
    void             *tick_ctx;
    char             *chunk;
    uint32_t          chunk_len;
    /* Filled in from fsops_measure when a real bar is wanted; 0 otherwise. */
    uint64_t total_bytes;
    uint32_t files_total;
} fsops_req_t;

/*
 * Bytes and files under `path`, for a progress bar that means something.
 * Counts the file itself when `path` is one.  Cheap in memory, not in time:
 * it is a whole extra walk, so ask for it when the wait is worth the bar.
 */
ag_err_t fsops_measure(fsops_req_t *req, const char *path, uint64_t *bytes,
                       uint32_t *files);

/*
 * Copy.  A file to a file, or a directory onto a directory that is created
 * along with everything under it.
 *
 * `to` is the finished name, not the directory to put it in - the caller has
 * already decided that, because "copy A to B" where B is an existing directory
 * means B/A and where it is not means B, and that decision belongs where the
 * user typed it.
 *
 * A copy that fails partway leaves the file it was writing deleted, not half
 * written: a truncated file that looks like a copy is the worst of the
 * outcomes.  Directories and files already finished are left where they are.
 */
ag_err_t fsops_copy(fsops_req_t *req, const char *from, const char *to);

/*
 * Move.  A rename when the two are on one filesystem, and a copy followed by a
 * delete when they are not - which is what the user meant either way, and the
 * only case where this module decides something on its own rather than asking.
 *
 * The delete happens only after the copy has finished with AG_OK.  A cancelled
 * or failed move leaves the source untouched.
 */
ag_err_t fsops_move(fsops_req_t *req, const char *from, const char *to);

/*
 * Delete, including a directory with things in it - children first, then the
 * directory, so a stop halfway leaves a smaller tree rather than a broken one.
 */
ag_err_t fsops_delete(fsops_req_t *req, const char *path);

/*
 * Joins a directory and a name with one separator, whatever the two ends carry.
 * Here rather than in each caller because a path built two ways is two bugs.
 */
void fsops_join(const char *dir, const char *name, char *out, size_t len);

/* True for "." and "..", which a walk must never follow. */
bool fsops_is_dot(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_FSOPS_H */
