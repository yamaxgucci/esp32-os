/*
 * Minimal libc stand-in header for -nostdlib .AXE builds.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_SMS_STDLIB_H
#define ARGON_SMS_STDLIB_H

#include <stddef.h>
#include <stdint.h>

void *malloc(size_t n);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void  free(void *p);
void  exit(int code);
void  abort(void);
int   abs(int x);
long  labs(long x);
int   atoi(const char *s);
double atof(const char *s);
long  strtol(const char *s, char **end, int base);
char *strdup(const char *s);
char *getenv(const char *name);
int   system(const char *cmd);

#endif
