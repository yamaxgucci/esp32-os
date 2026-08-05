/*
 * ArgonOS - configuration files (SYSTEM.CFG, BOARD.CFG).
 *
 * The format is deliberately the one every DOS user already knows: sections
 * in brackets, KEY=VALUE lines, ';' or '#' comments.
 *
 *     ; A:\SYSTEM.CFG
 *     [kernel]
 *     app_core_exclusive = yes
 *     app_text_arena     = 2M
 *
 *     [display]
 *     driver = st7789
 *     width  = 320
 *
 *     [modules]
 *     device = /sd/drv/bme280.sys
 *     device = /sd/drv/mcp23017.sys
 *
 * Parsing is zero-copy: the text buffer is chopped up in place and the table
 * holds pointers into it, so the buffer must outlive the ag_cfg_t.
 *
 * This file is free of kernel and ESP-IDF dependencies so it can be unit
 * tested on a host.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_CFG_H
#define ARGON_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AG_CFG_MAX_ENTRIES 128

typedef struct {
    const char *section; /* "" for keys before the first [section] */
    const char *key;
    const char *value;
    uint16_t    line;
} ag_cfg_entry_t;

typedef struct {
    ag_cfg_entry_t entries[AG_CFG_MAX_ENTRIES];
    uint16_t       count;
    uint16_t       dropped;    /* entries lost to the table being full  */
    uint16_t       bad_lines;  /* lines that were not KEY=VALUE         */
} ag_cfg_t;

/* Clears the table.  Call before the first ag_cfg_parse() into it. */
void ag_cfg_reset(ag_cfg_t *cfg);

/*
 * Parses `text` in place and appends the result to `cfg`.  Parsing several
 * files into one table is intentional: a board pack can be parsed first and a
 * user file second, with later entries overriding earlier ones.
 *
 * Returns AG_OK even when individual lines were malformed; check `bad_lines`
 * and `dropped` to report a warning.  Returns -AG_EINVAL only on NULL input.
 */
ag_err_t ag_cfg_parse(char *text, ag_cfg_t *cfg);

/*
 * Lookup by "section.key".  A key with no dot is looked up in the unnamed
 * section.  The most recently parsed matching entry wins.
 */
const char *ag_cfg_get(const ag_cfg_t *cfg, const char *dotted_key,
                       const char *fallback);
int32_t ag_cfg_get_int(const ag_cfg_t *cfg, const char *dotted_key,
                       int32_t fallback);
bool ag_cfg_get_bool(const ag_cfg_t *cfg, const char *dotted_key,
                     bool fallback);

/*
 * Iterates every value of a repeated key, oldest first.  Start with
 * *iter = 0; returns NULL when there are no more.
 *
 *     size_t it = 0;
 *     const char *path;
 *     while ((path = ag_cfg_next(&cfg, "modules.device", &it)) != NULL) { ... }
 */
const char *ag_cfg_next(const ag_cfg_t *cfg, const char *dotted_key,
                        size_t *iter);

/*
 * Parses "1024", "0x400", "16K", "2M" into a byte count.  Returns `fallback`
 * when the text is not a valid size.
 */
int32_t ag_cfg_parse_size(const char *text, int32_t fallback);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_CFG_H */
