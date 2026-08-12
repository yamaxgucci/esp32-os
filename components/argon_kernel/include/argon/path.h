/*
 * ArgonOS - path handling.
 *
 * ArgonOS uses POSIX style paths internally ("/sd/apps/hello.axe") but the
 * shell and applications ported from DOS habits may hand us drive letters,
 * backslashes and mixed separators.  Everything is funnelled through
 * ag_path_resolve() before it reaches the VFS, so the rest of the kernel only
 * ever sees canonical paths.
 *
 * This file is free of kernel and ESP-IDF dependencies so it can be unit
 * tested on a host.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PATH_H
#define ARGON_PATH_H

#include <stdbool.h>
#include <stddef.h>

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AG_PATH_MAX and AG_NAME_MAX are in argon/abi.h: applications size buffers by
 * them too, so they belong to the contract rather than to the kernel. */

/*
 * Drive letter aliases, DOS style.  The table is fixed at build time and
 * mirrors the standard mount points; a board pack may extend it.
 *
 *   A: -> /sd     removable card
 *   C: -> /sys    internal flash
 *   T: -> /tmp    ram disk
 *   D: -> /dev    devices
 */
const char *ag_path_drive(char letter);

/*
 * Turns `in` into a canonical absolute path.
 *
 *   - backslashes become slashes;
 *   - a leading drive letter is expanded through ag_path_drive();
 *   - relative paths are resolved against `cwd` (NULL means "/");
 *   - "." and ".." are folded, ".." at the root is a no-op;
 *   - redundant and trailing separators are dropped, root stays "/".
 *
 * Returns AG_OK, -AG_EINVAL for a malformed drive letter or NULL output, or
 * -AG_ERANGE when the result does not fit in `outlen`.
 */
ag_err_t ag_path_resolve(const char *in, const char *cwd, char *out,
                         size_t outlen);

/* Appends `child` to `base` with a single separator, then canonicalises. */
ag_err_t ag_path_join(const char *base, const char *child, char *out,
                      size_t outlen);

/*
 * Pointer to the last component of a canonical path.  Never NULL; returns a
 * pointer into `path`, or "" for the root.
 */
const char *ag_path_basename(const char *path);

/* Copies everything up to the last separator; "/a/b" -> "/a", "/a" -> "/". */
ag_err_t ag_path_dirname(const char *path, char *out, size_t outlen);

/*
 * Pointer to the extension including the dot ("/a/b.axe" -> ".axe"), or NULL.
 * A leading dot in the basename is not an extension (".config").
 */
const char *ag_path_ext(const char *path);

/* Case insensitive comparison, as used by FAT. */
int ag_path_icmp(const char *a, const char *b);

/*
 * DOS style wildcard match over a single path component.
 *   '*' matches any run of characters, '?' matches exactly one.
 * Matching is case insensitive, like the shells the syntax comes from.
 */
bool ag_path_match(const char *pattern, const char *name);

/* True when the path is already absolute and canonical. */
bool ag_path_is_absolute(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_PATH_H */
