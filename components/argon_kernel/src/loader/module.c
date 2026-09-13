/*
 * ArgonOS - loadable .SYS driver modules.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/module.h>

#include <stdio.h>
#include <string.h>

#include <argon/cfg.h>
#include <argon/device.h>
#include <argon/kernel.h>
#include <argon/log.h>
#include <argon/path.h>
#include <argon/probe.h>
#include <argon/port/task.h>

#include "core/sysconfig.h"

typedef void (*ag_module_fini_fn)(void);

/*
 * Two tasks per module, which is one more than anything needs today and a
 * bound rather than a list that grows - the same reasoning as AG_MODULE_MAX
 * above it.
 */
#define AG_MODULE_TASKS 2

typedef struct module_s module_t;

typedef struct {
    module_t       *owner;
    void          (*fn)(void *);
    void           *arg;
    ag_port_task_t  task;
    volatile bool   running;
} modtask_t;

struct module_s {
    bool             used;
    char             path[AG_PATH_MAX];
    ag_loaded_app_t  app;
    ag_module_fini_fn fini;
    modtask_t        tasks[AG_MODULE_TASKS];
};

static module_t               s_modules[AG_MODULE_MAX];
static module_t              *s_loading;
static const ag_probe_hint_t *s_hint;

const void *ag_module_loading(void) { return s_loading; }

const ag_probe_hint_t *ag_module_probe_hint(void) { return s_hint; }

void ag_module_on_unload(void (*fn)(void))
{
    if (s_loading == NULL) {
        ag_log(AG_LOG_WARN, "modules",
               "ag_module_on_unload outside ag_driver_init — ignored");
        return;
    }
    s_loading->fini = fn;
}

/*
 * The task the module asked for, wrapped so that the module can be told when it
 * has really gone.
 *
 * `running` is cleared here and not by the caller, because the only moment at
 * which it is true that the task is no longer inside the module's code is after
 * that code has returned - and the task deletes itself on the next line, which
 * is a place the module's code cannot reach.
 */
static void module_task_entry(void *arg)
{
    modtask_t *t = (modtask_t *)arg;

    t->fn(t->arg);
    t->running = false;
    ag_port_task_delete(NULL);
}

bool ag_module_task(void (*fn)(void *), void *arg, const char *name,
                    uint32_t stack, int priority, uint32_t flags)
{
    if (fn == NULL) {
        return false;
    }
    if (s_loading == NULL) {
        ag_log(AG_LOG_WARN, "modules",
               "ag_module_task outside ag_driver_init — ignored");
        return false;
    }

    modtask_t *slot = NULL;
    for (uint32_t i = 0; i < AG_MODULE_TASKS; i++) {
        if (!s_loading->tasks[i].running) {
            slot = &s_loading->tasks[i];
            break;
        }
    }
    if (slot == NULL) {
        ag_log(AG_LOG_WARN, "modules", "%s already has %u tasks",
               s_loading->app.header.name, (unsigned)AG_MODULE_TASKS);
        return false;
    }

    if (stack < 2048u) {
        stack = 2048u;
    }
    slot->owner = s_loading;
    slot->fn = fn;
    slot->arg = arg;
    slot->task = NULL;
    /* Marked before it exists, for the same reason the process layer does it:
     * a task that starts running before it is written down is a task the
     * module might not be waiting for. */
    slot->running = true;

    /*
     * The core, from the same flags an application's thread uses.  It matters:
     * the foreground application is pinned to the app core, so a driver doing
     * real work there is a second runnable task on the one core that is already
     * busy - which cost the game half its frame rate before the screen driver
     * asked for the system core instead.
     *
     * AG_THREAD_APP_CORE is zero, so "put me on the application core" and "no
     * flags at all" arrive here identically, and both must pin: an earlier
     * version let them fall through to no affinity, and every measurement taken
     * on the strength of that request was measuring an unknown core.
     */
    int core;
    if (flags & AG_THREAD_ANY_CORE) {
        core = AG_PORT_ANY_CORE;
    } else if (flags & AG_THREAD_SYS_CORE) {
        core = 0;
    } else {
        core = (int)ag_sysinfo()->app_core;
    }

    if (!ag_port_task_create(module_task_entry,
                             (name != NULL) ? name : "moddrv", stack, slot,
                             (unsigned)((priority > 0) ? priority : 5),
                             core, 0, &slot->task)) {
        slot->running = false;
        ag_log(AG_LOG_WARN, "modules", "no memory for a %u byte task stack",
               (unsigned)stack);
        return false;
    }
    /*
     * What is deliberately not here: the thread budget that watches a driver
     * task asking for a priority above the applications and drops it below them
     * when it overruns.  That lives on `worktree-fallout-cxx` with argon/budget.h
     * and has not landed here; until it does, a driver above priority 5 can
     * starve the foreground application and nothing will say so.
     */
    return true;
}

/*
 * How long to wait for a module's tasks after telling them to stop.
 *
 * Generous on purpose: the thing a driver task is most likely to be doing when
 * asked to stop is a write to a wire, and a band of pixels at four megabaud is
 * six milliseconds.  Half a second is eighty of those, and an unload that takes
 * half a second is a nuisance where an unload that unmaps running code is a
 * crash somewhere else entirely.
 */
#define MODULE_TASK_WAIT_MS 500u

static bool tasks_stopped(module_t *m)
{
    for (uint32_t i = 0; i < AG_MODULE_TASKS; i++) {
        if (m->tasks[i].running) {
            return false;
        }
    }
    return true;
}

static void set_string_path(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_len) {
        n = dst_len - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static module_t *find_by_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    for (uint32_t i = 0; i < AG_MODULE_MAX; i++) {
        if (s_modules[i].used &&
            ag_path_icmp(s_modules[i].app.header.name, name) == 0) {
            return &s_modules[i];
        }
    }
    return NULL;
}

static module_t *alloc_slot(void)
{
    for (uint32_t i = 0; i < AG_MODULE_MAX; i++) {
        if (!s_modules[i].used) {
            return &s_modules[i];
        }
    }
    return NULL;
}

static bool drop_module(module_t *m)
{
    if (m == NULL || !m->used) {
        return true;
    }
    /* While the image is still mapped: close net listens etc. */
    if (m->fini != NULL) {
        ag_module_fini_fn fini = m->fini;
        m->fini = NULL;
        fini();
    }

    /*
     * And then wait for the module's own tasks, which the hook above has just
     * been told to stop.  The hook is where a driver sets its flag; this is
     * where the flag is honoured, because a task still inside the image when
     * the image is unmapped is not an error anybody can catch - it is a jump
     * into memory that has been handed back.
     */
    if (!tasks_stopped(m)) {
        for (uint32_t waited = 0; waited < MODULE_TASK_WAIT_MS; waited += 10u) {
            ag_port_task_delay(ag_port_ms_to_ticks(10));
            if (tasks_stopped(m)) {
                break;
            }
        }
    }
    if (!tasks_stopped(m)) {
        ag_log(AG_LOG_ERROR, "modules",
               "%s: a task of its own has not stopped; keeping the image",
               m->app.header.name);
        return false;
    }

    /*
     * The registry is held across the revoke *and* the unload, because the
     * kernel calls into a driver's class vtable without going through a
     * handle: a panel's text_row and a touchscreen's poll are called straight
     * off dev->class_ops, from the console task, ten times a second.  Freeing
     * the arena those point into while that call is in flight is executing
     * memory that has just been given back - which took the board down with an
     * interrupt watchdog timeout the first time a driver was unloaded while it
     * was being polled.  Whoever calls a class op takes the same lock.
     */
    ag_dev_lock_hold();
    (void)ag_dev_revoke_owner(m);
    ag_loader_unload(&m->app);
    memset(m, 0, sizeof(*m));
    ag_dev_lock_release();
    return true;
}

ag_err_t ag_module_load_hinted(const char *path, const char *cwd,
                               const ag_probe_hint_t *hint)
{
    if (path == NULL || path[0] == '\0') {
        return -AG_EINVAL;
    }

    module_t *slot = alloc_slot();
    if (slot == NULL) {
        ag_log(AG_LOG_ERROR, "modules",
               "cannot load %s: already %u modules", path,
               (unsigned)AG_MODULE_MAX);
        return -AG_ENFILE;
    }

    memset(slot, 0, sizeof(*slot));
    ag_err_t err = ag_loader_load(path, cwd, &slot->app);
    if (err != AG_OK) {
        return err;
    }

    if ((slot->app.header.flags & AG_AXE_DRIVER) == 0) {
        ag_log(AG_LOG_ERROR, "modules",
               "%s: not a driver (missing AG_AXE_DRIVER)", path);
        ag_loader_unload(&slot->app);
        return -AG_EINVAL;
    }

    const char *name = slot->app.header.name;
    if (name[0] == '\0') {
        ag_loader_unload(&slot->app);
        return -AG_EFORMAT;
    }
    if (slot->app.binding.entry == NULL) {
        ag_loader_unload(&slot->app);
        return -AG_EFORMAT;
    }

    /*
     * Same header name → replace the resident image.  drv install copies a
     * fresh .SYS onto C: then load(); refusing with EEXIST left the old code
     * running ("already loaded") while the file on disk was new.
     */
    {
        module_t *existing = find_by_name(name);
        if (existing != NULL) {
            ag_log(AG_LOG_INFO, "modules", "replacing %s (%s → %s)", name,
                   existing->path, path);
            if (!drop_module(existing)) {
                ag_loader_unload(&slot->app);
                return -AG_EBUSY;
            }
        }
    }

    set_string_path(slot->path, sizeof(slot->path), path);
    slot->used = true;

    /*
     * The owner cookie is the module slot itself.  Anything registered during
     * init is tagged with it, and unload revokes by that pointer.  The probe
     * hint is visible for the same window, so the driver knows which address
     * matched without reading the config itself.
     */
    s_hint = hint;
    s_loading = slot;
    const ag_driver_init_fn init =
        (ag_driver_init_fn)(uintptr_t)slot->app.binding.entry;
    err = init();
    s_loading = NULL;
    s_hint = NULL;

    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, "modules", "%s: ag_driver_init returned %d", name,
               (int)err);
        (void)drop_module(slot);
        return err;
    }

    ag_log(AG_LOG_INFO, "modules", "loaded %s v%s from %s", name,
           slot->app.header.version, path);
    return AG_OK;
}

ag_err_t ag_module_load(const char *path, const char *cwd)
{
    return ag_module_load_hinted(path, cwd, NULL);
}

ag_err_t ag_module_unload(const char *name)
{
    module_t *m = find_by_name(name);
    if (m == NULL) {
        return -AG_ENOENT;
    }

    ag_log(AG_LOG_INFO, "modules", "unloading %s", m->app.header.name);
    if (!drop_module(m)) {
        return -AG_EBUSY;
    }
    return AG_OK;
}

ag_err_t ag_module_info(uint32_t index, ag_modinfo_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }

    uint32_t seen = 0;
    for (uint32_t i = 0; i < AG_MODULE_MAX; i++) {
        if (!s_modules[i].used) {
            continue;
        }
        if (seen == index) {
            memset(out, 0, sizeof(*out));
            memcpy(out->name, s_modules[i].app.header.name,
                   sizeof(out->name) - 1u);
            memcpy(out->version, s_modules[i].app.header.version,
                   sizeof(out->version) - 1u);
            memcpy(out->path, s_modules[i].path, sizeof(out->path) - 1u);
            out->code_bytes = s_modules[i].app.header.code.size;
            out->data_bytes = s_modules[i].app.header.data.size;
            return AG_OK;
        }
        seen++;
    }
    return -AG_ENOENT;
}

uint32_t ag_module_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < AG_MODULE_MAX; i++) {
        if (s_modules[i].used) {
            n++;
        }
    }
    return n;
}

/*
 * Older drv install wrote files under /sys/DRV while SYSTEM.CFG listed
 * c:\drv\… → /sys/drv/… . LittleFS is case-sensitive, so boot missed them.
 */
static ag_err_t load_device_path(const char *path)
{
    char       resolved[AG_PATH_MAX];
    char       alt[AG_PATH_MAX];
    ag_err_t   err;
    const char *rest;

    err = ag_module_load(path, NULL);
    if (err != -AG_ENOENT) {
        return err;
    }
    if (ag_path_resolve(path, NULL, resolved, sizeof(resolved)) != AG_OK) {
        return err;
    }
    if (strncmp(resolved, "/sys/drv/", 9) == 0) {
        rest = resolved + 9;
        if (snprintf(alt, sizeof(alt), "/sys/DRV/%s", rest) < (int)sizeof(alt)) {
            const ag_err_t err2 = ag_module_load(alt, NULL);
            if (err2 == AG_OK) {
                ag_log(AG_LOG_WARN, "modules",
                       "boot: loaded legacy path %s (use drv install again)",
                       alt);
                return AG_OK;
            }
        }
    } else if (strncmp(resolved, "/sys/DRV/", 9) == 0) {
        rest = resolved + 9;
        if (snprintf(alt, sizeof(alt), "/sys/drv/%s", rest) < (int)sizeof(alt)) {
            const ag_err_t err2 = ag_module_load(alt, NULL);
            if (err2 == AG_OK) {
                return AG_OK;
            }
        }
    }
    return err;
}

ag_err_t ag_modules_boot(void)
{
    const ag_cfg_t *cfg = ag_sysconfig();
    if (cfg == NULL) {
        return AG_OK;
    }

    size_t      it = 0;
    const char *path;
    uint32_t    loaded = 0;
    uint32_t    failed = 0;

    while ((path = ag_cfg_next(cfg, "modules.device", &it)) != NULL) {
        const ag_err_t err = load_device_path(path);
        if (err == AG_OK) {
            loaded++;
        } else {
            failed++;
            ag_log(AG_LOG_WARN, "modules", "boot: %s failed (%d)", path,
                   (int)err);
        }
    }

    if (loaded > 0 || failed > 0) {
        ag_log(AG_LOG_INFO, "modules", "boot: %u loaded, %u failed",
               (unsigned)loaded, (unsigned)failed);
    }

    /* After the explicit list: pick up whatever the buses can see. */
    uint32_t probed = 0;
    uint32_t missed = 0;
    (void)ag_probe_run(&probed, &missed);
    return AG_OK;
}
