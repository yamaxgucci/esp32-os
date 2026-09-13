/*
 * ArgonOS - the syscall table.
 *
 * Nothing here does any work: every entry forwards to the subsystem that owns
 * the job.  That is the point of the table - it is the contract, and keeping it
 * free of logic is what makes it cheap to keep stable.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <argon/session.h>
#include <argon/shell.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <argon/journal.h>

#include <argon/codepage.h>
#include <argon/console.h>
#include <argon/devfs.h>
#include <argon/device.h>
#include <argon/audio.h>
#include <argon/display.h>
#include <argon/hostfs.h>
#include <argon/input.h>
#include <argon/kernel.h>
#include <argon/keys.h>
#include <argon/cfg.h>
#include <argon/loader.h>
#include <argon/log.h>
#include <argon/module.h>
#include <argon/net.h>
#include <argon/power.h>
#include <argon/proc.h>
#include <argon/shell.h>
#include <argon/filemap.h>
#include <argon/vfs.h>

#include "core/sysconfig.h"

#include <argon/port/config.h>

#include <argon/port/ble.h>
#include <argon/port/bt.h>
#include <argon/port/time.h>
#include <argon/port/task.h>
#include <argon/port/wifi.h>
#include <argon/port/camera.h>

#include "dev/io.h"
#include "net/espnow.h"
#include "net/wifimon.h"

/* ---------------------------------------------------------------------- */
/* sys                                                                    */
/* ---------------------------------------------------------------------- */

static void api_info(ag_sysinfo_t *out)
{
    if (out != NULL) {
        *out = *ag_sysinfo();
    }
}

static void api_log(ag_log_level_t level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ag_vlog(level, (tag != NULL) ? tag : "app", fmt, ap);
    va_end(ap);
}

static void api_vlog(ag_log_level_t level, const char *tag, const char *fmt,
                     va_list ap)
{
    ag_vlog(level, (tag != NULL) ? tag : "app", fmt, ap);
}

static void api_exit(int code) { ag_proc_exit(code); }

static void api_panic(const char *msg)
{
    ag_log(AG_LOG_ERROR, "app", "panic: %s", (msg != NULL) ? msg : "(no reason)");
    ag_proc_exit(-1);
}

static void *api_sym(const char *name)
{
    (void)name;
    return NULL; /* no optional entry points yet */
}

static const char *api_strerror(ag_err_t err)
{
    switch (-err) {
    case AG_OK:        return "ok";
    case AG_EPERM:     return "not permitted";
    case AG_ENOENT:    return "not found";
    case AG_EINTR:     return "interrupted";
    case AG_EIO:       return "input/output error";
    case AG_EBADF:     return "bad handle";
    case AG_EAGAIN:    return "try again";
    case AG_ENOMEM:    return "out of memory";
    case AG_EACCES:    return "access denied";
    case AG_EBUSY:     return "busy";
    case AG_EEXIST:    return "already exists";
    case AG_ENODEV:    return "no such device";
    case AG_ENOTDIR:   return "not a directory";
    case AG_EISDIR:    return "is a directory";
    case AG_EINVAL:    return "invalid argument";
    case AG_ENFILE:    return "too many open files";
    case AG_ENOSPC:    return "no space left";
    case AG_EROFS:     return "write protected";
    case AG_ERANGE:    return "out of range";
    case AG_ENOSYS:    return "not implemented";
    case AG_ENOTSUP:   return "not supported";
    case AG_ETIMEDOUT: return "timed out";
    case AG_EABI:      return "ABI mismatch";
    case AG_EFORMAT:   return "malformed image";
    case AG_EKILLED:   return "terminated";
    default:           return "error";
    }
}

static void api_heartbeat(void) { ag_proc_heartbeat(); }

static void api_module_on_unload(void (*fn)(void))
{
    ag_module_on_unload(fn);
}

/* Defined below, next to the other session-facing calls. */
static int32_t api_prompt_in_slot(int slot, const char *cwd);

static const ag_sys_api_t k_sys = {
    .size = sizeof(ag_sys_api_t),
    .info = api_info,
    .log = api_log,
    .vlog = api_vlog,
    .exit = api_exit,
    .panic = api_panic,
    .sym = api_sym,
    .strerror = api_strerror,
    .heartbeat = api_heartbeat,
    .module_on_unload = api_module_on_unload,
    .prompt_in_slot = api_prompt_in_slot,
};

/* ---------------------------------------------------------------------- */
/* mem                                                                    */
/* ---------------------------------------------------------------------- */

/*
 * Memory comes from the calling process's arena, so that ending the process
 * gives all of it back at once and no application can fragment the kernel's
 * heap.  The process layer is where that lives; this is forwarding, as
 * everything in this file should be.
 */
static void *api_alloc(size_t bytes) { return ag_proc_alloc(bytes, 0); }

static void *api_alloc_caps(size_t bytes, uint32_t caps)
{
    return ag_proc_alloc(bytes, caps);
}

static void *api_realloc(void *ptr, size_t bytes)
{
    return ag_proc_realloc(ptr, bytes);
}

static void api_free(void *ptr) { ag_proc_free(ptr); }

static size_t api_usable_size(const void *ptr)
{
    return ag_proc_usable_size(ptr);
}

static void api_meminfo(ag_meminfo_t *out) { ag_proc_meminfo(out); }

static const ag_mem_api_t k_mem = {
    .size = sizeof(ag_mem_api_t),
    .alloc = api_alloc,
    .alloc_caps = api_alloc_caps,
    .realloc = api_realloc,
    .free = api_free,
    .usable_size = api_usable_size,
    .info = api_meminfo,
};

/* ---------------------------------------------------------------------- */
/* con                                                                    */
/* ---------------------------------------------------------------------- */

/*
 * Console I/O belongs to the focused session process.  One shared text screen:
 * a background app must not paint over the focused slot (FM/shell).  Its
 * stdout still goes to the journal (`log`) so `run /b` progress is not lost.
 * Kernel/shell call ag_console_* directly and are unaffected.
 */
static bool has_console_focus(void)
{
    const ag_pid_t me = ag_proc_self();
    return me == AG_PID_KERNEL || ag_proc_focused();
}

static void bg_stdout_write(const char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    const ag_pid_t me = ag_proc_self();
    char           name[32] = "app";
    for (uint32_t i = 0;; i++) {
        ag_procinfo_t info;
        if (ag_proc_info(i, &info) != AG_OK) {
            break;
        }
        if (info.pid == me) {
            memcpy(name, info.name, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            break;
        }
    }
    ag_log_app_write(me, name, buf, len);
}

static int32_t api_con_write(const char *buf, size_t len)
{
    if (!has_console_focus()) {
        bg_stdout_write(buf, len);
        return (int32_t)len;
    }
    ag_console_write(buf, len);
    return (int32_t)len;
}

static int32_t api_con_puts(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    if (!has_console_focus()) {
        bg_stdout_write(s, strlen(s));
        if (s[0] != '\0' && s[strlen(s) - 1] != '\n') {
            bg_stdout_write("\n", 1);
        }
        return (int32_t)strlen(s);
    }
    ag_console_puts(s);
    return (int32_t)strlen(s);
}

static int32_t api_con_vprintf(const char *fmt, va_list ap)
{
    if (!has_console_focus()) {
        char buf[AG_JOURNAL_LINE_MAX];
        const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        if (n <= 0) {
            return n;
        }
        const size_t len =
            ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
        bg_stdout_write(buf, len);
        return n;
    }
    return ag_console_vprintf(fmt, ap);
}

static int32_t api_con_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int32_t n = api_con_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

static bool has_the_keyboard(void) { return has_console_focus(); }

static int32_t api_getch(void)
{
    if (!has_the_keyboard()) {
        return -AG_EPERM;
    }
    return ag_console_getch(UINT32_MAX);
}

/*
 * Looks without taking: the idiom this exists for is `if (kbhit()) getch()`, and
 * a version that consumed the event would throw away the key it reported.  It
 * answers "something is waiting", not "how many" - and an event carrying no
 * character counts as something, because deciding that here would mean taking
 * the event to look at it.
 */
static int32_t api_kbhit(void)
{
    if (!has_the_keyboard()) {
        return 0;
    }
    return ag_console_peek_event(NULL) ? 1 : 0;
}

static int32_t api_readline(char *buf, size_t len)
{
    if (!has_the_keyboard()) {
        return -AG_EPERM;
    }
    return ag_console_readline(buf, len);
}

static void api_cls(void)
{
    if (!has_console_focus()) {
        return;
    }
    ag_console_lock();
    ag_screen_cls(ag_console_screen());
    ag_console_unlock();
}

static void api_gotoxy(uint16_t x, uint16_t y)
{
    if (!has_console_focus()) {
        return;
    }
    ag_console_lock();
    ag_screen_gotoxy(ag_console_screen(), x, y);
    ag_console_unlock();
}

static void api_set_attr(uint8_t attr)
{
    if (!has_console_focus()) {
        return;
    }
    ag_console_lock();
    ag_screen_set_attr(ag_console_screen(), attr);
    ag_console_unlock();
}

static void api_set_cursor(bool visible)
{
    if (!has_console_focus()) {
        return;
    }
    ag_console_lock();
    ag_screen_set_cursor(ag_console_screen(), visible);
    ag_console_unlock();
}

static void api_coninfo(ag_coninfo_t *out)
{
    if (out == NULL) {
        return;
    }
    ag_console_lock();
    const ag_screen_t *sc = ag_console_screen();
    out->cols = sc->cols;
    out->rows = sc->rows;
    out->cur_x = sc->cur_x;
    out->cur_y = sc->cur_y;
    out->attr = sc->attr;
    out->has_local_display = ag_display_ready();
    /*
     * The cell, in the pixels a pointer event uses.  Zero when there is no
     * surface: a serial terminal is all there is, and it has no pixels.
     */
    out->cell_w = 0;
    out->cell_h = 0;
    uint16_t sw = 0, sh = 0;
    if (ag_display_size(&sw, &sh) && sc->cols > 0 && sc->rows > 0) {
        out->cell_w = (uint16_t)(sw / sc->cols);
        out->cell_h = (uint16_t)(sh / sc->rows);
    }
    ag_console_unlock();
}

static void api_poke(uint16_t x, uint16_t y, char ch, uint8_t attr)
{
    if (!has_console_focus()) {
        return;
    }
    ag_console_lock();
    ag_screen_poke(ag_console_screen(), x, y, ch, attr);
    ag_console_unlock();
}

static void api_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, char ch,
                     uint8_t attr)
{
    if (!has_console_focus()) {
        return;
    }
    ag_console_lock();
    ag_screen_fill(ag_console_screen(), x, y, w, h, ch, attr);
    ag_console_unlock();
}

/*
 * The console's cells, read back (ABI 0.43).
 *
 * Under the console lock, because a row is copied out of the live screen and
 * the console writes to it from another task - a torn row would be a line of
 * text with half of two different messages in it, which is the kind of thing
 * that gets blamed on the reader.
 */
/*
 * A row of another slot's screen.
 *
 * The permission is the same one api_peek_row asks for and it is asked for
 * the same reason: the console belongs to the foreground, and a program
 * that is not in front has no business reading what is on it.  What is
 * different is WHICH screen - a slot with a prompt of its own has one
 * nobody is looking at, and a window that draws it is the only way to see
 * it.
 */
static int32_t api_peek_row_slot(int slot, uint16_t row, ag_textcell_t *cells,
                                 uint16_t max)
{
    if (cells == NULL || max == 0) {
        return -AG_EINVAL;
    }
    if (!has_console_focus()) {
        return -AG_EPERM;
    }

    int32_t written = -AG_ENODEV;

    ag_console_lock();
    const ag_screen_t *sc = ag_console_screen_of_slot(slot);
    if (sc != NULL && row < sc->rows) {
        const ag_cell_t *src = ag_screen_row(sc, row);
        if (src != NULL) {
            uint16_t n = sc->cols;
            if (n > max) {
                n = max;
            }
            for (uint16_t i = 0; i < n; i++) {
                cells[i].ch = (uint8_t)src[i].ch;
                cells[i].attr = src[i].attr;
            }
            written = (int32_t)n;
        }
    }
    ag_console_unlock();
    return written;
}

/*
 * Where the cursor is on another slot's screen.
 *
 * Same permission as reading its rows, and for the same reason: this is
 * somebody else's screen, and only whatever is holding the glass has any
 * business drawing it.
 */
static int32_t api_cursor_of_slot(int slot, uint16_t *x, uint16_t *y)
{
    if (x == NULL || y == NULL) {
        return -AG_EINVAL;
    }
    if (!has_console_focus()) {
        return -AG_EPERM;
    }

    int32_t err = -AG_ENODEV;
    ag_console_lock();
    const ag_screen_t *sc = ag_console_screen_of_slot(slot);
    if (sc != NULL) {
        *x = sc->cur_x;
        *y = sc->cur_y;
        err = 0;
    }
    ag_console_unlock();
    return err;
}

/*
 * Give a slot a shell of its own.  A slot below zero means any free one -
 * a window asking for a prompt has no business choosing a number, and
 * nothing in the ABI lets it see which are taken.
 */
static int32_t api_prompt_in_slot(int slot, const char *cwd)
{
    if (slot >= 0) {
        const ag_err_t err = ag_shell_start_in_slot_at(slot, cwd);
        return (err == AG_OK) ? (int32_t)slot : (int32_t)err;
    }

    /*
     * A free slot is one with nothing RUNNING in it, not merely one with
     * no prompt.  Asking only about the prompt put the first one in the
     * caller's own slot - the desktop asks for this, and the desktop is
     * itself the application sitting in slot 1.
     */
    ag_session_slot_t slots[AG_SESSION_SLOTS];
    ag_session_info(slots);
    for (int i = 0; i < AG_SESSION_SLOTS; i++) {
        if (slots[i].pid != AG_PID_KERNEL) {
            continue;
        }
        if (ag_shell_start_in_slot_at(i, cwd) == AG_OK) {
            return (int32_t)i;
        }
    }
    return -AG_ENOSPC;
}

static bool api_inp_post_to_slot(int slot, const ag_event_t *ev)
{
    if (ev == NULL || !has_console_focus()) {
        return false;
    }
    return ag_console_post_to_slot(slot, ev);
}

static int32_t api_peek_row(uint16_t row, ag_textcell_t *cells, uint16_t max)
{
    if (cells == NULL || max == 0) {
        return -AG_EINVAL;
    }
    if (!has_console_focus()) {
        return -AG_EPERM;
    }

    int32_t written = -AG_ENODEV;

    ag_console_lock();
    const ag_screen_t *sc = ag_console_screen();
    if (sc != NULL && row < sc->rows) {
        const ag_cell_t *src = ag_screen_row(sc, row);
        if (src != NULL) {
            uint16_t n = sc->cols;
            if (n > max) {
                n = max;
            }
            for (uint16_t i = 0; i < n; i++) {
                cells[i].ch = (uint8_t)src[i].ch;
                cells[i].attr = src[i].attr;
            }
            written = (int32_t)n;
        }
    }
    ag_console_unlock();
    return written;
}

static uint16_t api_codepage(void) { return ag_cp_number(ag_cp_active()); }

static ag_err_t api_set_codepage(uint16_t number)
{
    ag_cp_t chosen;
    if (!ag_cp_from_number(number, &chosen)) {
        return -AG_EINVAL;
    }
    ag_cp_set_active(chosen);

    /* The bytes on the screen now mean other characters, so every row is stale. */
    ag_console_lock();
    ag_screen_mark_all_dirty(ag_console_screen());
    ag_console_unlock();
    return AG_OK;
}

static const ag_con_api_t k_con = {
    .size = sizeof(ag_con_api_t),
    .write = api_con_write,
    .puts = api_con_puts,
    .printf = api_con_printf,
    .vprintf = api_con_vprintf,
    .getch = api_getch,
    .kbhit = api_kbhit,
    .readline = api_readline,
    .cls = api_cls,
    .gotoxy = api_gotoxy,
    .set_attr = api_set_attr,
    .set_cursor = api_set_cursor,
    .info = api_coninfo,
    .poke = api_poke,
    .fill = api_fill,
    .codepage = api_codepage,
    .set_codepage = api_set_codepage,
    .peek_row = api_peek_row,
    .peek_row_slot = api_peek_row_slot,
    .cursor_of_slot = api_cursor_of_slot,
};

/* ---------------------------------------------------------------------- */
/* fs                                                                     */
/* ---------------------------------------------------------------------- */

static ag_handle_t api_open(const char *path, uint32_t flags)
{
    return ag_vfs_open(path, ag_proc_cwd(), flags);
}

static ag_err_t api_stat(const char *path, ag_stat_t *out)
{
    return ag_vfs_stat(path, ag_proc_cwd(), out);
}

static ag_err_t api_unlink(const char *path)
{
    return ag_vfs_unlink(path, ag_proc_cwd());
}

static ag_err_t api_rename(const char *from, const char *to)
{
    return ag_vfs_rename(from, to, ag_proc_cwd());
}

static ag_err_t api_mkdir(const char *path)
{
    return ag_vfs_mkdir(path, ag_proc_cwd());
}

static ag_err_t api_rmdir(const char *path)
{
    return ag_vfs_rmdir(path, ag_proc_cwd());
}

static ag_handle_t api_opendir(const char *path)
{
    return ag_vfs_opendir(path, ag_proc_cwd());
}

static ag_err_t api_getcwd(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }
    const char  *cwd = ag_proc_cwd();
    const size_t n = strlen(cwd);
    if (n + 1 > len) {
        return -AG_ERANGE;
    }
    memcpy(buf, cwd, n + 1);
    return AG_OK;
}

static ag_err_t api_chdir(const char *path)
{
    char resolved[AG_PATH_MAX];
    ag_err_t err = ag_path_resolve(path, ag_proc_cwd(), resolved,
                                   sizeof(resolved));
    if (err != AG_OK) {
        return err;
    }

    ag_stat_t st;
    err = ag_vfs_stat(resolved, NULL, &st);
    if (err != AG_OK) {
        return err;
    }
    if (!(st.attr & AG_A_DIR)) {
        return -AG_ENOTDIR;
    }
    /*
     * The process's own directory, not the shell's.  An application that changes
     * directory used to change the shell's, which came back as a surprise the
     * moment it exited.
     */
    return ag_proc_set_cwd(resolved);
}

static ag_err_t api_mountinfo(const char *mount, ag_fsinfo_t *out)
{
    for (uint32_t i = 0;; i++) {
        ag_mountinfo_t mi;
        if (ag_vfs_mount_info(i, &mi) != AG_OK) {
            return -AG_ENOENT;
        }
        if (mount == NULL || strcmp(mi.mount, mount) == 0) {
            *out = mi.info;
            return AG_OK;
        }
    }
}

/*
 * A file mapped where it can be read as bytes.  The path is resolved against
 * the calling process's own directory, like every other fs call here.
 */
static ag_err_t api_map(const char *path, const void **out, uint64_t *out_len)
{
    return ag_filemap_open(path, ag_proc_cwd(), out, out_len);
}

static ag_err_t api_unmap(const void *ptr)
{
    return ag_filemap_close(ptr);
}

static const ag_fs_api_t k_fs = {
    .size = sizeof(ag_fs_api_t),
    .open = api_open,
    .close = ag_vfs_close,
    .read = ag_vfs_read,
    .write = ag_vfs_write,
    .seek = ag_vfs_seek,
    .sync = ag_vfs_sync,
    .truncate = ag_vfs_truncate,
    .stat = api_stat,
    .unlink = api_unlink,
    .rename = api_rename,
    .mkdir = api_mkdir,
    .rmdir = api_rmdir,
    .opendir = api_opendir,
    .readdir = ag_vfs_readdir,
    .closedir = ag_vfs_closedir,
    .getcwd = api_getcwd,
    .chdir = api_chdir,
    .mountinfo = api_mountinfo,
    .map = api_map,
    .unmap = api_unmap,
};

/* ---------------------------------------------------------------------- */
/* time                                                                   */
/* ---------------------------------------------------------------------- */

static ag_time_t api_us(void) { return (ag_time_t)ag_port_us(); }
static uint32_t api_ms(void) { return (uint32_t)(ag_port_us() / 1000); }
/*
 * Real CPU cycles, not microseconds.  This used to return ag_port_us()
 * - the same number as api_us() - which made the entry useless for exactly the
 * job it exists for: costing a DSP inner loop, where a microsecond is 240
 * cycles and the whole question is how many of them one sample costs.  The
 * counter is per-core and wraps every ~18 s at 240 MHz; callers measure
 * differences, so the wrap is theirs to handle.
 */
static uint64_t api_cycles(void) { return (uint64_t)ag_port_cycles(); }

static void api_delay_ms(uint32_t ms) { ag_port_task_delay(ag_port_ms_to_ticks(ms)); }

static void api_delay_us(uint32_t us)
{
    /* Busy-wait: below a tick there is nothing to yield to. */
    const int64_t until = ag_port_us() + (int64_t)us;
    while (ag_port_us() < until) {
    }
}

static ag_err_t api_get_datetime(ag_datetime_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }

    const time_t now = time(NULL);
    struct tm    tm_buf;
    if (gmtime_r(&now, &tm_buf) == NULL) {
        return -AG_EIO;
    }

    out->year = (uint16_t)(tm_buf.tm_year + 1900);
    out->month = (uint8_t)(tm_buf.tm_mon + 1);
    out->day = (uint8_t)tm_buf.tm_mday;
    out->hour = (uint8_t)tm_buf.tm_hour;
    out->minute = (uint8_t)tm_buf.tm_min;
    out->second = (uint8_t)tm_buf.tm_sec;
    out->weekday = (uint8_t)tm_buf.tm_wday;
    return AG_OK;
}

/*
 * Set the wall clock, UTC.
 *
 * The entry beside this one said `NULL, needs an RTC driver` for as long
 * as it has existed, and that was two different questions treated as one.
 * Keeping time across a power cut needs an RTC and none of these boards
 * has one; KNOWING what the time is only needs somebody to say so, and
 * until now the only somebody was a network time server - which the CYD
 * cannot reach at all, because bringing its radio up damages its
 * SYSTEM.CFG.  A machine that cannot be told the time by the person
 * sitting in front of it is worse than one with no clock.
 *
 * It is lost at every power cut, and that is honest rather than
 * regrettable: the shell draws an unset clock as --:-- rather than as
 * half past midnight in 1970.
 */
static ag_err_t api_set_datetime(const ag_datetime_t *dt)
{
    if (dt == NULL || dt->year < 1970u || dt->month < 1u || dt->month > 12u ||
        dt->day < 1u || dt->day > 31u || dt->hour > 23u || dt->minute > 59u ||
        dt->second > 60u) {
        return -AG_EINVAL;
    }

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = (int)dt->year - 1900;
    tm.tm_mon = (int)dt->month - 1;
    tm.tm_mday = (int)dt->day;
    tm.tm_hour = (int)dt->hour;
    tm.tm_min = (int)dt->minute;
    tm.tm_sec = (int)dt->second;
    tm.tm_isdst = 0;

    /* No timezone is set on the board, so mktime's "local" is UTC. */
    const time_t t = mktime(&tm);
    if (t == (time_t)-1) {
        return -AG_EINVAL;
    }

    struct timeval tv;
    tv.tv_sec = t;
    tv.tv_usec = 0;
    return (settimeofday(&tv, NULL) == 0) ? AG_OK : -AG_EIO;
}

static const ag_time_api_t k_time = {
    .size = sizeof(ag_time_api_t),
    .us = api_us,
    .ms = api_ms,
    .cycles = api_cycles,
    .delay_ms = api_delay_ms,
    .delay_us = api_delay_us,
    .get_datetime = api_get_datetime,
    .set_datetime = api_set_datetime,
    .timer_create = NULL,
    .timer_delete = NULL,
};

/* ---------------------------------------------------------------------- */
/* inp                                                                    */
/* ---------------------------------------------------------------------- */

/*
 * Whole events, for applications that need more than characters: arrow keys,
 * function keys and modifiers have no character to report, so con->getch cannot
 * carry them.  Anything that draws its own screen - a file manager, an editor -
 * needs this rather than that.
 */
static bool api_inp_poll(ag_event_t *out, uint32_t timeout_ms)
{
    if (ag_proc_take_focus_event(out)) {
        return true;
    }
    if (!has_the_keyboard()) {
        /*
         * Must sleep: a tight poll while unfocused starves the shell (and
         * everything else on this core).  Wake early if focus returns.
         */
        if (timeout_ms == 0) {
            return false;
        }
        uint32_t left = timeout_ms;
        while (left > 0u) {
            const uint32_t step = left > 50u ? 50u : left;
            ag_port_task_delay(ag_port_ms_to_ticks(step < 1u ? 1u : step));
            left -= step;
            if (ag_proc_take_focus_event(out)) {
                return true;
            }
            if (has_the_keyboard()) {
                return ag_console_read_event(out, left);
            }
        }
        return false;
    }
    return ag_console_read_event(out, timeout_ms);
}

static void api_inp_flush(void) { ag_console_flush_input(); }

static uint16_t api_inp_mods(void) { return ag_console_mods(); }

/*
 * Level state from the input layer (HostFS PADPUSH is one source).  Stale or
 * missing → 0 so callers can fall back to sticky keys via btn/btnp.
 */
static uint32_t api_inp_pad(int which) { return ag_input_pad_byte(which); }

static int32_t api_inp_btn(int id) { return ag_input_btnp(0, id); }

static int32_t api_inp_btnp(int pad, int id) { return ag_input_btnp(pad, id); }

static bool api_inp_inject(const ag_event_t *ev)
{
    return ag_console_inject_event(ev);
}

static const ag_inp_api_t k_inp = {
    .size = sizeof(ag_inp_api_t),
    .poll = api_inp_poll,
    .flush = api_inp_flush,
    .key_pressed = ag_console_key_pressed,
    .mods = api_inp_mods,
    .pad = api_inp_pad,
    .btn = api_inp_btn,
    .btnp = api_inp_btnp,
    .inject = api_inp_inject,
    .post_to_slot = api_inp_post_to_slot,
};

/* ---------------------------------------------------------------------- */
/* dev                                                                    */
/* ---------------------------------------------------------------------- */

/*
 * A device handle is a file handle.  Everything below forwards to the VFS,
 * because /dev is a mount and the handle table, the ownership and the closing
 * of what a killed process left open are already there - a second table would
 * be a second place to forget.  Only ioctl and the class vtable need the device
 * itself, and those come back from the handle.
 */
static ag_handle_t api_dev_open(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return -AG_EINVAL;
    }

    /*
     * Both spellings work: "sd0" because that is the device's name, and
     * "d:\sd0" or "/dev/sd0" because that is where it lives.  An application
     * that has a path from somewhere else should not have to take it apart.
     */
    char        path[AG_PATH_MAX];
    const char *target = name;
    const bool  is_path = strchr(name, '/') != NULL ||
                         strchr(name, '\\') != NULL ||
                         strchr(name, ':') != NULL;

    if (!is_path) {
        const size_t n = strlen(name);
        if (n + sizeof("/dev/") > sizeof(path)) {
            return -AG_ERANGE;
        }
        memcpy(path, "/dev/", sizeof("/dev/") - 1);
        memcpy(path + sizeof("/dev/") - 1, name, n + 1);
        target = path;
    }

    /*
     * Read-write first, and read-only when the device refuses to be written.
     * The ABI has one way in, so it cannot ask the caller which it wanted, and
     * a caller that only reads must not be turned away from a read-only device.
     */
    const char *cwd = is_path ? ag_proc_cwd() : NULL;

    const ag_handle_t h = ag_vfs_open(target, cwd, AG_O_RDWR);
    return (h != -AG_EROFS) ? h : ag_vfs_open(target, cwd, AG_O_RDONLY);
}

static ag_err_t api_dev_ioctl(ag_handle_t h, uint32_t cmd, void *arg,
                              size_t arglen)
{
    ag_device_t *dev = ag_devfs_device_of(h);
    if (dev == NULL) {
        return -AG_EBADF;
    }
    return ag_dev_ioctl2(dev, ag_devfs_session_of(h), cmd, arg, arglen);
}

static const void *api_dev_ops(ag_handle_t h)
{
    const ag_device_t *dev = ag_devfs_device_of(h);
    return (dev != NULL) ? dev->class_ops : NULL;
}

/*
 * Only a module that is mid-init may publish a device.  The owner cookie is the
 * module slot, so unload can revoke everything that init registered - including
 * devices it added before a later failure.
 */
static ag_err_t api_dev_add(const ag_dev_add_t *desc)
{
    const void *owner = ag_module_loading();
    if (owner == NULL) {
        return -AG_EPERM;
    }
    if (desc == NULL) {
        return -AG_EINVAL;
    }

    const ag_dev_desc_t full = {
        .name = desc->name,
        .driver = desc->driver,
        .cls = desc->cls,
        .flags = desc->flags,
        .ops = desc->ops,
        .class_ops = desc->class_ops,
        .priv = desc->priv,
        .owner = owner,
    };
    return ag_dev_register(&full, NULL);
}

static ag_err_t api_dev_remove(const char *name)
{
    const void *owner = ag_module_loading();
    if (owner == NULL) {
        return -AG_EPERM;
    }

    ag_device_t *dev = ag_dev_find(name);
    if (dev == NULL) {
        return -AG_ENOENT;
    }
    if (dev->owner != owner) {
        return -AG_EPERM;
    }
    return ag_dev_unregister(dev);
}

static void *api_dev_get_priv(ag_device_t *dev)
{
    return (dev != NULL) ? dev->priv : NULL;
}

static const ag_probe_hint_t *api_dev_probe_hint(void)
{
    return ag_module_probe_hint();
}

static const ag_dev_api_t k_dev = {
    .size = sizeof(ag_dev_api_t),
    .enumerate = ag_dev_info,
    .open = api_dev_open,
    .close = ag_vfs_close,
    .read = ag_vfs_read,
    .write = ag_vfs_write,
    .ioctl = api_dev_ioctl,
    .ops = api_dev_ops,
    .add = api_dev_add,
    .remove = api_dev_remove,
    .get_priv = api_dev_get_priv,
    .probe_hint = api_dev_probe_hint,
};

/* ---------------------------------------------------------------------- */
/* proc                                                                   */
/* ---------------------------------------------------------------------- */

/*
 * The ABI hands out `const char **` because an application has no business
 * modifying its own arguments; the process layer copies them into the child
 * immediately, so casting the constness away here goes nowhere.
 */
static int32_t api_exec(const char *path, int argc, const char **argv)
{
    int32_t        code = 0;
    const ag_err_t err = ag_proc_exec(path, argc, (char **)argv, 0, &code);

    if (err != AG_OK && err != -AG_EKILLED) {
        return err;
    }
    return code;
}

static ag_pid_t api_spawn(const char *path, int argc, const char **argv,
                          uint32_t flags)
{
    ag_pid_t       pid = 0;
    const ag_err_t err = ag_proc_spawn(path, argc, (char **)argv, flags, &pid);

    return (err == AG_OK) ? pid : (ag_pid_t)err;
}

static ag_err_t api_wait(ag_pid_t pid, int32_t *exit_code, uint32_t timeout_ms)
{
    return ag_proc_wait(pid, exit_code, timeout_ms);
}

static ag_err_t api_kill(ag_pid_t pid)
{
    return ag_proc_kill(pid, "killed by another process");
}

/* ---------------------------------------------------------------------- */

/*
 * The pid is taken here rather than passed in, for the same reason it is in
 * every other subtable: an application cannot answer on somebody else's
 * behalf, and a call that took a pid would have to be checked for exactly
 * that.
 */
static ag_err_t api_power_status(ag_power_status_t *out)
{
    return ag_power_poll(ag_proc_self(), (uint32_t)(ag_port_us() / 1000), out,
                         ag_powerctl_cpu_mhz());
}

static ag_err_t api_power_answer(ag_power_answer_t a, const char *why)
{
    return ag_power_reply(ag_proc_self(), a, why);
}

static ag_err_t api_power_declare(ag_power_fitness_t fitness, const char *why)
{
    const ag_err_t err = ag_power_declare(ag_proc_self(), fitness, why);
    if (err == AG_OK) {
        /*
         * "I need the full clock" has to be true by the time this returns, not
         * by the next tick: the first buffer an audio path fills is as real as
         * the thousandth.
         */
        ag_powerctl_declared(fitness);
    }
    return err;
}

static const ag_power_api_t k_power = {
    .size = sizeof(ag_power_api_t),
    .status = api_power_status,
    .answer = api_power_answer,
    .declare = api_power_declare,
};

static const ag_proc_api_t k_proc = {
    .size = sizeof(ag_proc_api_t),
    .exec = api_exec,
    .spawn = api_spawn,
    .wait = api_wait,
    .kill = api_kill,
    .self = ag_proc_self,
    .enumerate = ag_proc_info,
    .foreground = ag_proc_set_foreground,
    .getenv = NULL, /* no environment yet */
    .setenv = NULL,
    .interrupted = ag_proc_interrupted,
    .watchdog = ag_proc_watchdog,
    .focused = ag_proc_focused,
};

/* ---------------------------------------------------------------------- */

/*
 * Subsystems that do not exist yet are NULL rather than stubs that fail.  An
 * application can then ask - if (ag_api()->net) - and adapt, which is what the
 * feature probing in the ABI is for.  gfx is live from 0.8 (soft framebuffer).
 */
#if AG_PORT_HAS_BLE_PERIPH || AG_PORT_HAS_BLE_CENTRAL
/*
 * BLE for applications.  Every entry that puts the radio on the air starts it
 * first (ag_port_bt_start) and moves the bus where a radio needs it
 * (ag_powerctl_bus_needed) - an application should not have to know that `bt on`
 * is a separate step.  The rest are thin: the port carries the whole of it and
 * the shell drives the same calls, so this is the append to the table that lets
 * an .AXE reach what only the shell could before.  Entries a build left out
 * (central or peripheral) stay NULL by designated-initialiser, and an
 * application feature-probes with AG_HAS.
 */
#if AG_PORT_HAS_BLE_PERIPH
static ag_err_t api_ble_midi_advertise(const char *name)
{
    const ag_err_t serr = ag_port_bt_start();
    if (serr != AG_OK) {
        return serr;
    }
    ag_powerctl_bus_needed();
    return ag_port_ble_midi_advertise(name);
}

static ag_err_t api_ble_midi_send(uint8_t status, uint8_t d1, uint8_t d2)
{
    return ag_port_ble_midi_send(status, d1, d2);
}

static bool api_ble_midi_ready(void) { return ag_port_ble_midi_ready(); }

static ag_err_t api_ble_adv_stop(void) { return ag_port_ble_adv_stop(); }

static ag_err_t api_ble_adv_start(const char *name)
{
    const ag_err_t serr = ag_port_bt_start();
    if (serr != AG_OK) {
        return serr;
    }
    ag_powerctl_bus_needed();
    return ag_port_ble_adv_start(name);
}

static ag_err_t api_ble_adv_raw(const uint8_t addr[6], const void *data,
                                uint32_t len)
{
    /*
     * The radio is the system's to raise (bt on), as it is for Wi-Fi.
     * An application does not raise it here: on this chip the BLE
     * bring-up is not safe from an application task, and the port
     * answers -AG_ENODEV when the radio is off, which the caller
     * surfaces as "run bt on first".
     */
    ag_powerctl_bus_needed();
    return ag_port_ble_adv_raw(addr, (const uint8_t *)data, len);
}

static void api_ble_adv_set_read(const void *data, uint32_t len)
{
    ag_port_ble_adv_set_read(data, len);
}

static int32_t api_ble_adv_last_write(uint8_t *out, uint32_t max)
{
    return ag_port_ble_adv_last_write(out, max);
}

static ag_err_t api_ble_adv_status(ag_ble_adv_status_t *out)
{
    return ag_port_ble_adv_status(out);
}
#endif /* AG_PORT_HAS_BLE_PERIPH */

#if AG_PORT_HAS_BLE_CENTRAL
static ag_err_t api_ble_scan(ag_ble_dev_t *out, uint32_t max, uint32_t *found,
                             uint32_t seconds)
{
    /*
     * The radio is the system's to raise (bt on), as it is for Wi-Fi.
     * An application does not raise it here: on this chip the BLE
     * bring-up is not safe from an application task, and the port
     * answers -AG_ENODEV when the radio is off, which the caller
     * surfaces as "run bt on first".
     */
    ag_powerctl_bus_needed();
    return ag_port_ble_scan(out, max, found, seconds);
}

static ag_err_t api_ble_connect(const uint8_t addr[6], int addr_type,
                                uint32_t timeout_ms)
{
    /*
     * The radio is the system's to raise (bt on), as it is for Wi-Fi.
     * An application does not raise it here: on this chip the BLE
     * bring-up is not safe from an application task, and the port
     * answers -AG_ENODEV when the radio is off, which the caller
     * surfaces as "run bt on first".
     */
    ag_powerctl_bus_needed();
    return ag_port_ble_connect(addr, addr_type, timeout_ms);
}

static ag_err_t api_ble_disconnect(void) { return ag_port_ble_disconnect(); }
static bool     api_ble_connected(void) { return ag_port_ble_connected(); }
static ag_err_t api_ble_discover(uint32_t timeout_ms)
{
    return ag_port_ble_discover(timeout_ms);
}
static uint32_t api_ble_services(ag_ble_svc_t *out, uint32_t max)
{
    return ag_port_ble_services(out, max);
}
static uint32_t api_ble_chars(ag_ble_chr_t *out, uint32_t max)
{
    return ag_port_ble_chars(out, max);
}
static int32_t api_ble_read(uint16_t handle, uint8_t *out, uint32_t max,
                            uint32_t timeout_ms)
{
    return ag_port_ble_read(handle, out, max, timeout_ms);
}
static ag_err_t api_ble_write(uint16_t handle, const void *data, uint32_t len,
                              bool with_response, uint32_t timeout_ms)
{
    return ag_port_ble_write(handle, data, len, with_response, timeout_ms);
}
#endif /* AG_PORT_HAS_BLE_CENTRAL */

static const ag_ble_api_t k_ble = {
    .size = sizeof(ag_ble_api_t),
#if AG_PORT_HAS_BLE_PERIPH
    .midi_advertise = api_ble_midi_advertise,
    .midi_send = api_ble_midi_send,
    .midi_ready = api_ble_midi_ready,
    .adv_stop = api_ble_adv_stop,
    .adv_start = api_ble_adv_start,
    .adv_set_read = api_ble_adv_set_read,
    .adv_last_write = api_ble_adv_last_write,
    .adv_status = api_ble_adv_status,
#endif
#if AG_PORT_HAS_BLE_CENTRAL
    .scan = api_ble_scan,
    .connect = api_ble_connect,
    .disconnect = api_ble_disconnect,
    .connected = api_ble_connected,
    .discover = api_ble_discover,
    .services = api_ble_services,
    .chars = api_ble_chars,
    .read = api_ble_read,
    .write = api_ble_write,
#endif
#if AG_PORT_HAS_BLE_PERIPH
    .adv_raw = api_ble_adv_raw,
#endif
};
#endif /* AG_PORT_HAS_BLE_PERIPH || AG_PORT_HAS_BLE_CENTRAL */

#if AG_PORT_HAS_WIFIMON
/*
 * Monitor / raw injection for applications: the app-facing face of the same
 * kernel ring and counters the shell's `mon` drains (src/net/wifimon.c).  recv
 * waits up to timeout_ms for a frame rather than making every application write
 * its own poll loop; everything else is a straight pass to the kernel layer,
 * which is where the lock between the radio task and the caller already lives.
 */
static ag_err_t api_wm_start(void)
{
    ag_powerctl_bus_needed();
    /*
     * Promiscuous is a mode of a started radio, not a way to start one - so the
     * radio has to be up first, exactly as the shell's `mon on` brings it up
     * (src/shell/shell.c).  An application that only ever captures should not
     * have to know that `wifi on` is a separate step.
     */
    ag_port_wifi_status_t st;
    if (ag_port_wifi_status(&st) != AG_OK || st.state == AG_WIFI_OFF) {
        const ag_err_t nerr = ag_net_init();
        if (nerr != AG_OK) {
            return nerr;
        }
    }
    return ag_wifimon_start();
}
static ag_err_t api_wm_stop(void)
{
    ag_wifimon_stop();
    return AG_OK;
}
static ag_err_t api_wm_channel(uint8_t primary)
{
    return ag_wifimon_channel(primary);
}
static uint8_t api_wm_channel_get(void) { return ag_wifimon_channel_get(); }
static ag_err_t api_wm_filter(uint32_t mask) { return ag_wifimon_filter(mask); }

static int32_t api_wm_recv(void *buf, uint32_t max, ag_wifimon_frame_t *meta,
                           uint32_t timeout_ms)
{
    const int64_t until = (int64_t)ag_port_us() + (int64_t)timeout_ms * 1000;
    for (;;) {
        int8_t   rssi = 0;
        uint8_t  channel = 0;
        uint32_t full = 0;
        uint32_t copied = 0;
        if (ag_wifimon_drain(&rssi, &channel, &full, (uint8_t *)buf, max,
                             &copied)) {
            if (meta != NULL) {
                meta->rssi = rssi;
                meta->channel = channel;
                meta->length = full;
            }
            return (int32_t)copied;
        }
        if ((int64_t)ag_port_us() >= until) {
            return -AG_EAGAIN;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(2));
    }
}

static ag_err_t api_wm_tx(const void *frame, uint32_t len)
{
    return ag_wifimon_tx(frame, len);
}
static void api_wm_counters(uint32_t out[AG_WIFIMON_C_N])
{
    ag_wifimon_counters(out);
}
static uint32_t api_wm_dropped(void) { return ag_wifimon_dropped(); }

static const ag_wifimon_api_t k_wifimon = {
    .size = sizeof(ag_wifimon_api_t),
    .start = api_wm_start,
    .stop = api_wm_stop,
    .channel = api_wm_channel,
    .channel_get = api_wm_channel_get,
    .filter = api_wm_filter,
    .recv = api_wm_recv,
    .tx_raw = api_wm_tx,
    .counters = api_wm_counters,
    .dropped = api_wm_dropped,
};
#endif /* AG_PORT_HAS_WIFIMON */

#if AG_PORT_HAS_WIFI
/*
 * The rest of the radio for applications (ABI 0.39): station, access point and
 * ESP-NOW.  Each call is a thin pass to the same port and kernel layer the
 * shell's `wifi` and `espnow` commands drive (src/shell/shell.c), so an
 * application and the prompt reach the radio through one implementation.  The
 * only work done here is raising the radio when a call needs it and converting
 * the port's structs into the ABI's - the ABI cannot name the GPL port types,
 * so the two are kept field-for-field parallel and copied across.
 */
static ag_err_t api_wifi_radio_up(void)
{
    ag_powerctl_bus_needed();
    ag_port_wifi_status_t st;
    if (ag_port_wifi_status(&st) != AG_OK || st.state == AG_WIFI_OFF) {
        return ag_net_init();
    }
    return AG_OK;
}

static ag_err_t api_wifi_start(void) { return api_wifi_radio_up(); }
static ag_err_t api_wifi_stop(void) { return ag_port_wifi_stop(); }

static ag_err_t api_wifi_scan(ag_wifi_ap_t *out, uint32_t max, uint32_t *found)
{
    if (out == NULL || max == 0u) {
        return -AG_EINVAL;
    }
    const ag_err_t up = api_wifi_radio_up();
    if (up != AG_OK) {
        return up;
    }
    /*
     * No heap here.  The port fills at most sixteen records from a stack buffer
     * of its own, so a matching stack buffer converts them without an
     * allocation - which matters because with the radio up and an application
     * resident on this board, a few hundred spare bytes of heap is exactly what
     * there is not, and a scan that failed only because the conversion buffer
     * could not be allocated read as -AG_ENOMEM for a scan that would have
     * worked.  `found` still reports every access point the radio heard.
     */
    ag_port_wifi_ap_t tmp[16];
    uint32_t          cap = (max < 16u) ? max : 16u;
    uint32_t          n = 0;
    const ag_err_t    err = ag_port_wifi_scan(tmp, cap, &n);
    if (err == AG_OK) {
        const uint32_t copy = (n < cap) ? n : cap;
        for (uint32_t i = 0; i < copy; i++) {
            memcpy(out[i].ssid, tmp[i].ssid, sizeof(out[i].ssid));
            memcpy(out[i].bssid, tmp[i].bssid, 6u);
            out[i].rssi = tmp[i].rssi;
            out[i].channel = tmp[i].channel;
            out[i].auth = (uint8_t)tmp[i].auth;
        }
        if (found != NULL) {
            *found = n;
        }
    }
    return err;
}

static ag_err_t api_wifi_connect(const char *ssid, const char *pass,
                                 const uint8_t bssid[6])
{
    ag_powerctl_bus_needed();
    return ag_port_wifi_connect(ssid, pass, bssid);
}

static ag_err_t api_wifi_disconnect(void) { return ag_port_wifi_disconnect(); }

static ag_err_t api_wifi_status(ag_wifi_status_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    ag_port_wifi_status_t st;
    const ag_err_t        err = ag_port_wifi_status(&st);
    if (err != AG_OK) {
        return err;
    }
    memset(out, 0, sizeof(*out));
    out->state = (uint8_t)st.state;
    memcpy(out->ssid, st.ssid, sizeof(out->ssid));
    memcpy(out->bssid, st.bssid, 6u);
    out->pinned = st.pinned;
    out->rssi = st.rssi;
    out->channel = st.channel;
    out->attempts = st.attempts;
    out->last_reason = (int32_t)st.last_reason;
    return AG_OK;
}

#if AG_PORT_WIFI_HAS_AP
static ag_err_t api_wifi_ap_start(const char *ssid, const char *pass,
                                  uint8_t channel, bool hidden)
{
    const ag_err_t up = api_wifi_radio_up();
    if (up != AG_OK) {
        return up;
    }
    return ag_port_wifi_ap_start(ssid, pass, channel, hidden);
}
static ag_err_t api_wifi_ap_stop(void) { return ag_port_wifi_ap_stop(); }
static ag_err_t api_wifi_ap_status(ag_wifi_ap_status_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    ag_port_wifi_ap_status_t ap;
    const ag_err_t           err = ag_port_wifi_ap_status(&ap);
    if (err != AG_OK) {
        return err;
    }
    memset(out, 0, sizeof(*out));
    out->on = ap.on;
    memcpy(out->ssid, ap.ssid, sizeof(out->ssid));
    out->channel = ap.channel;
    out->hidden = ap.hidden;
    out->secured = ap.secured;
    out->clients = ap.clients;
    out->ip = ap.ip;
    return AG_OK;
}
#endif /* AG_PORT_WIFI_HAS_AP */

#if AG_PORT_HAS_ESPNOW
static ag_err_t api_wifi_espnow_start(void)
{
    const ag_err_t up = api_wifi_radio_up();
    if (up != AG_OK) {
        return up;
    }
    return ag_espnow_start();
}
static ag_err_t api_wifi_espnow_stop(void)
{
    ag_espnow_stop();
    return AG_OK;
}
static ag_err_t api_wifi_espnow_self(uint8_t out[6])
{
    return ag_espnow_self(out);
}
static ag_err_t api_wifi_espnow_peer_add(const uint8_t mac[6], uint8_t channel,
                                         const uint8_t *key)
{
    return ag_espnow_peer_add(mac, channel, key);
}
static ag_err_t api_wifi_espnow_peer_del(const uint8_t mac[6])
{
    return ag_espnow_peer_del(mac);
}
static ag_err_t api_wifi_espnow_send(const uint8_t mac[6], const void *data,
                                     uint32_t len)
{
    return ag_espnow_send(mac, data, len);
}
static int32_t api_wifi_espnow_recv(uint8_t mac[6], void *buf, uint32_t max,
                                    uint32_t timeout_ms)
{
    const int64_t until = (int64_t)ag_port_us() + (int64_t)timeout_ms * 1000;
    for (;;) {
        uint32_t len = 0;
        if (ag_espnow_recv(mac, (uint8_t *)buf, max, &len)) {
            return (int32_t)len;
        }
        if ((int64_t)ag_port_us() >= until) {
            return -AG_EAGAIN;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(2));
    }
}
static uint32_t api_wifi_espnow_dropped(void) { return ag_espnow_dropped(); }
#endif /* AG_PORT_HAS_ESPNOW */

static const ag_wifi_api_t k_wifi = {
    .size = sizeof(ag_wifi_api_t),
    .start = api_wifi_start,
    .stop = api_wifi_stop,
    .scan = api_wifi_scan,
    .connect = api_wifi_connect,
    .disconnect = api_wifi_disconnect,
    .status = api_wifi_status,
#if AG_PORT_WIFI_HAS_AP
    .ap_start = api_wifi_ap_start,
    .ap_stop = api_wifi_ap_stop,
    .ap_status = api_wifi_ap_status,
#endif
#if AG_PORT_HAS_ESPNOW
    .espnow_start = api_wifi_espnow_start,
    .espnow_stop = api_wifi_espnow_stop,
    .espnow_self = api_wifi_espnow_self,
    .espnow_peer_add = api_wifi_espnow_peer_add,
    .espnow_peer_del = api_wifi_espnow_peer_del,
    .espnow_send = api_wifi_espnow_send,
    .espnow_recv = api_wifi_espnow_recv,
    .espnow_dropped = api_wifi_espnow_dropped,
#endif
};
#endif /* AG_PORT_HAS_WIFI */

#if defined(CONFIG_ARGON_ENABLE_CAMERA) && CONFIG_ARGON_ENABLE_CAMERA
/* The DVP transport, straight from the port - the sensor driver is a .SYS. */
static const ag_cam_api_t k_cam = {
    .size = sizeof(ag_cam_api_t),
    .configure = ag_port_cam_configure,
    .capture = ag_port_cam_capture,
    .stop = ag_port_cam_stop,
};
#endif

/* ----------------------------------------------------------------------- */
/* cfg - read the system/board configuration (SYSTEM.CFG + BOARD.CFG).      */
/*                                                                          */
/* Read-only for now: a driver reads its own keys (a radio its ssid, a      */
/* panel its rotation).  Writing back to SYSTEM.CFG is the owner's job      */
/* through the shell, so set_str/commit answer -AG_ENOTSUP rather than      */
/* pretend.  Without this table api->cfg was NULL and every `.SYS`/`.AXE`   */
/* silently saw no configuration at all.                                    */
/* ----------------------------------------------------------------------- */
static ag_err_t api_cfg_get_str(const char *key, char *buf, size_t len)
{
    if (key == NULL || buf == NULL || len == 0) {
        return -AG_EINVAL;
    }
    buf[0] = '\0';
    const ag_cfg_t *cfg = ag_sysconfig();
    if (cfg == NULL) {
        return -AG_ENOENT;
    }
    const char *val = ag_cfg_get(cfg, key, NULL);
    if (val == NULL) {
        return -AG_ENOENT;
    }
    size_t i = 0;
    for (; val[i] != '\0' && i + 1 < len; i++) {
        buf[i] = val[i];
    }
    buf[i] = '\0';
    return AG_OK;
}

static int32_t api_cfg_get_int(const char *key, int32_t def)
{
    const ag_cfg_t *cfg = ag_sysconfig();
    return (cfg == NULL) ? def : ag_cfg_get_int(cfg, key, def);
}

static bool api_cfg_get_bool(const char *key, bool def)
{
    const ag_cfg_t *cfg = ag_sysconfig();
    return (cfg == NULL) ? def : ag_cfg_get_bool(cfg, key, def);
}

static ag_err_t api_cfg_set_str(const char *key, const char *value)
{
    (void)key;
    (void)value;
    return -AG_ENOTSUP;
}

static ag_err_t api_cfg_commit(void)
{
    return -AG_ENOTSUP;
}

static const ag_cfg_api_t k_cfg = {
    .size = sizeof(ag_cfg_api_t),
    .get_str = api_cfg_get_str,
    .get_int = api_cfg_get_int,
    .get_bool = api_cfg_get_bool,
    .set_str = api_cfg_set_str,
    .commit = api_cfg_commit,
};

static const ag_api_t k_api = {
    .size = sizeof(ag_api_t),
    .abi_major = AG_ABI_MAJOR,
    .abi_minor = AG_ABI_MINOR,
    .sys = &k_sys,
    .mem = &k_mem,
    .fs = &k_fs,
    .con = &k_con,
    .inp = &k_inp,
    .gfx = &ag_gfx_api_table,
    .dev = &k_dev,
    .io = &ag_io_api_table,
    .time = &k_time,
    .task = &ag_task_api_table,
    .proc = &k_proc,
    .cfg = &k_cfg,
#if defined(CONFIG_ARGON_ENABLE_NET) && CONFIG_ARGON_ENABLE_NET
    .net = &ag_net_api_impl,
#else
    .net = NULL,
#endif
    .audio = &ag_audio_api_table,
#if AG_PORT_HAS_BLE_PERIPH || AG_PORT_HAS_BLE_CENTRAL
    .ble = &k_ble,
#else
    .ble = NULL,
#endif
    .power = &k_power,
#if AG_PORT_HAS_WIFIMON
    .wifimon = &k_wifimon,
#else
    .wifimon = NULL,
#endif
#if AG_PORT_HAS_WIFI
    .wifi = &k_wifi,
#else
    .wifi = NULL,
#endif
#if defined(CONFIG_ARGON_ENABLE_CAMERA) && CONFIG_ARGON_ENABLE_CAMERA
    .cam = &k_cam,
#else
    .cam = NULL,
#endif
};

const ag_api_t *ag_loader_api(void) { return &k_api; }

/*
 * The same pointer an application gets, for the parts of the system that are
 * built into the image but written as applications - the file manager is one.
 * They call through this table exactly as a loaded program would, which is the
 * point: if the built-in works, the loaded one works, and the contract is being
 * used rather than worked around.
 */
const ag_api_t *g_ag_api = &k_api;
