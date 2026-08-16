/*
 * Minimal libc stand-in header for -nostdlib .AXE builds.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_SMS_UNISTD_H
#define ARGON_SMS_UNISTD_H

#include <stddef.h>

int access(const char *path, int mode);

#ifndef F_OK
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
#endif

#endif
