/*
 * ArgonOS - a small allocator for a fixed region, with the bookkeeping kept
 * outside it.
 *
 * Used for the application code arena.  Two things make a general heap the wrong
 * tool there:
 *
 *   - the region is executable memory reached through the instruction window,
 *     and putting an allocator's metadata in it would mean reading and writing
 *     that window byte-wise on every allocation - exactly the access pattern
 *     that is a known trap on this family (see docs/05-status.md);
 *   - there are at most a handful of blocks in it, one per loaded application,
 *     so first fit over an ordered array is both faster and easier to reason
 *     about than a size-class allocator.
 *
 * Blocks stay sorted by offset, so a free block is simply a gap between two used
 * ones and coalescing is not a thing that can be forgotten.
 *
 * No dependency on FreeRTOS or the chip: built and tested on the host.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_ARENA_H
#define ARGON_ARENA_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t offset;
    uint32_t size;
} ag_arena_block_t;

typedef struct {
    uint8_t          *base;
    size_t            size;
    ag_arena_block_t *blocks;
    uint32_t          max_blocks;
    uint32_t          count; /* used blocks, kept sorted by offset          */
} ag_arena_t;

/*
 * `blocks` is storage for at most `max_blocks` live allocations and must live
 * somewhere ordinary - not inside the region being managed.
 */
void ag_arena_init(ag_arena_t *arena, void *base, size_t size,
                   ag_arena_block_t *blocks, uint32_t max_blocks);

/*
 * First fit, aligned up to `align` (a power of two, at least 1).  NULL when
 * nothing fits or the block table is full - a caller that gets NULL should say
 * how much it wanted and how much was free, because those two numbers are the
 * whole diagnosis.
 */
void *ag_arena_alloc(ag_arena_t *arena, size_t bytes, size_t align);

/* False when the pointer was not one of ours, which is worth reporting. */
bool ag_arena_free(ag_arena_t *arena, void *ptr);

size_t   ag_arena_size(const ag_arena_t *arena);
size_t   ag_arena_used(const ag_arena_t *arena);
size_t   ag_arena_free_bytes(const ag_arena_t *arena);
/* The largest single allocation that would succeed right now. */
size_t   ag_arena_largest_free(const ag_arena_t *arena, size_t align);
uint32_t ag_arena_blocks(const ag_arena_t *arena);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_ARENA_H */
