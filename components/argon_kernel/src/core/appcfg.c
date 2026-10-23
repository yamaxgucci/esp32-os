/*
 * ArgonOS - where an application keeps what it remembers.  See appcfg.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "appcfg.h"

#include <string.h>

#include <argon/log.h>
#include <argon/path.h>
#include <argon/proc.h>
#include <argon/vfs.h>

#include "appcfg_text.h"
#include "proc/proc_internal.h"

#define TAG "appcfg"

/*
 * Two static buffers rather than stack ones: the caller is an application's own
 * task, and how much stack it has is the application's business.  AG_APPCFG_TEXT_MAX
 * is 1 KB each - a settings file that outgrows it is holding something other
 * than settings, and the rewrite refuses rather than truncating.
 */
static char s_old[AG_APPCFG_TEXT_MAX];
static char s_new[AG_APPCFG_TEXT_MAX];

static const char *app_name(void)
{
    const proc_t *p = ag_proc_current();
    /* A built-in has a name too and may keep settings; the kernel itself has
     * none, and gets the fallback file in appcfg_text.c. */
    return (p != NULL) ? p->name : "";
}

ag_err_t ag_appcfg_dos_path(const char *app, char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return -AG_EINVAL;
    }
    char name[AG_APPCFG_KEY_MAX];
    ag_appcfg_file_name(app, name, sizeof(name));
    char tail[AG_APPCFG_KEY_MAX + 8];
    size_t at = 0;
    for (const char *p = name; *p != '\0' && at + 1u < sizeof(tail); p++) {
        tail[at++] = *p;
    }
    for (const char *p = ".CFG"; *p != '\0' && at + 1u < sizeof(tail); p++) {
        tail[at++] = *p;
    }
    tail[at] = '\0';

    /* `C:\CFG\NAME.CFG`, the way a person would type it: the file the kernel
     * opens is /sys/CFG/NAME.CFG, and both names are built from the same two
     * pieces so they cannot drift apart. */
    size_t out_at = 0;
    for (const char *p = "C:\\CFG\\"; *p != '\0' && out_at + 1u < len; p++) {
        out[out_at++] = *p;
    }
    for (const char *p = tail; *p != '\0' && out_at + 1u < len; p++) {
        out[out_at++] = *p;
    }
    out[out_at] = '\0';
    return AG_OK;
}

/* The kernel's own address for the same file, and for its temporary. */
static void app_path(const char *app, bool temp, char *out, size_t len)
{
    char name[AG_APPCFG_KEY_MAX];
    ag_appcfg_file_name(app, name, sizeof(name));
    size_t at = 0;
    const char *const dir = AG_APPCFG_DIR "/";
    for (const char *p = dir; *p != '\0' && at + 1u < len; p++) {
        out[at++] = *p;
    }
    for (const char *p = name; *p != '\0' && at + 1u < len; p++) {
        out[at++] = *p;
    }
    for (const char *p = temp ? ".TMP" : ".CFG"; *p != '\0' && at + 1u < len;
         p++) {
        out[at++] = *p;
    }
    out[at] = '\0';
}

/* Reads the whole file into s_old.  Zero when there is no file yet, which is
 * every program's first run. */
static size_t read_text(const char *app)
{
    char path[AG_PATH_MAX];
    app_path(app, false, path, sizeof(path));
    const ag_handle_t h = ag_vfs_open(path, NULL, AG_O_RDONLY);
    if (h < 0) {
        s_old[0] = '\0';
        return 0;
    }
    size_t at = 0;
    while (at + 1u < sizeof(s_old)) {
        const int32_t n = ag_vfs_read(h, s_old + at, sizeof(s_old) - 1u - at);
        if (n <= 0) {
            break;
        }
        at += (size_t)n;
    }
    (void)ag_vfs_close(h);
    s_old[at] = '\0';
    return at;
}

ag_err_t ag_appcfg_get(const char *key, char *out, size_t len)
{
    if (key == NULL || out == NULL || len == 0) {
        return -AG_EINVAL;
    }
    if (read_text(app_name()) == 0) {
        return -AG_ENOENT;
    }
    return ag_appcfg_text_get(s_old, key, out, len) ? AG_OK : -AG_ENOENT;
}

int32_t ag_appcfg_get_int(const char *key, int32_t fallback)
{
    if (key == NULL) {
        return fallback;
    }
    if (read_text(app_name()) == 0) {
        return fallback;
    }
    return ag_appcfg_text_get_int(s_old, key, fallback);
}

ag_err_t ag_appcfg_set(const char *key, const char *value)
{
    if (key == NULL || value == NULL) {
        return -AG_EINVAL;
    }
    const char *const app = app_name();
    (void)read_text(app);

    const size_t len = ag_appcfg_text_set(s_old, key, value, s_new,
                                          sizeof(s_new));
    if (len == 0) {
        ag_log(AG_LOG_WARN, TAG,
               "%s = %s does not fit in %u bytes; the file is left alone", key,
               value, (unsigned)sizeof(s_new));
        return -AG_ENOSPC;
    }

    const ag_err_t made = ag_vfs_mkdir(AG_APPCFG_DIR, NULL);
    if (made != AG_OK && made != -AG_EEXIST) {
        ag_log(AG_LOG_WARN, TAG, "%s: %d", AG_APPCFG_DIR, (int)made);
        return made;
    }

    char path[AG_PATH_MAX];
    char temp[AG_PATH_MAX];
    app_path(app, false, path, sizeof(path));
    app_path(app, true, temp, sizeof(temp));

    const ag_handle_t h = ag_vfs_open(temp, NULL,
                                      AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        return (ag_err_t)h;
    }
    if (ag_vfs_write(h, s_new, len) != (int32_t)len) {
        (void)ag_vfs_close(h);
        (void)ag_vfs_unlink(temp, NULL);
        return -AG_EIO;
    }
    (void)ag_vfs_close(h);

    /* Through a temporary, and renamed at the end: a power cut in the middle of
     * a settings write leaves the old file, not half of the new one.
     *
     * A rename over a file that is already there is not what every backend
     * does.  littlefs answers -AG_EEXIST rather than replacing the target, so
     * the *first* write of a settings file worked and every later one failed:
     * `install.dir` was remembered once and never changed again, and the shop
     * printed "cannot remember that (already exists)" the second time it was
     * asked.  Found on the S3-CAM on 23 Oct 2026 (the earlier warning in the
     * same log, `/sys/CFG/STORE.CFG: -17`, was this from the very first
     * `-web`).  So: replace, and only when the backend refuses to, remove the
     * old file and try again - the window in which a power cut costs the file
     * is the length of one syscall, and the common path keeps its atomicity. */
    ag_err_t err = ag_vfs_rename(temp, path, NULL);
    if (err == -AG_EEXIST) {
        (void)ag_vfs_unlink(path, NULL);
        err = ag_vfs_rename(temp, path, NULL);
    }
    if (err != AG_OK) {
        (void)ag_vfs_unlink(temp, NULL);
        ag_log(AG_LOG_WARN, TAG, "%s: %d", path, (int)err);
    }
    return err;
}
