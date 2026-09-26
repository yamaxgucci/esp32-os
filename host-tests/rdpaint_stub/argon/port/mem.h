/*
 * The port's allocator, for the host test of net/rdpaint.c only: malloc, and
 * every kind of memory is the same kind.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RDPAINT_STUB_MEM_H
#define RDPAINT_STUB_MEM_H

#include <stddef.h>
#include <stdlib.h>

#define AG_MEM_FAST 1u
#define AG_MEM_SLOW 2u
#define AG_MEM_BYTE 4u
#define AG_MEM_DMA  8u
#define AG_MEM_EXEC 16u

static inline void *ag_port_alloc(size_t bytes, unsigned caps)
{
    (void)caps;
    return malloc(bytes);
}

static inline void ag_port_free(void *p) { free(p); }

#endif /* RDPAINT_STUB_MEM_H */
