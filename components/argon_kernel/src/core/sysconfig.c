/*
 * ArgonOS - system configuration files.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "core/sysconfig.h"

#include <stdio.h>
#include <string.h>

#include <argon/board.h>
#include <argon/codepage.h>
#include <argon/loader.h>
#include <argon/log.h>
#include <argon/vfs.h>

#include <argon/port/mem.h>

/*
 * One buffer holds every configuration file, laid end to end.  4 KB is a lot of
 * configuration; running out is reported rather than silently truncating, since
 * a half-read config file is worse than none.
 */
#define AG_CFG_TEXT_BYTES 4096

static ag_cfg_t s_cfg;
static char    *s_text;
static size_t   s_text_used;
static char     s_sources[64];

const ag_cfg_t *ag_sysconfig(void) { return &s_cfg; }
const char     *ag_sysconfig_sources(void) { return s_sources; }

static void note_source(const char *name)
{
    const size_t len = strlen(s_sources);
    snprintf(s_sources + len, sizeof(s_sources) - len, "%s%s",
             (len > 0) ? ", " : "", name);
}

/*
 * Appends a file to the shared buffer and parses it.  Returns -AG_ENOENT when
 * the file is simply not there, which is the common case and not a problem.
 */
static ag_err_t load_one(const char *path, const char *label)
{
    const ag_handle_t h = ag_vfs_open(path, NULL, AG_O_RDONLY);
    if (h < 0) {
        return h;
    }

    char  *dst = s_text + s_text_used;
    size_t room = AG_CFG_TEXT_BYTES - s_text_used;
    if (room < 2) {
        ag_vfs_close(h);
        return -AG_ENOSPC;
    }

    /* Leave a byte for the terminator the parser needs. */
    const int32_t n = ag_vfs_read(h, dst, room - 1);
    ag_vfs_close(h);

    if (n < 0) {
        return n;
    }
    dst[n] = '\0';
    s_text_used += (size_t)n + 1;

    const ag_err_t err = ag_cfg_parse(dst, &s_cfg);
    if (err == AG_OK) {
        note_source(label);
    }
    return err;
}

ag_err_t ag_sysconfig_init(void)
{
    if (s_text == NULL) {
        s_text = (char *)ag_port_alloc(AG_CFG_TEXT_BYTES,
                                       AG_MEM_SLOW | AG_MEM_BYTE);
        if (s_text == NULL) {
            s_text = (char *)ag_port_alloc(AG_CFG_TEXT_BYTES,
                                           AG_MEM_FAST | AG_MEM_BYTE);
        }
        if (s_text == NULL) {
            return -AG_ENOMEM;
        }
    }

    ag_cfg_reset(&s_cfg);
    s_text_used = 0;
    s_sources[0] = '\0';

    /*
     * BOARD.CFG first so that SYSTEM.CFG can override it: the board file
     * describes the hardware, the system file describes this installation, and
     * the more specific of the two should win.
     */
    (void)load_one("/sys/BOARD.CFG", "BOARD.CFG");
    (void)load_one("/sys/SYSTEM.CFG", "SYSTEM.CFG");

    /*
     * The code page is applied here rather than in the console stage, which runs
     * earlier: an installation whose files are Cyrillic should not have to type
     * chcp after every boot, and boot messages are ASCII either way.
     */
    const int32_t page = ag_cfg_get_int(&s_cfg, "console.codepage", 0);
    if (page != 0) {
        ag_cp_t chosen;
        if (ag_cp_from_number((uint16_t)page, &chosen)) {
            ag_cp_set_active(chosen);
        } else {
            ag_log(AG_LOG_WARN, "config", "no such code page: %d", (int)page);
        }
    }

    /*
     * Code arena: the linked buffer is a ceiling (CONFIG_ARGON_APP_ARENA_KB).
     * [memory] app_arena_kb= clips how much of it the loader may hand out.
     * Missing key → full ceiling.  Needs reboot to take effect after a change.
     */
    {
        const char *raw = ag_cfg_get(&s_cfg, "memory.app_arena_kb", NULL);
        if (raw != NULL) {
            const int32_t want = ag_cfg_get_int(&s_cfg, "memory.app_arena_kb", 0);
            if (want <= 0) {
                ag_log(AG_LOG_WARN, "config",
                       "memory.app_arena_kb=%s ignored (need positive KB)", raw);
            } else {
                const size_t usable =
                    ag_loader_set_arena_kb((uint32_t)want);
                ag_log(AG_LOG_INFO, "config",
                       "app code arena %u KB (asked %d)",
                       (unsigned)(usable / 1024u), (int)want);
            }
        }
    }

    /* Hardware description reaches the board layer even if nothing was read:
     * it is then a no-op, and the defaults stand. */
    return ag_board_apply_config(&s_cfg);
}
