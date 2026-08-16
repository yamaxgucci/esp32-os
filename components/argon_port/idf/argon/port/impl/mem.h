/*
 * ArgonOS port: ESP-IDF - memory.
 *
 * The capability bits are ESP-IDF's own values rather than a set of our own
 * translated at each call.  That is deliberate and it is the rule this whole
 * layer is built on: where the port and the system below already agree, the
 * wrapper must compile to nothing at all.  Give AG_MEM_FAST a value of its own
 * and every allocation grows a translation; give it MALLOC_CAP_INTERNAL and the
 * generated code is the code that was there before the layer existed.
 *
 * A port whose allocator does not speak in a capability mask is free to define
 * these as 1, 2, 4 and translate - it pays for the translation because it has
 * to, not because the interface asked for it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_MEM_H
#define ARGON_PORT_IMPL_MEM_H

#include "esp_heap_caps.h"
#include "multi_heap.h"

#define AG_MEM_FAST MALLOC_CAP_INTERNAL
#define AG_MEM_SLOW MALLOC_CAP_SPIRAM
#define AG_MEM_BYTE MALLOC_CAP_8BIT
#define AG_MEM_DMA  MALLOC_CAP_DMA
#define AG_MEM_EXEC MALLOC_CAP_EXEC

typedef multi_heap_handle_t ag_port_heap_t;

static inline void *ag_port_alloc(size_t bytes, unsigned caps)
{
    return heap_caps_malloc(bytes, caps);
}

static inline void *ag_port_calloc(size_t count, size_t size, unsigned caps)
{
    return heap_caps_calloc(count, size, caps);
}

static inline void *ag_port_realloc(void *p, size_t bytes, unsigned caps)
{
    return heap_caps_realloc(p, bytes, caps);
}

static inline void *ag_port_alloc_aligned(size_t align, size_t bytes,
                                          unsigned caps)
{
    return heap_caps_aligned_alloc(align, bytes, caps);
}

static inline void ag_port_free(void *p) { heap_caps_free(p); }

static inline size_t ag_port_mem_free(unsigned caps)
{
    return heap_caps_get_free_size(caps);
}

static inline size_t ag_port_mem_total(unsigned caps)
{
    return heap_caps_get_total_size(caps);
}

static inline size_t ag_port_mem_largest(unsigned caps)
{
    return heap_caps_get_largest_free_block(caps);
}

static inline size_t ag_port_alloc_size(const void *p)
{
    return heap_caps_get_allocated_size((void *)p);
}

/* ---------------------------------------------------------------------- */
/* A heap inside a block of memory - the process arena                     */
/* ---------------------------------------------------------------------- */

static inline ag_port_heap_t ag_port_heap_register(void *mem, size_t bytes)
{
    return multi_heap_register(mem, bytes);
}

static inline void *ag_port_heap_alloc(ag_port_heap_t h, size_t bytes)
{
    return multi_heap_malloc(h, bytes);
}

static inline void *ag_port_heap_realloc(ag_port_heap_t h, void *p, size_t bytes)
{
    return multi_heap_realloc(h, p, bytes);
}

static inline void ag_port_heap_free(ag_port_heap_t h, void *p)
{
    multi_heap_free(h, p);
}

static inline size_t ag_port_heap_alloc_size(ag_port_heap_t h, const void *p)
{
    return multi_heap_get_allocated_size(h, (void *)p);
}

static inline void ag_port_heap_info(ag_port_heap_t h, ag_port_heap_info_t *out)
{
    multi_heap_info_t info;
    multi_heap_get_info(h, &info);
    out->allocated = info.total_allocated_bytes;
    out->free_bytes = info.total_free_bytes;
    out->largest_free = info.largest_free_block;
}

#endif /* ARGON_PORT_IMPL_MEM_H */
