/*
 * Minimal libc stand-in header for -nostdlib .AXE builds.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_SMS_STDLIB_H
#define ARGON_SMS_STDLIB_H

#include <stddef.h>
#include <stdint.h>

void *malloc(size_t n);

/* The least the arena has ever had free, watched across big allocations that a
 * once-a-frame sampler cannot see.  (size_t)-1 until one happens. */
size_t ag_libc_arena_low(void);

/* Print the first max_reports allocations of min_bytes or more, with the
 * address they were asked from.  Off until called with a non-zero min_bytes;
 * for finding out who wants the block that will not fit, and who took the
 * memory it would have gone in. */
void ag_libc_malloc_trace(size_t min_bytes, int max_reports);

/* Count allocations by the call site four frames up, rather than printing
 * them.  For memory that arrives as hundreds of small blocks, which naming
 * the big ones cannot see.  _dump prints the top sites and clears the table,
 * so each dump measures one window. */
/* A second place for the out-of-arena line to go; NULL for none. */
void ag_libc_log_sink(void (*sink)(const char *line));

/*
 * Called when an allocation has been refused, before the caller is told.  It
 * should give memory back if it can and return non-zero if it did; the
 * allocation is then tried once more.  See the note in libc_shim.c: the
 * failure that matters on this machine is fragmentation, and the program that
 * keeps a cache is the one that can do something about it.
 */
void ag_libc_oom_sink(int (*sink)(size_t wanted));
/* ARGON: a lender of last resort - malloc asks it when the arena is empty, and
 * offers every pointer to the giveback before freeing.  See libc_shim.c. */
void ag_libc_borrow_sink(void *(*borrow)(size_t wanted),
                         int (*giveback)(void *p, size_t *sizePtr));
extern unsigned ag_libc_oom_rescues;
void ag_libc_malloc_profile(int on);
void ag_libc_malloc_profile_dump(int top);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void  free(void *p);
void  exit(int code);
int   atexit(void (*fn)(void));
void  abort(void);
int   abs(int x);
long  labs(long x);
int   atoi(const char *s);
double atof(const char *s);
double strtod(const char *s, char **end);
long  strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
char *strdup(const char *s);
char *getenv(const char *name);
int   system(const char *cmd);

#endif
