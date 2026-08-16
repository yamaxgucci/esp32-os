/*
 * Minimal libc stand-in header for -nostdlib .AXE builds.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_SMS_SYS_STAT_H
#define ARGON_SMS_SYS_STAT_H
#include <sys/types.h>
int mkdir(const char *path, mode_t mode);
#endif
