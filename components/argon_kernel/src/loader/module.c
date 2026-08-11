/*
 * ArgonOS - loadable .SYS driver modules.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/module.h>

#include <stdio.h>
#include <string.h>

#include <argon/cfg.h>
#include <argon/device.h>
#include <argon/log.h>
#include <argon/path.h>
#include <argon/probe.h>

#include "core/sysconfig.h"

typedef void (*ag_module_fini_fn)(void);

typedef struct {
    bool             used;
    char             path[AG_PATH_MAX];
    ag_loaded_app_t  app;
    ag_module_fini_fn fini;
} module_t;

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

static void drop_module(module_t *m)
{
    if (m == NULL || !m->used) {
        return;
    }
    /* While the image is still mapped: close net listens etc. */
    if (m->fini != NULL) {
        ag_module_fini_fn fini = m->fini;
        m->fini = NULL;
        fini();
    }
    (void)ag_dev_revoke_owner(m);
    ag_loader_unload(&m->app);
    memset(m, 0, sizeof(*m));
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
            drop_module(existing);
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
        drop_module(slot);
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
    drop_module(m);
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
