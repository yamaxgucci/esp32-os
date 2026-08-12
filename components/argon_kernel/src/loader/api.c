/*
 * ArgonOS - the syscall table.
 *
 * Nothing here does any work: every entry forwards to the subsystem that owns
 * the job.  That is the point of the table - it is the contract, and keeping it
 * free of logic is what makes it cheap to keep stable.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <stdarg.h>
#include <string.h>
#include <time.h>

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
#include <argon/loader.h>
#include <argon/log.h>
#include <argon/module.h>
#include <argon/net.h>
#include <argon/proc.h>
#include <argon/shell.h>
#include <argon/vfs.h>

#include "sdkconfig.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dev/io.h"

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

static int32_t api_con_write(const char *buf, size_t len)
{
    ag_console_write(buf, len);
    return (int32_t)len;
}

static int32_t api_con_puts(const char *s)
{
    ag_console_puts(s);
    return (s != NULL) ? (int32_t)strlen(s) : 0;
}

static int32_t api_con_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = ag_console_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

static int32_t api_con_vprintf(const char *fmt, va_list ap)
{
    return ag_console_vprintf(fmt, ap);
}

/*
 * The keyboard belongs to whatever is in the foreground.  A background process
 * that asks for input is not made to wait for something it will never get: it is
 * told, which is the only answer it can do anything with.
 */
static bool has_the_keyboard(void)
{
    const ag_pid_t me = ag_proc_self();
    return me == AG_PID_KERNEL || me == ag_proc_foreground();
}

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
    ag_console_lock();
    ag_screen_cls(ag_console_screen());
    ag_console_unlock();
}

static void api_gotoxy(uint16_t x, uint16_t y)
{
    ag_console_lock();
    ag_screen_gotoxy(ag_console_screen(), x, y);
    ag_console_unlock();
}

static void api_set_attr(uint8_t attr)
{
    ag_console_lock();
    ag_screen_set_attr(ag_console_screen(), attr);
    ag_console_unlock();
}

static void api_set_cursor(bool visible)
{
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
    ag_console_unlock();
}

static void api_poke(uint16_t x, uint16_t y, char ch, uint8_t attr)
{
    ag_console_lock();
    ag_screen_poke(ag_console_screen(), x, y, ch, attr);
    ag_console_unlock();
}

static void api_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, char ch,
                     uint8_t attr)
{
    ag_console_lock();
    ag_screen_fill(ag_console_screen(), x, y, w, h, ch, attr);
    ag_console_unlock();
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
};

/* ---------------------------------------------------------------------- */
/* time                                                                   */
/* ---------------------------------------------------------------------- */

static ag_time_t api_us(void) { return (ag_time_t)esp_timer_get_time(); }
static uint32_t api_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint64_t api_cycles(void) { return (uint64_t)esp_timer_get_time(); }

static void api_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void api_delay_us(uint32_t us)
{
    /* Busy-wait: below a tick there is nothing to yield to. */
    const int64_t until = esp_timer_get_time() + (int64_t)us;
    while (esp_timer_get_time() < until) {
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

static const ag_time_api_t k_time = {
    .size = sizeof(ag_time_api_t),
    .us = api_us,
    .ms = api_ms,
    .cycles = api_cycles,
    .delay_ms = api_delay_ms,
    .delay_us = api_delay_us,
    .get_datetime = api_get_datetime,
    .set_datetime = NULL, /* needs an RTC driver */
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
    return ag_dev_ioctl(dev, cmd, arg, arglen);
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
    .cfg = NULL,
#if defined(CONFIG_ARGON_ENABLE_NET) && CONFIG_ARGON_ENABLE_NET
    .net = &ag_net_api_impl,
#else
    .net = NULL,
#endif
    .audio = &ag_audio_api_table,
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
