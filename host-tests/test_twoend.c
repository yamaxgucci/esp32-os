/*
 * ArgonOS - the two-ended heap (components/argon_port/idf/src/twoend_core.h).
 *
 * Two kinds of check.  Where blocks go: small ones at the top of the fullest
 * arena with room, large ones at the bottom - the whole point of it.  And
 * that nothing is ever lost or overlapped: random allocations, frees, resizes
 * and aligned requests against a record of what should be where, with the
 * heap's every structure walked after each step.  The allocator runs in every
 * malloc of the board, so the second is not optional.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <stdlib.h>

#define TE_IRAM
#define TE_LOCK()
#define TE_UNLOCK()
#include "../components/argon_port/idf/src/twoend_core.h"

#include "test.h"

/* Every structure of the heap agrees with every other; 0 if so. */
static int te_check(const te_heap_t *h)
{
    size_t free_sum = 0;
    uint32_t used = 0;
    const te_free_t *expect = h->head, *prevf = NULL;
    for (int i = 0; i < h->arenas; i++) {
        uintptr_t b = h->arena[i].lo;
        bool prev_used = true, prev_free = false;
        size_t arena_free = 0;
        for (;;) {
            const uint32_t hdr = *TE_AT(b);
            const uint32_t size = TE_SIZE(hdr);
            if (((hdr & TE_PREV_USED) != 0) != prev_used) return 1;
            if (b == h->arena[i].hi - TE_HDR) {
                if (size != 0 || !(hdr & TE_USED)) return 2;
                break;
            }
            if (b > h->arena[i].hi - TE_HDR || size < TE_MIN || (size & 3u)) return 3;
            if (hdr & TE_USED) {
                used++;
                prev_used = true;
                prev_free = false;
            } else {
                if (prev_free) return 4;
                if (*TE_AT(b + size - 4u) != size) return 5;
                if ((const te_free_t *)b != expect || expect->prev != prevf) return 6;
                prevf = expect;
                expect = expect->next;
                free_sum += size;
                arena_free += size;
                prev_used = false;
                prev_free = true;
            }
            b += size;
        }
        if (arena_free != h->arena_free[i]) return 7;
    }
    if (expect != NULL || h->tail != prevf) return 8;
    if (free_sum != h->free_bytes || used != h->used_blocks) return 9;
    return 0;
}

static void test_directions(void)
{
    static uint32_t a[1024], b[2048];
    te_heap_t h;
    memset(&h, 0, sizeof(h));
    h.small_max = 128;
    AG_CHECK(te_add_arena(&h, b, sizeof(b)));
    AG_CHECK(te_add_arena(&h, a, sizeof(a)));
    AG_CHECK_INT(h.arenas, 2);

    /* both arenas empty: the fuller is the smaller, a's top */
    uint8_t *s1 = te_alloc(&h, 24);
    AG_CHECK(s1 > (uint8_t *)a && s1 < (uint8_t *)a + sizeof(a));
    AG_CHECK(s1 > (uint8_t *)a + sizeof(a) - 64);
    uint8_t *s2 = te_alloc(&h, 24);
    AG_CHECK(s2 < s1);                              /* downward */

    /* large: the lowest address with room, from its bottom */
    uint8_t *l1 = te_alloc(&h, 1000);
    const uint8_t *low = (uintptr_t)a < (uintptr_t)b ? (uint8_t *)a : (uint8_t *)b;
    AG_CHECK(l1 == low + TE_HDR);
    uint8_t *l2 = te_alloc(&h, 1000);
    AG_CHECK(l2 > l1);                              /* upward */
    AG_CHECK_INT(te_check(&h), 0);

    /* freeing everything gives two whole arenas back */
    te_free(&h, s1);
    te_free(&h, l1);
    te_free(&h, s2);
    te_free(&h, l2);
    AG_CHECK_INT(te_check(&h), 0);
    AG_CHECK(h.free_bytes == h.total);
    AG_CHECK(h.head != NULL && h.head->next == h.tail && h.tail->next == NULL);

    /* aligned, and the piece before it stays free */
    void *al = te_alloc_aligned(&h, 256, 100);
    AG_CHECK(al != NULL && ((uintptr_t)al & 255u) == 0);
    AG_CHECK_INT(te_check(&h), 0);
    te_free(&h, al);
    AG_CHECK(h.free_bytes == h.total);
}

#define LIVE 600
static struct { uint8_t *p; size_t n; uint8_t tag; } s_live[LIVE];

static size_t rsize(void)
{
    const int r = rand() % 100;
    if (r < 60) return 1 + (size_t)(rand() % 128);
    if (r < 90) return 129 + (size_t)(rand() % 2000);
    return 2000 + (size_t)(rand() % 12000);
}

static bool intact(int i)
{
    for (size_t k = 0; k < s_live[i].n; k++) {
        if (s_live[i].p[k] != (uint8_t)(s_live[i].tag + k)) return false;
    }
    return true;
}

static void fill(int i)
{
    for (size_t k = 0; k < s_live[i].n; k++) s_live[i].p[k] = (uint8_t)(s_live[i].tag + k);
}

static void test_random(void)
{
    static uint8_t m1[48 * 1024], m2[30 * 1024], m3[5 * 1024];
    te_heap_t h;
    memset(&h, 0, sizeof(h));
    memset(s_live, 0, sizeof(s_live));
    h.small_max = 128;
    te_add_arena(&h, m2 + 1, sizeof(m2) - 1);       /* not word aligned */
    te_add_arena(&h, m1, sizeof(m1));
    te_add_arena(&h, m3, sizeof(m3));
    srand(20261029);
    int bad_check = 0, bad_data = 0, bad_align = 0;
    for (long op = 0; op < 200000; op++) {
        const int i = rand() % LIVE;
        const int what = rand() % 10;
        if (s_live[i].p == NULL) {
            const size_t n = rsize();
            void *p;
            if (what == 0) {
                const size_t al = (size_t)8 << (rand() % 6);
                p = te_alloc_aligned(&h, al, n);
                if (p != NULL && ((uintptr_t)p & (al - 1u))) bad_align++;
            } else {
                p = te_alloc(&h, n);
            }
            if (p != NULL) {
                if (((uintptr_t)p & 3u) || te_usable(p) < n) bad_align++;
                s_live[i].p = p;
                s_live[i].n = n;
                s_live[i].tag = (uint8_t)rand();
                fill(i);
            }
        } else if (what < 3) {
            if (!intact(i)) bad_data++;
            const size_t n = rsize();
            if (te_resize(&h, s_live[i].p, n) != NULL) {
                const size_t keep = n < s_live[i].n ? n : s_live[i].n;
                s_live[i].n = keep;
                if (!intact(i)) bad_data++;
                s_live[i].n = n;
                fill(i);
            }
        } else {
            if (!intact(i)) bad_data++;
            te_free(&h, s_live[i].p);
            s_live[i].p = NULL;
        }
        if ((op % 53) == 0 && te_check(&h) != 0) bad_check++;
    }
    for (int i = 0; i < LIVE; i++) {
        if (s_live[i].p != NULL) {
            if (!intact(i)) bad_data++;
            te_free(&h, s_live[i].p);
        }
    }
    AG_CHECK_INT(bad_check, 0);
    AG_CHECK_INT(bad_data, 0);
    AG_CHECK_INT(bad_align, 0);
    AG_CHECK_INT(te_check(&h), 0);
    AG_CHECK(h.free_bytes == h.total);
}

void run_twoend_tests(void)
{
    test_directions();
    test_random();
}
