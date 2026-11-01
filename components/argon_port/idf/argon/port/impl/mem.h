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

#include <stdbool.h>

#define AG_MEM_FAST MALLOC_CAP_INTERNAL
#define AG_MEM_SLOW MALLOC_CAP_SPIRAM
#define AG_MEM_BYTE MALLOC_CAP_8BIT
#define AG_MEM_DMA  MALLOC_CAP_DMA
#define AG_MEM_EXEC MALLOC_CAP_EXEC
/*
 * Byte-accessible internal RAM that is NOT the DMA-capable DRAM the radio and
 * peripherals fight over.  On the ESP32 this is the D/IRAM (SRAM1) region,
 * dual-mapped and reachable byte-wise from the data bus, and it excludes the
 * word-only pure-IRAM block.  A no-PSRAM board that must hold both an
 * application and the Wi-Fi driver puts the application's writable memory here,
 * so the driver's ~36 KB contiguous allocation still finds room in DRAM.
 */
#define AG_MEM_IRAM8 MALLOC_CAP_IRAM_8BIT

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

typedef struct {
    ag_port_mem_walk_fn fn;
    void               *ctx;
} ag_port_mem_walk_ctx_t;

static inline bool ag_port_mem_walk_one(walker_heap_into_t heap,
                                        walker_block_info_t block, void *arg)
{
    (void)heap;
    const ag_port_mem_walk_ctx_t *w = (const ag_port_mem_walk_ctx_t *)arg;
    w->fn(w->ctx, block.ptr, block.size, block.used ? 1 : 0);
    return true;
}

static inline void ag_port_mem_walk(unsigned caps, ag_port_mem_walk_fn fn,
                                    void *ctx)
{
    ag_port_mem_walk_ctx_t w = {fn, ctx};
    heap_caps_walk(caps, ag_port_mem_walk_one, &w);
}

#if CONFIG_ARGON_HEAP_TWOEND
/* The two-ended heap (src/twoend_hw.c): small blocks from the top. */
typedef struct {
    uint32_t total, free, low, blocks, arenas, small_max;
    uint32_t lo[8], hi[8];
} ag_port_twoend_info_t;
bool ag_port_twoend(ag_port_twoend_info_t *out);
/* Take what has become free since start-up: the ESP32 gives the heap the
 * memory of its start-up stacks only once the scheduler runs. */
void ag_port_twoend_take_rest(void);
/* Allocations by the calling task stay for good until this is turned off:
 * place them with the long-lived (src/twoend_hw.c). */
void ag_port_mem_keep(bool on);
#endif

#if CONFIG_ARGON_SMALL_ZONE_KB > 0
/* The heap of small blocks (src/smallzone_hw.c).  `spilled`: small requests
 * the zone had no room for, served by the ordinary heap instead; `lent`: big
 * ones the ordinary heap had no room for, served by the zone. */
typedef struct {
    uint32_t total, free, low, blocks, max, spilled, lent;
} ag_port_zone_info_t;
bool ag_port_small_zone(ag_port_zone_info_t *out);
#endif

#if CONFIG_ARGON_HEAP_WHO
/* Diagnostic build only (src/heapwho_hw.c): who holds each block, and who
 * held the ones given back.  `seq` is the block's number in the order of
 * allocation, `gone` the number reached when it was given back. */
#define AG_PORT_HEAP_PCS 4
typedef struct {
    uint32_t ptr, size, seq, gone, task;
    uint32_t pc[AG_PORT_HEAP_PCS];
} ag_port_heap_rec_t;
void     ag_port_heap_who_mark(const char *text);
bool     ag_port_heap_who_get(int which, uint32_t i, ag_port_heap_rec_t *out);
uint32_t ag_port_heap_who_seq(uint32_t *lost);
#endif

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
