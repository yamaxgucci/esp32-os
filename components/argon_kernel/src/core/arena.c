/*
 * ArgonOS - a small allocator for a fixed region.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/arena.h>

#include <string.h>

void ag_arena_init(ag_arena_t *arena, void *base, size_t size,
                   ag_arena_block_t *blocks, uint32_t max_blocks)
{
    if (arena == NULL) {
        return;
    }
    memset(arena, 0, sizeof(*arena));
    if (base == NULL || blocks == NULL || max_blocks == 0) {
        return;
    }
    arena->base = (uint8_t *)base;
    arena->size = size;
    arena->blocks = blocks;
    arena->max_blocks = max_blocks;
    memset(blocks, 0, max_blocks * sizeof(*blocks));
}

static uint32_t align_up(uint32_t value, size_t align)
{
    const uint32_t mask = (uint32_t)align - 1u;
    return (value + mask) & ~mask;
}

/* Where the gap before block `index` starts; `index` == count means the tail. */
static uint32_t gap_start(const ag_arena_t *arena, uint32_t index)
{
    if (index == 0) {
        return 0;
    }
    const ag_arena_block_t *prev = &arena->blocks[index - 1];
    return prev->offset + prev->size;
}

static uint32_t gap_end(const ag_arena_t *arena, uint32_t index)
{
    if (index >= arena->count) {
        return (uint32_t)arena->size;
    }
    return arena->blocks[index].offset;
}

void *ag_arena_alloc(ag_arena_t *arena, size_t bytes, size_t align)
{
    if (arena == NULL || arena->base == NULL || bytes == 0) {
        return NULL;
    }
    if (align == 0 || (align & (align - 1)) != 0) {
        return NULL; /* not a power of two */
    }
    if (arena->count >= arena->max_blocks) {
        return NULL;
    }

    for (uint32_t i = 0; i <= arena->count; i++) {
        const uint32_t start = align_up(gap_start(arena, i), align);
        const uint32_t end = gap_end(arena, i);

        if (start > end || (uint32_t)(end - start) < bytes) {
            continue;
        }

        /* Insert in offset order, so gaps stay implicit and always coalesced. */
        for (uint32_t j = arena->count; j > i; j--) {
            arena->blocks[j] = arena->blocks[j - 1];
        }
        arena->blocks[i].offset = start;
        arena->blocks[i].size = (uint32_t)bytes;
        arena->count++;
        return arena->base + start;
    }

    return NULL;
}

bool ag_arena_free(ag_arena_t *arena, void *ptr)
{
    if (arena == NULL || arena->base == NULL || ptr == NULL) {
        return false;
    }
    if ((uint8_t *)ptr < arena->base ||
        (uint8_t *)ptr >= arena->base + arena->size) {
        return false;
    }

    const uint32_t offset = (uint32_t)((uint8_t *)ptr - arena->base);
    for (uint32_t i = 0; i < arena->count; i++) {
        if (arena->blocks[i].offset != offset) {
            continue;
        }
        for (uint32_t j = i; j + 1 < arena->count; j++) {
            arena->blocks[j] = arena->blocks[j + 1];
        }
        arena->count--;
        memset(&arena->blocks[arena->count], 0, sizeof(arena->blocks[0]));
        return true;
    }

    return false;
}

size_t ag_arena_size(const ag_arena_t *arena)
{
    return (arena != NULL) ? arena->size : 0;
}

size_t ag_arena_used(const ag_arena_t *arena)
{
    if (arena == NULL) {
        return 0;
    }
    size_t used = 0;
    for (uint32_t i = 0; i < arena->count; i++) {
        used += arena->blocks[i].size;
    }
    return used;
}

size_t ag_arena_free_bytes(const ag_arena_t *arena)
{
    return (arena != NULL) ? arena->size - ag_arena_used(arena) : 0;
}

size_t ag_arena_largest_free(const ag_arena_t *arena, size_t align)
{
    if (arena == NULL || arena->base == NULL) {
        return 0;
    }
    if (align == 0 || (align & (align - 1)) != 0) {
        return 0;
    }
    if (arena->count >= arena->max_blocks) {
        return 0; /* nothing can be allocated, whatever the free space says */
    }

    size_t largest = 0;
    for (uint32_t i = 0; i <= arena->count; i++) {
        const uint32_t start = align_up(gap_start(arena, i), align);
        const uint32_t end = gap_end(arena, i);
        if (start < end && (size_t)(end - start) > largest) {
            largest = (size_t)(end - start);
        }
    }
    return largest;
}

uint32_t ag_arena_blocks(const ag_arena_t *arena)
{
    return (arena != NULL) ? arena->count : 0;
}
