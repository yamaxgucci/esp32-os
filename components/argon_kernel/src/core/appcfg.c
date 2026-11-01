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
#include <argon/port/mem.h>

#include "appcfg_text.h"
#include "proc/proc_internal.h"

#define TAG "appcfg"

/*
 * The file's text, old and new, in a block taken for the call rather than on
 * the stack: the caller is an application's own task, and how much stack it
 * has is the application's business.  AG_APPCFG_TEXT_MAX is 1 KB each - a
 * settings file that outgrows it is holding something other than settings,
 * and the rewrite refuses rather than truncating.
 *
 * Not static: 2 KB of .bss for a call made a few times in a program's life
 * pushed the S3's QEMU image 832 bytes past its DRAM when the shop's line met
 * main (1 Nov 2026) - the same budget cmd_fs.c's copy buffer gave back.  PSRAM
 * first where there is any.  It also lets two programs ask at once.
 */
static char *text_alloc(void)
{
    char *p = ag_port_alloc(2u * AG_APPCFG_TEXT_MAX, AG_MEM_SLOW | AG_MEM_BYTE);
    if (p == NULL) {
        p = ag_port_alloc(2u * AG_APPCFG_TEXT_MAX, AG_MEM_FAST | AG_MEM_BYTE);
    }
    return p;
}

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

/* Reads the whole file into `old` (AG_APPCFG_TEXT_MAX).  Zero when there is
 * no file yet, which is every program's first run. */
static size_t read_text(const char *app, char *old)
{
    char path[AG_PATH_MAX];
    app_path(app, false, path, sizeof(path));
    const ag_handle_t h = ag_vfs_open(path, NULL, AG_O_RDONLY);
    if (h < 0) {
        old[0] = '\0';
        return 0;
    }
    size_t at = 0;
    while (at + 1u < AG_APPCFG_TEXT_MAX) {
        const int32_t n = ag_vfs_read(h, old + at, AG_APPCFG_TEXT_MAX - 1u - at);
        if (n <= 0) {
            break;
        }
        at += (size_t)n;
    }
    (void)ag_vfs_close(h);
    old[at] = '\0';
    return at;
}

ag_err_t ag_appcfg_get(const char *key, char *out, size_t len)
{
    if (key == NULL || out == NULL || len == 0) {
        return -AG_EINVAL;
    }
    char *const text = text_alloc();
    if (text == NULL) {
        return -AG_ENOMEM;
    }
    ag_err_t err = -AG_ENOENT;
    if (read_text(app_name(), text) != 0 &&
        ag_appcfg_text_get(text, key, out, len)) {
        err = AG_OK;
    }
    ag_port_free(text);
    return err;
}

int32_t ag_appcfg_get_int(const char *key, int32_t fallback)
{
    if (key == NULL) {
        return fallback;
    }
    char *const text = text_alloc();
    if (text == NULL) {
        return fallback;
    }
    int32_t v = fallback;
    if (read_text(app_name(), text) != 0) {
        v = ag_appcfg_text_get_int(text, key, fallback);
    }
    ag_port_free(text);
    return v;
}

static ag_err_t write_text(const char *app, const char *text, size_t len);

ag_err_t ag_appcfg_set(const char *key, const char *value)
{
    if (key == NULL || value == NULL) {
        return -AG_EINVAL;
    }
    char *const old = text_alloc();
    if (old == NULL) {
        return -AG_ENOMEM;
    }
    char *const new_text = old + AG_APPCFG_TEXT_MAX;
    const char *const app = app_name();
    (void)read_text(app, old);

    const size_t len = ag_appcfg_text_set(old, key, value, new_text,
                                          AG_APPCFG_TEXT_MAX);
    ag_err_t err;
    if (len == 0) {
        ag_log(AG_LOG_WARN, TAG,
               "%s = %s does not fit in %u bytes; the file is left alone", key,
               value, (unsigned)AG_APPCFG_TEXT_MAX);
        err = -AG_ENOSPC;
    } else {
        err = write_text(app, new_text, len);
    }
    ag_port_free(old);
    return err;
}

/* The new text to the program's file, through a temporary. */
static ag_err_t write_text(const char *app, const char *text, size_t len)
{

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
    if (ag_vfs_write(h, text, len) != (int32_t)len) {
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
