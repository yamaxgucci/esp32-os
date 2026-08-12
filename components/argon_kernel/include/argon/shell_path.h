/*
 * ArgonOS - PATH search and file associations (portable).
 *
 * Resolves a bare command name against a DOS-style PATH string and looks up
 * file associations from a parsed SYSTEM.CFG table.  Free of FreeRTOS /
 * ESP-IDF so host tests can cover the logic.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_SHELL_PATH_H
#define ARGON_SHELL_PATH_H

#include <stdbool.h>
#include <stddef.h>

#include <argon/cfg.h>
#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * True when `name` already names a place (drive, absolute, or relative with
 * a directory separator) and should not be walked through PATH.
 */
bool ag_shell_name_is_path(const char *name);

/*
 * Resolves `name` to an existing regular file.
 *
 * When `name` is already a path, only `ag_path_resolve` + VFS stat are used.
 * Otherwise each `;`-separated entry in `path_env` (NULL or "" = no search) is
 * tried with `name`, then `name.axe` / `name.AXE`.  `cwd` is the process cwd
 * for relative PATH entries.  On success writes a canonical absolute path to
 * `out` and returns AG_OK; otherwise -AG_ENOENT / -AG_EINVAL / -AG_ERANGE.
 */
ag_err_t ag_shell_resolve_cmd(const char *name, const char *cwd,
                              const char *path_env, char *out, size_t outlen);

/*
 * Looks up an association for the extension of `filename` (including the dot)
 * in `[assoc]` of `cfg`.  Keys are compared case-insensitively (".txt" matches
 * ".TXT").  Returns the handler value, or NULL.
 */
const char *ag_shell_assoc_lookup(const ag_cfg_t *cfg, const char *filename);

/* True for empty lines and REM / ; / # comments used in AUTOEXEC.BAT. */
bool ag_shell_autoexec_skip_line(const char *line);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_SHELL_PATH_H */
