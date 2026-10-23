/*
 * ArgonOS - an application's own settings, as text.
 *
 * The file is `C:\CFG\<APP>.CFG`: `key = value` lines, the same shape as
 * SYSTEM.CFG, because a person may well open it and a format nobody can read is
 * a format nobody can fix.  It is small on purpose - an application remembers a
 * handful of things, not a database - and every operation here is pure: the
 * caller has the bytes and gets bytes back.  Reading and writing the file is
 * appcfg.c's job, and the split is what lets a host test cover the whole of this
 * file with no filesystem at all.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_APPCFG_TEXT_H
#define ARGON_APPCFG_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A settings line, and the file as a whole.  Both are deliberately small: the
 * budget is a few keys, and a file that grew past this is a file with something
 * else in it. */
#define AG_APPCFG_KEY_MAX 48
#define AG_APPCFG_VALUE_MAX 128
#define AG_APPCFG_TEXT_MAX 1024

/*
 * The value of `key`, or false when the file does not have it.  Quotes around
 * the value are taken off, and an unquoted `;` or `#` ends it - the same rules
 * SYSTEM.CFG follows, so "path = "C:\APPS;A:\APPS"" means what it looks like.
 */
bool ag_appcfg_text_get(const char *text, const char *key, char *out,
                        size_t len);

/* ... and the same key as a number, in decimal or 0x hex. */
int32_t ag_appcfg_text_get_int(const char *text, const char *key,
                               int32_t fallback);

/*
 * `in`, with `key = value` set: the line is replaced where it already is, and
 * appended when it is not.  Returns the length written into `out`, or 0 when it
 * would not fit - the caller writes nothing then, so a full file cannot lose
 * the settings it already had.
 */
size_t ag_appcfg_text_set(const char *in, const char *key, const char *value,
                          char *out, size_t outlen);

/*
 * An application name as a filename: upper case, and nothing that is not a
 * letter, a digit, `_` or `-`.  Two programs whose names differ only in case or
 * punctuation must not end up sharing a file, and a name with a path separator
 * in it must not become a path.
 */
void ag_appcfg_file_name(const char *app, char *out, size_t len);

#endif /* ARGON_APPCFG_TEXT_H */
