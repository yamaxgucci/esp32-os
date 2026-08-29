/*
 * ArgonOS - POSIX directory reading, over the ArgonOS filesystem ABI.
 *
 * The engine enumerates a directory in exactly one place, db.cc, to find the
 * .DAT files and the loose game folders next to them.  It wants opendir,
 * readdir, closedir and d_type - and d_type is the part newlib does not have,
 * because newlib does not have a filesystem to ask.
 *
 * This header is on the include path ahead of the toolchain's, so <dirent.h>
 * in the engine finds this one.  That is deliberate and it is the reason the
 * engine itself needs no patch.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_FALLOUT_DIRENT_H
#define ARGON_FALLOUT_DIRENT_H

#include <argon/argon.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Only the two the engine tests for.  A name that is not a directory is a
 * regular file as far as this OS is concerned: there are no symlinks, no
 * sockets and no device nodes outside /dev, which is not walked here. */
#define DT_UNKNOWN 0
#define DT_REG 8
#define DT_DIR 4

struct dirent {
    unsigned char d_type;
    char d_name[256];
};

typedef struct DIR DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_FALLOUT_DIRENT_H */
