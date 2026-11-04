/*
 * The few C library calls the agent and miniz make, on top of the Win32 API
 * alone - so the executable needs no C runtime DLL at all.
 *
 * Why: a MinGW build links msvcrt or UCRT, and neither is there on every
 * Windows this has to run on (UCRT arrived with Windows 10; the msvcrt on 98 is
 * older than anything a current toolchain expects).  kernel32, user32 and gdi32
 * are there on all of them, from 98 to 11, so the agent uses nothing else.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef KVMAGENT_COMPAT_H
#define KVMAGENT_COMPAT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef struct agent_file FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int   printf(const char *fmt, ...);
int   fprintf(FILE *f, const char *fmt, ...);
int   snprintf(char *buf, size_t cap, const char *fmt, ...);
int   vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int   fflush(FILE *f);
char *fgets(char *buf, int cap, FILE *f);
FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *f);

void *malloc(size_t n);
void *calloc(size_t a, size_t b);
void *realloc(void *p, size_t n);
void  free(void *p);

void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcat(char *d, const char *s);
char  *strrchr(const char *s, int c);
int    atoi(const char *s);
#define isalnum agent_isalnum
int    agent_isalnum(int c);

#endif /* KVMAGENT_COMPAT_H */
