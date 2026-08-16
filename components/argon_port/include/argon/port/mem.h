/*
 * ArgonOS port contract - memory.
 *
 * This system has more than one kind of RAM and the difference is not an
 * optimisation: internal SRAM is the only memory that can be executed from and
 * the only memory an interrupt may touch, while PSRAM is where megabytes live.
 * So every allocation says which it wants, and a port that has only one kind of
 * memory answers every request from it.
 *
 * The capability bits are a mask and they combine: AG_MEM_SLOW | AG_MEM_BYTE
 * means "PSRAM, byte-addressable".  A port defines their values itself, so that
 * on a system whose allocator already speaks in such a mask the wrapper is the
 * identity function and costs nothing.
 *
 * What a port must supply:
 *
 *   AG_MEM_FAST   memory the CPU reaches without a cache miss (internal SRAM)
 *   AG_MEM_SLOW   large, external, unusable from an ISR (PSRAM)
 *   AG_MEM_BYTE   individually addressable bytes, not word-only
 *   AG_MEM_DMA    reachable by the DMA engines
 *   AG_MEM_EXEC   instructions may be fetched from it
 *
 *   void  *ag_port_alloc(size_t bytes, unsigned caps)
 *   void  *ag_port_calloc(size_t count, size_t size, unsigned caps)
 *   void  *ag_port_realloc(void *p, size_t bytes, unsigned caps)
 *   void  *ag_port_alloc_aligned(size_t align, size_t bytes, unsigned caps)
 *   void   ag_port_free(void *p)
 *   size_t ag_port_mem_free(unsigned caps)
 *   size_t ag_port_mem_total(unsigned caps)
 *   size_t ag_port_mem_largest(unsigned caps)
 *
 * All allocators return NULL on failure and never abort.  ag_port_free(NULL) is
 * legal and does nothing.  Memory from ag_port_alloc_aligned is released by
 * ag_port_free like any other.
 *
 * The three size queries are what the `mem` command prints, so they are allowed
 * to be approximate on a port that cannot answer exactly - but they must not
 * lie about zero: a port with no such memory reports 0 rather than the total of
 * some other pool.
 *
 *   size_t ag_port_alloc_size(const void *p)
 *
 * says how large a block actually is; 0 if the port cannot tell.
 *
 * A heap inside a block of memory
 * -------------------------------
 *
 * A process allocates from its own arena, not from the system heap, and the
 * reason is worth keeping in view: when the process ends, the arena goes back
 * in one operation and the kernel heap is not left fragmented by an application
 * that has already stopped running.  That needs an allocator that can be handed
 * a block of memory and manage it - not a second global heap.
 *
 *   ag_port_heap_t ag_port_heap_register(void *mem, size_t bytes)
 *   void  *ag_port_heap_alloc(ag_port_heap_t h, size_t bytes)
 *   void  *ag_port_heap_realloc(ag_port_heap_t h, void *p, size_t bytes)
 *   void   ag_port_heap_free(ag_port_heap_t h, void *p)
 *   size_t ag_port_heap_alloc_size(ag_port_heap_t h, const void *p)
 *   void   ag_port_heap_info(ag_port_heap_t h, ag_port_heap_info_t *out)
 *
 * where ag_port_heap_info_t carries three numbers the shell prints:
 *
 *   size_t allocated;      bytes handed out
 *   size_t free_bytes;     bytes still available
 *   size_t largest_free;   the largest single allocation that would succeed
 *
 * There is no ag_port_heap_destroy: the arena is released by freeing the block
 * it was registered over, and the port must not leave anything behind that
 * outlives it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_MEM_H
#define ARGON_PORT_MEM_H

#include <stddef.h>

typedef struct {
    size_t allocated;
    size_t free_bytes;
    size_t largest_free;
} ag_port_heap_info_t;

#include <argon/port/impl/mem.h>

#endif /* ARGON_PORT_MEM_H */
