/*
 * ArgonOS - where an application keeps what it remembers.
 *
 * `C:\CFG\<APP>.CFG`, one file per program, named after the program's own name
 * from its image header - so a program cannot write another's settings, and
 * does not have to be trusted not to: the name is not an argument.
 *
 * It is a FACILITY rather than a convention because the alternative is every
 * image inventing its own file, path and format, and then a person with a card
 * in a PC finding none of them.  The shape is the one SYSTEM.CFG already has -
 * `key = value`, comments with `;` - because it is the shape this tree already
 * parses and documents, and because somebody will open this file in an editor.
 *
 * Settings are written whole: read the file, write it back with one line
 * changed, through a temporary.  A settings write is a rare event (the shop
 * writes two, once), so the cost of a rewrite is not worth a journal, but the
 * temporary is: a power cut in the middle of `key = value` must leave the old
 * file, not half of the new one.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_APPCFG_H
#define ARGON_APPCFG_H

#include <argon/abi.h>

/* The directory all of them live in, as the kernel addresses it. */
#define AG_APPCFG_DIR "/sys/CFG"

/* `C:\CFG\NAME.CFG` for a program name, as a person would type it. */
ag_err_t ag_appcfg_dos_path(const char *app, char *out, size_t len);

/* The calling process's settings.  -AG_ENOENT when it has never written one. */
ag_err_t ag_appcfg_get(const char *key, char *out, size_t len);
int32_t  ag_appcfg_get_int(const char *key, int32_t fallback);
ag_err_t ag_appcfg_set(const char *key, const char *value);

#endif /* ARGON_APPCFG_H */
