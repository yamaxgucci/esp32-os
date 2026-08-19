/*
 * ArgonOS - system configuration files.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "core/sysconfig.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <argon/board.h>
#include <argon/codepage.h>
#include <argon/loader.h>
#include <argon/log.h>
#include <argon/vfs.h>

#include <argon/port/mem.h>

#define TAG "cfg"

/*
 * One buffer holds every configuration file, laid end to end - the parser keeps
 * pointers into it, so it has to stay.
 *
 * 4 KB is a great deal of configuration and almost no documentation, which is
 * why comment lines never get here: load_one reads a line at a time and keeps
 * only what the parser needs.  The board pack in tree is over four kilobytes of
 * which some three hundred bytes are settings, so it would not otherwise fit at
 * all.  Running out is reported rather than silently truncating - a half-read
 * config file is worse than none, and it is worse still when nothing says so.
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
 * Longest line a configuration file may have.  A section header, a key and a
 * path fit comfortably; a comment does not have to, because comments never
 * reach here.
 */
#define AG_CFG_LINE_MAX 160

typedef struct {
    char  *out;      /* where kept lines go: into the shared text buffer */
    size_t room;     /* how much of it is left                          */
    size_t used;
    char   line[AG_CFG_LINE_MAX];
    size_t line_len;
    bool   overflow;  /* a kept line did not fit in the shared buffer   */
    bool   long_line; /* a line was longer than AG_CFG_LINE_MAX         */
} cfg_sink_t;

/*
 * One finished line, kept or dropped.
 *
 * Comments and blank lines are dropped here rather than after the whole file is
 * read, and that is the difference between "a configuration file may be as long
 * as it likes" and "4 KB, comments included".  The parser keeps its entries as
 * pointers into the shared buffer, so whatever is kept has to stay resident -
 * but there is no reason for the prose to stay with it, and the board pack in
 * tree is four kilobytes of which some three hundred bytes are settings.
 *
 * Reading first and stripping afterwards was the previous shape, and it failed
 * the moment BOARD.CFG grew past the buffer: the read was truncated before
 * anything could be dropped.
 */
static void sink_line(cfg_sink_t *sk)
{
    size_t first = 0;
    while (first < sk->line_len &&
           (sk->line[first] == ' ' || sk->line[first] == '\t' ||
            sk->line[first] == '\r')) {
        first++;
    }

    const bool keep = (first < sk->line_len && sk->line[first] != ';' &&
                       sk->line[first] != '#');
    if (keep) {
        const size_t n = sk->line_len - first;
        if (sk->used + n + 1u < sk->room) {
            memcpy(sk->out + sk->used, sk->line + first, n);
            sk->used += n;
            sk->out[sk->used++] = '\n';
        } else {
            sk->overflow = true;
        }
    }
    sk->line_len = 0;
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

    cfg_sink_t sk = {0};
    sk.out = s_text + s_text_used;
    sk.room = AG_CFG_TEXT_BYTES - s_text_used;
    if (sk.room < 2) {
        ag_vfs_close(h);
        ag_log(AG_LOG_WARN, TAG, "%s: no room left for it (%u of %u used)",
               label, (unsigned)s_text_used, (unsigned)AG_CFG_TEXT_BYTES);
        return -AG_ENOSPC;
    }

    char chunk[128];
    for (;;) {
        const int32_t n = ag_vfs_read(h, chunk, sizeof(chunk));
        if (n <= 0) {
            break;
        }
        for (int32_t i = 0; i < n; i++) {
            const char c = chunk[i];
            if (c == '\n') {
                sink_line(&sk);
            } else if (sk.line_len + 1u < sizeof(sk.line)) {
                sk.line[sk.line_len++] = c;
            } else {
                sk.long_line = true;
            }
        }
    }
    sink_line(&sk); /* a last line with no newline after it */
    ag_vfs_close(h);

    sk.out[sk.used] = '\0';
    s_text_used += sk.used + 1;

    if (sk.overflow) {
        ag_log(AG_LOG_WARN, TAG,
               "%s: settings past %u bytes are ignored - the buffer is full",
               label, (unsigned)sk.used);
    }
    if (sk.long_line) {
        ag_log(AG_LOG_WARN, TAG, "%s: a line over %u characters was cut",
               label, (unsigned)AG_CFG_LINE_MAX);
    }

    const ag_err_t err = ag_cfg_parse(sk.out, &s_cfg);
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
