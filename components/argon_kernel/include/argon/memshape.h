/*
 * ArgonOS - buffers that can be made again elsewhere.
 *
 * A heap cannot move a block: whoever holds the pointer has to.  So whoever
 * keeps a long-lived buffer it could rebuild - a staging buffer, a cache, a
 * table it can make again - says so here, and the kernel asks for the rebuild
 * when the memory around has changed enough for the new place to be a better
 * one: after something large went away (`wifi off`, an application's end, the
 * card taken out), before an application is refused memory, or when asked
 * (`mem compact`).
 *
 * The callback runs in the task that triggered it, with nothing of the
 * kernel's held.  It takes its own lock, makes sure no one is using the
 * buffer, and either lets it go - to be made again on next use, where the
 * heap then puts it - or allocates the new one first and copies.  Either is
 * fine; what is not is keeping the old one.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_MEMSHAPE_H
#define ARGON_MEMSHAPE_H

#include <stdbool.h>

typedef void (*ag_reshape_fn)(void *ctx);

/* `name` is for `mem compact`'s report; the string must outlive the entry. */
bool ag_mem_reshaper_add(const char *name, ag_reshape_fn fn, void *ctx);

/* Ask every one of them now; `why` goes to the journal with the result.
 * Returns the gain in the largest free block, in bytes (may be 0). */
long ag_mem_reshape(const char *why);

#endif /* ARGON_MEMSHAPE_H */
