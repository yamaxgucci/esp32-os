/*
 * ArgonOS - system configuration files (kernel private).
 *
 * Loads BOARD.CFG and SYSTEM.CFG from internal flash into one table, then hands
 * the board overrides to the board layer.  Parsing is zero-copy, so the text
 * buffer is kept for the lifetime of the system rather than freed.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_SYSCONFIG_H
#define ARGON_SYSCONFIG_H

#include <argon/cfg.h>

/*
 * Reads what is there and applies it.  Missing files are not an error: a board
 * with no configuration runs on its defaults, which is the normal state of a
 * board that has just been flashed.
 */
ag_err_t ag_sysconfig_init(void);

const ag_cfg_t *ag_sysconfig(void);

/* Files that were found and parsed, for the boot report and the shell. */
const char *ag_sysconfig_sources(void);

#endif /* ARGON_SYSCONFIG_H */
