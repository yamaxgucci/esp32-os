/*
 * ArgonOS - code arena allocator tests.
 *
 * The interesting cases are the ones that decide whether a second application
 * can be loaded after the first one has come and gone: alignment, reuse of a
 * freed hole, and coalescing of neighbouring holes.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/arena.h>

#include "test.h"

#define REGION 1024u
#define BLOCKS 4u

static uint8_t          s_region[REGION];
static ag_arena_block_t s_blocks[BLOCKS];

static void reset(ag_arena_t *arena)
{
    ag_arena_init(arena, s_region, REGION, s_blocks, BLOCKS);
}

/* Offset of a pointer inside the region, or -1 when it is not in it. */
static long at(const void *p)
{
    if (p == NULL) {
        return -1;
    }
    return (long)((const uint8_t *)p - s_region);
}

static void test_alloc_and_free(void)
{
    ag_arena_t arena;
    reset(&arena);

    AG_CHECK_INT(ag_arena_size(&arena), REGION);
    AG_CHECK_INT(ag_arena_free_bytes(&arena), REGION);
    AG_CHECK_INT(ag_arena_blocks(&arena), 0);

    void *a = ag_arena_alloc(&arena, 100, 16);
    AG_CHECK_INT(at(a), 0);
    void *b = ag_arena_alloc(&arena, 100, 16);
    /* Aligned up from 100 to 112, not packed against its neighbour. */
    AG_CHECK_INT(at(b), 112);

    AG_CHECK_INT(ag_arena_used(&arena), 200);
    AG_CHECK_INT(ag_arena_free_bytes(&arena), REGION - 200);
    AG_CHECK_INT(ag_arena_blocks(&arena), 2);

    AG_CHECK(ag_arena_free(&arena, a));
    AG_CHECK_INT(ag_arena_blocks(&arena), 1);

    /* Freeing something twice, or something that was never ours, is reported
     * rather than corrupting the block table. */
    AG_CHECK(!ag_arena_free(&arena, a));
    AG_CHECK(!ag_arena_free(&arena, NULL));
    AG_CHECK(!ag_arena_free(&arena, s_region + 5));

    static uint8_t elsewhere[8];
    AG_CHECK(!ag_arena_free(&arena, elsewhere));

    AG_CHECK(ag_arena_free(&arena, b));
    AG_CHECK_INT(ag_arena_free_bytes(&arena), REGION);
}

static void test_first_fit_reuses_a_hole(void)
{
    ag_arena_t arena;
    reset(&arena);

    void *a = ag_arena_alloc(&arena, 128, 16);
    void *b = ag_arena_alloc(&arena, 128, 16);
    void *c = ag_arena_alloc(&arena, 128, 16);
    AG_CHECK_INT(at(a), 0);
    AG_CHECK_INT(at(b), 128);
    AG_CHECK_INT(at(c), 256);

    /* Drop the middle one and put something smaller in its place. */
    AG_CHECK(ag_arena_free(&arena, b));
    void *d = ag_arena_alloc(&arena, 64, 16);
    AG_CHECK_INT(at(d), 128);

    /* What no longer fits in the hole goes after the last block. */
    void *e = ag_arena_alloc(&arena, 100, 16);
    AG_CHECK_INT(at(e), 384);
}

static void test_neighbouring_holes_coalesce(void)
{
    ag_arena_t arena;
    reset(&arena);

    void *a = ag_arena_alloc(&arena, 256, 16);
    void *b = ag_arena_alloc(&arena, 256, 16);
    void *c = ag_arena_alloc(&arena, 256, 16);
    AG_CHECK(a != NULL && b != NULL && c != NULL);

    /* 512 bytes free in two adjacent holes; an allocation that size only fits
     * if they are one hole - which is what keeping the blocks ordered buys. */
    AG_CHECK(ag_arena_free(&arena, a));
    AG_CHECK(ag_arena_free(&arena, b));

    void *big = ag_arena_alloc(&arena, 512, 16);
    AG_CHECK_INT(at(big), 0);
    AG_CHECK_INT(ag_arena_largest_free(&arena, 16), REGION - 768);
}

static void test_limits(void)
{
    ag_arena_t arena;
    reset(&arena);

    /* Bigger than the region. */
    AG_CHECK(ag_arena_alloc(&arena, REGION + 1, 16) == NULL);

    /* Exactly the region fits, and then nothing else does. */
    void *whole = ag_arena_alloc(&arena, REGION, 16);
    AG_CHECK_INT(at(whole), 0);
    AG_CHECK(ag_arena_alloc(&arena, 1, 16) == NULL);
    AG_CHECK_INT(ag_arena_largest_free(&arena, 16), 0);
    AG_CHECK(ag_arena_free(&arena, whole));

    /* The block table is a limit of its own, and largest_free tells the truth
     * about it: free space that cannot be handed out is not free space. */
    for (uint32_t i = 0; i < BLOCKS; i++) {
        AG_CHECK(ag_arena_alloc(&arena, 16, 16) != NULL);
    }
    AG_CHECK_INT(ag_arena_blocks(&arena), BLOCKS);
    AG_CHECK(ag_arena_alloc(&arena, 16, 16) == NULL);
    AG_CHECK_INT(ag_arena_largest_free(&arena, 16), 0);

    /* Nonsense arguments are refused rather than rounded into something. */
    reset(&arena);
    AG_CHECK(ag_arena_alloc(&arena, 0, 16) == NULL);
    AG_CHECK(ag_arena_alloc(&arena, 16, 0) == NULL);
    AG_CHECK(ag_arena_alloc(&arena, 16, 24) == NULL); /* not a power of two */
    AG_CHECK(ag_arena_alloc(NULL, 16, 16) == NULL);

    ag_arena_t uninitialised;
    ag_arena_init(&uninitialised, NULL, 0, NULL, 0);
    AG_CHECK(ag_arena_alloc(&uninitialised, 16, 16) == NULL);
    AG_CHECK_INT(ag_arena_free_bytes(&uninitialised), 0);
}

static void test_alignment_is_honoured(void)
{
    ag_arena_t arena;
    reset(&arena);

    void *a = ag_arena_alloc(&arena, 1, 1);
    AG_CHECK_INT(at(a), 0);
    void *b = ag_arena_alloc(&arena, 1, 64);
    AG_CHECK_INT(at(b), 64);
    void *c = ag_arena_alloc(&arena, 1, 256);
    AG_CHECK_INT(at(c), 256);

    /* The gap before b is 63 bytes but only reachable at 1-byte alignment. */
    void *d = ag_arena_alloc(&arena, 32, 1);
    AG_CHECK_INT(at(d), 1);
}

void run_arena_tests(void)
{
    test_alloc_and_free();
    test_first_fit_reuses_a_hole();
    test_neighbouring_holes_coalesce();
    test_limits();
    test_alignment_is_honoured();
}
