/*
 * ArgonOS - matching .SYS modules to chips on I2C.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/probe.h>

#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_IDF_TARGET
#include <argon/cfg.h>
#include <argon/loader.h>
#include <argon/log.h>
#include <argon/module.h>
#include <argon/path.h>

#include "core/sysconfig.h"
#endif

static const char *skip_ws(const char *s)
{
    while (s != NULL && (*s == ' ' || *s == '\t')) {
        s++;
    }
    return s;
}

static bool parse_u8(const char *text, const char *end, uint8_t *out)
{
    if (text == NULL || text >= end || out == NULL) {
        return false;
    }
    char   tmp[16];
    size_t n = (size_t)(end - text);
    if (n == 0 || n >= sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, text, n);
    tmp[n] = '\0';

    char *stop = NULL;
    unsigned long v = strtoul(tmp, &stop, 0);
    if (stop == tmp || *stop != '\0' || v > 0xfful) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

static bool parse_int(const char *text, const char *end, int *out)
{
    if (text == NULL || text >= end || out == NULL) {
        return false;
    }
    char   tmp[16];
    size_t n = (size_t)(end - text);
    if (n == 0 || n >= sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, text, n);
    tmp[n] = '\0';

    char *stop = NULL;
    long  v = strtol(tmp, &stop, 0);
    if (stop == tmp || *stop != '\0' || v < 0 || v > 255) {
        return false;
    }
    *out = (int)v;
    return true;
}

ag_err_t ag_probe_parse(const char *line, ag_probe_entry_t *out)
{
    if (line == NULL || out == NULL) {
        return -AG_EINVAL;
    }

    line = skip_ws(line);
    if (line[0] == '\0') {
        return -AG_EINVAL;
    }

    const char *c1 = strchr(line, ':');
    if (c1 == NULL) {
        return -AG_EINVAL;
    }
    const char *c2 = strchr(c1 + 1, ':');
    if (c2 == NULL) {
        return -AG_EINVAL;
    }

    memset(out, 0, sizeof(*out));
    if (!parse_int(line, c1, &out->hint.bus)) {
        return -AG_EINVAL;
    }
    if (!parse_u8(c1 + 1, c2, &out->hint.addr)) {
        return -AG_EINVAL;
    }
    if (out->hint.addr > 0x7f) {
        return -AG_EINVAL;
    }

    const char *rest = c2 + 1;
    /* Optional id field: <reg>=<val>:path */
    const char *eq = strchr(rest, '=');
    const char *c3 = strchr(rest, ':');
    if (eq != NULL && c3 != NULL && eq < c3) {
        if (!parse_u8(rest, eq, &out->hint.id_reg) ||
            !parse_u8(eq + 1, c3, &out->hint.id_val)) {
            return -AG_EINVAL;
        }
        out->hint.has_id = true;
        rest = c3 + 1;
    }

    rest = skip_ws(rest);
    if (rest[0] == '\0') {
        return -AG_EINVAL;
    }

    /* Trim trailing whitespace on the path. */
    size_t n = strlen(rest);
    while (n > 0 && (rest[n - 1u] == ' ' || rest[n - 1u] == '\t')) {
        n--;
    }
    if (n == 0 || n >= sizeof(out->path)) {
        return -AG_EINVAL;
    }
    memcpy(out->path, rest, n);
    out->path[n] = '\0';
    return AG_OK;
}

#ifdef CONFIG_IDF_TARGET

static bool path_already_loaded(const char *path)
{
    for (uint32_t i = 0;; i++) {
        ag_modinfo_t info;
        if (ag_module_info(i, &info) != AG_OK) {
            break;
        }
        if (ag_path_icmp(info.path, path) == 0) {
            return true;
        }
    }
    return false;
}

static ag_err_t chip_present(const ag_probe_entry_t *e)
{
    const ag_api_t *api = ag_loader_api();
    if (api == NULL || api->io == NULL || api->io->i2c_probe == NULL) {
        return -AG_ENOSYS;
    }

    const ag_err_t ack = api->io->i2c_probe(e->hint.bus, e->hint.addr);
    if (ack != AG_OK) {
        return ack;
    }
    if (!e->hint.has_id) {
        return AG_OK;
    }
    if (api->io->i2c_wrrd == NULL) {
        return -AG_ENOSYS;
    }

    uint8_t       id = 0;
    const uint8_t reg = e->hint.id_reg;
    const ag_err_t err =
        api->io->i2c_wrrd(e->hint.bus, e->hint.addr, &reg, 1, &id, 1, 50);
    if (err != AG_OK) {
        return err;
    }
    return (id == e->hint.id_val) ? AG_OK : -AG_ENOENT;
}

ag_err_t ag_probe_run(uint32_t *loaded_out, uint32_t *missed_out)
{
    const ag_cfg_t *cfg = ag_sysconfig();
    uint32_t        loaded = 0;
    uint32_t        missed = 0;
    uint32_t        bad = 0;

    if (cfg == NULL) {
        if (loaded_out != NULL) {
            *loaded_out = 0;
        }
        if (missed_out != NULL) {
            *missed_out = 0;
        }
        return AG_OK;
    }

    size_t      it = 0;
    const char *line;
    while ((line = ag_cfg_next(cfg, "modules.probe", &it)) != NULL) {
        ag_probe_entry_t entry;
        if (ag_probe_parse(line, &entry) != AG_OK) {
            bad++;
            ag_log(AG_LOG_WARN, "probe", "bad entry: %s", line);
            continue;
        }

        if (path_already_loaded(entry.path)) {
            ag_log(AG_LOG_INFO, "probe", "%s already loaded", entry.path);
            continue;
        }

        const ag_err_t present = chip_present(&entry);
        if (present == -AG_ENODEV) {
            /* Bus not configured - say so once per entry, count as missed. */
            missed++;
            ag_log(AG_LOG_WARN, "probe", "i2c%d not configured (need BOARD.CFG)",
                   entry.hint.bus);
            continue;
        }
        if (present != AG_OK) {
            missed++;
            continue;
        }

        const ag_err_t err =
            ag_module_load_hinted(entry.path, NULL, &entry.hint);
        if (err == AG_OK) {
            loaded++;
            if (entry.hint.has_id) {
                ag_log(AG_LOG_INFO, "probe",
                       "i2c%d:0x%02x id 0x%02x=0x%02x -> %s", entry.hint.bus,
                       entry.hint.addr, entry.hint.id_reg, entry.hint.id_val,
                       entry.path);
            } else {
                ag_log(AG_LOG_INFO, "probe", "i2c%d:0x%02x -> %s",
                       entry.hint.bus, entry.hint.addr, entry.path);
            }
        } else {
            bad++;
            ag_log(AG_LOG_WARN, "probe", "%s: load failed (%d)", entry.path,
                   (int)err);
        }
    }

    if (loaded_out != NULL) {
        *loaded_out = loaded;
    }
    if (missed_out != NULL) {
        *missed_out = missed;
    }
    if (loaded > 0 || missed > 0 || bad > 0) {
        ag_log(AG_LOG_INFO, "probe", "%u loaded, %u absent, %u failed",
               (unsigned)loaded, (unsigned)missed, (unsigned)bad);
    }
    return AG_OK;
}

#else /* host build: parse is tested; run needs real buses */

ag_err_t ag_probe_run(uint32_t *loaded_out, uint32_t *missed_out)
{
    if (loaded_out != NULL) {
        *loaded_out = 0;
    }
    if (missed_out != NULL) {
        *missed_out = 0;
    }
    return -AG_ENOSYS;
}

#endif
