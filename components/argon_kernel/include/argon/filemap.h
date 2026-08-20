/*
 * ArgonOS - a file's bytes at an address, read only.  See src/fs/filemap.c.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_FILEMAP_H
#define ARGON_FILEMAP_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stages `path` into flash and maps it where it can be read as bytes.
 *
 * The pointer is good until ag_filemap_close, and the bytes are read only -
 * writing through it is a fault, not a change.  The staging is a full copy into
 * the appfs partition, so this costs as long as writing that many bytes of
 * flash and is worth it only for data that will be read a great many times.
 */
ag_err_t ag_filemap_open(const char *path, const char *cwd, const void **out,
                         uint64_t *out_len);

/* Gives back one mapping, by the pointer it handed out. */
ag_err_t ag_filemap_close(const void *ptr);

/* Every mapping, for process teardown.  Returns how many there were. */
uint32_t ag_filemap_release_all(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_FILEMAP_H */
