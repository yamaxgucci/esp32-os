/*
 * ArgonOS port: a two-ended heap - the allocator itself, no ESP-IDF in it.
 *
 * Small blocks are taken from the top of memory downward, large ones from the
 * bottom upward, and the free space between them is one piece that both sides
 * eat into.  What fragments a heap is small long-lived blocks landing among
 * large short-lived ones (tools/heapwho.py on the CYD, 29 Oct 2026: FreeRTOS's
 * 84-byte mutexes walling in the holes the radio's buffers left).  A fixed
 * area for the small ones fixed that and cost what was idle inside it; here
 * there is no line between the two, only two directions.
 *
 * Several arenas (separate stretches of RAM) are one address-ordered space: the
 * lowest arena's bottom is "the bottom", the highest arena's top "the top".
 * Except that a small block goes to the fullest arena that has room for it,
 * at its top: small blocks gather beside what is already held, and the
 * emptiest arena is left whole for the large.  On the CYD the highest arena
 * was also the emptiest, and small blocks gathered at its top took 7 KB off
 * the one piece an application's data has to fit in.
 *
 * Blocks carry a 4-byte header: size (with the header, a multiple of 4) and
 * two flags.  A free block also holds the list links after its header and its
 * size in its last word, so the block above it can find its start.  The free
 * blocks of every arena are one doubly linked list in address order; small
 * requests walk it from the tail, large ones from the head.  Each arena ends
 * in a 4-byte used header of size 0, which no merge crosses.
 *
 * Included by twoend_hw.c (the firmware) and by the host test; the includer
 * defines TE_IRAM, TE_LOCK() and TE_UNLOCK().
 */
#ifndef TWOEND_CORE_H
#define TWOEND_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TE_USED       1u
#define TE_PREV_USED  2u
#define TE_FLAGS      3u
#define TE_HDR        4u
#define TE_MAX_ARENAS 8

typedef struct te_free {
    uint32_t        hdr;
    struct te_free *next, *prev;
} te_free_t;

/* header, two links, footer: 16 bytes on the ESP32 */
#define TE_MIN ((uint32_t)((sizeof(te_free_t) + 4u + 3u) & ~(size_t)3u))

typedef struct {
    uintptr_t lo, hi;        /* [lo, hi): the last 4 bytes are the end mark */
} te_arena_t;

typedef struct {
    te_arena_t arena[TE_MAX_ARENAS];
    int        arenas;
    te_free_t *head, *tail;  /* free blocks, lowest address first */
    size_t     small_max;    /* this or less: from the top */
    size_t     arena_free[TE_MAX_ARENAS];
    size_t     free_bytes, min_free, total;
    uint32_t   used_blocks;
} te_heap_t;

#define TE_SIZE(h)  ((h) & ~TE_FLAGS)
#define TE_AT(a)    ((uint32_t *)(uintptr_t)(a))

static inline TE_IRAM void te_footer(uintptr_t b, uint32_t size)
{
    *TE_AT(b + size - 4u) = size;
}

static inline TE_IRAM void te_set_prev_used(uintptr_t next, bool used)
{
    uint32_t *n = TE_AT(next);
    *n = used ? (*n | TE_PREV_USED) : (*n & ~TE_PREV_USED);
}

static TE_IRAM void te_unlink(te_heap_t *h, te_free_t *f)
{
    if (f->prev) f->prev->next = f->next; else h->head = f->next;
    if (f->next) f->next->prev = f->prev; else h->tail = f->prev;
}

/* Put f into the list after `after` (NULL: at the head). */
static TE_IRAM void te_link_after(te_heap_t *h, te_free_t *after, te_free_t *f)
{
    f->prev = after;
    f->next = after ? after->next : h->head;
    if (f->next) f->next->prev = f; else h->tail = f;
    if (after) after->next = f; else h->head = f;
}

/* The free block that should come before address a, or NULL. */
static TE_IRAM te_free_t *te_before(te_heap_t *h, uintptr_t a)
{
    /* walk from whichever end is nearer in address */
    if (h->tail && (uintptr_t)h->tail < a) return h->tail;
    if (!h->head || (uintptr_t)h->head > a) return NULL;
    const uintptr_t mid = ((uintptr_t)h->head + (uintptr_t)h->tail) / 2u;
    if (a < mid) {
        te_free_t *f = h->head;
        while (f->next && (uintptr_t)f->next < a) f = f->next;
        return f;
    }
    te_free_t *f = h->tail;
    while (f && (uintptr_t)f > a) f = f->prev;
    return f;
}

/* A free block of `size` at b, its neighbours' flags kept true. */
static TE_IRAM void te_make_free(te_heap_t *h, uintptr_t b, uint32_t size,
                                 bool prev_used, te_free_t *after)
{
    te_free_t *f = (te_free_t *)b;
    f->hdr = size | (prev_used ? TE_PREV_USED : 0u);
    te_footer(b, size);
    te_set_prev_used(b + size, false);
    te_link_after(h, after, f);
}

/* Add [lo, lo+bytes) as an arena.  Arenas are kept in address order. */
static bool te_add_arena(te_heap_t *h, void *mem, size_t bytes)
{
    uintptr_t lo = ((uintptr_t)mem + 3u) & ~(uintptr_t)3u;
    uintptr_t hi = ((uintptr_t)mem + bytes) & ~(uintptr_t)3u;
    if (h->arenas == TE_MAX_ARENAS || hi < lo + TE_MIN + TE_HDR) return false;
    int i = h->arenas;
    while (i > 0 && h->arena[i - 1].lo > lo) {
        h->arena[i] = h->arena[i - 1];
        h->arena_free[i] = h->arena_free[i - 1];
        i--;
    }
    h->arena[i].lo = lo;
    h->arena[i].hi = hi;
    h->arenas++;
    const uint32_t size = (uint32_t)(hi - TE_HDR - lo);
    *TE_AT(hi - TE_HDR) = 0u | TE_USED;            /* end mark, prev free */
    te_make_free(h, lo, size, true, te_before(h, lo));
    h->arena_free[i] = size;
    h->free_bytes += size;
    h->total += size;
    h->min_free = h->free_bytes;
    return true;
}

static inline TE_IRAM bool te_owns(const te_heap_t *h, const void *p)
{
    const uintptr_t a = (uintptr_t)p;
    for (int i = 0; i < h->arenas; i++) {
        if (a >= h->arena[i].lo && a < h->arena[i].hi) return true;
    }
    return false;
}

static inline TE_IRAM uint32_t te_need(size_t size)
{
    uint32_t n = (uint32_t)((size + TE_HDR + 3u) & ~(size_t)3u);
    return n < TE_MIN ? TE_MIN : n;
}

static inline TE_IRAM int te_arena_of(const te_heap_t *h, uintptr_t a)
{
    for (int i = 0; i < h->arenas; i++) {
        if (a >= h->arena[i].lo && a < h->arena[i].hi) return i;
    }
    return 0;
}

/* Hand out `need` bytes of free block f at its low end (large) or high end
 * (small); the rest stays free where it was. */
static TE_IRAM void *te_carve(te_heap_t *h, te_free_t *f, uint32_t need,
                              bool from_top)
{
    const uintptr_t b = (uintptr_t)f;
    const uint32_t  size = TE_SIZE(f->hdr);
    const bool      prev_used = (f->hdr & TE_PREV_USED) != 0;
    uintptr_t       u;
    if (size - need < TE_MIN) {
        need = size;                                   /* take it whole */
        te_unlink(h, f);
        u = b;
        *TE_AT(u) = need | TE_USED | (prev_used ? TE_PREV_USED : 0u);
    } else if (from_top) {
        const uint32_t rest = size - need;
        f->hdr = rest | (prev_used ? TE_PREV_USED : 0u);
        te_footer(b, rest);
        u = b + rest;
        *TE_AT(u) = need | TE_USED;                    /* prev: free */
    } else {
        te_free_t *before = f->prev;
        te_unlink(h, f);
        u = b;
        *TE_AT(u) = need | TE_USED | (prev_used ? TE_PREV_USED : 0u);
        te_free_t *r = (te_free_t *)(b + need);
        r->hdr = (size - need) | TE_PREV_USED;
        te_footer((uintptr_t)r, size - need);
        te_link_after(h, before, r);
    }
    te_set_prev_used(u + need, true);
    h->arena_free[te_arena_of(h, u)] -= need;
    h->free_bytes -= need;
    if (h->free_bytes < h->min_free) h->min_free = h->free_bytes;
    h->used_blocks++;
    return (void *)(u + TE_HDR);
}

/* `keep`: placed like a small block whatever its size - it will stay. */
static TE_IRAM void *te_alloc_as(te_heap_t *h, size_t size, bool keep)
{
    if (size == 0 || size > 0x7FFFFFF0u) return NULL;
    const uint32_t need = te_need(size);
    void *p = NULL;
    TE_LOCK();
    if (keep || size <= h->small_max) {
        /* the fullest arena first, from its top */
        uint32_t tried = 0;
        while (p == NULL) {
            int best = -1;
            for (int i = 0; i < h->arenas; i++) {
                if ((tried & (1u << i)) || h->arena_free[i] < need) continue;
                if (best < 0 || h->arena_free[i] < h->arena_free[best]) best = i;
            }
            if (best < 0) break;
            tried |= 1u << best;
            const uintptr_t lo = h->arena[best].lo, hi = h->arena[best].hi;
            for (te_free_t *f = h->tail; f; f = f->prev) {
                const uintptr_t a = (uintptr_t)f;
                if (a >= hi) continue;
                if (a < lo) break;
                if (TE_SIZE(f->hdr) >= need) { p = te_carve(h, f, need, true); break; }
            }
        }
    } else {
        for (te_free_t *f = h->head; f; f = f->next) {
            if (TE_SIZE(f->hdr) >= need) { p = te_carve(h, f, need, false); break; }
        }
    }
    TE_UNLOCK();
    return p;
}

static inline TE_IRAM void *te_alloc(te_heap_t *h, size_t size)
{
    return te_alloc_as(h, size, false);
}

/* Aligned: from the bottom, the first free block with room for an aligned
 * start; the piece below it, if any, stays free. */
static TE_IRAM void *te_alloc_aligned(te_heap_t *h, size_t align, size_t size)
{
    if (align <= 4u) return te_alloc(h, size);
    if ((align & (align - 1u)) != 0 || size == 0 || size > 0x7FFFFFF0u) return NULL;
    const uint32_t need = te_need(size);
    void *p = NULL;
    TE_LOCK();
    for (te_free_t *f = h->head; f; f = f->next) {
        const uintptr_t b = (uintptr_t)f;
        const uint32_t  size_f = TE_SIZE(f->hdr);
        uintptr_t user = (b + TE_HDR + align - 1u) & ~(uintptr_t)(align - 1u);
        while (user - TE_HDR != b && user - TE_HDR - b < TE_MIN) user += align;
        const uintptr_t u = user - TE_HDR;
        if (u + need > b + size_f) continue;
        if (u == b) { p = te_carve(h, f, need, false); break; }
        /* the lead stays free in f's place; the rest is a block of its own */
        const uint32_t lead = (uint32_t)(u - b);
        const bool prev_used = (f->hdr & TE_PREV_USED) != 0;
        f->hdr = lead | (prev_used ? TE_PREV_USED : 0u);
        te_footer(b, lead);
        te_free_t *g = (te_free_t *)u;
        g->hdr = size_f - lead;                         /* prev: free */
        te_footer(u, size_f - lead);
        te_link_after(h, f, g);
        p = te_carve(h, g, need, false);
        break;
    }
    TE_UNLOCK();
    return p;
}

static TE_IRAM void te_free_locked(te_heap_t *h, void *p)
{
    uintptr_t b = (uintptr_t)p - TE_HDR;
    uint32_t  hdr = *TE_AT(b);
    uint32_t  size = TE_SIZE(hdr);
    bool      prev_used = (hdr & TE_PREV_USED) != 0;
    h->arena_free[te_arena_of(h, b)] += size;
    h->free_bytes += size;
    h->used_blocks--;

    te_free_t *after;
    const uintptr_t nb = b + size;
    const uint32_t  nh = *TE_AT(nb);
    if (!(nh & TE_USED)) {                         /* merge the one above */
        te_free_t *n = (te_free_t *)nb;
        after = n->prev;
        te_unlink(h, n);
        size += TE_SIZE(nh);
    } else {
        after = NULL;
    }
    if (!prev_used) {                              /* merge into the one below */
        const uint32_t psize = *TE_AT(b - 4u);
        te_free_t *pf = (te_free_t *)(b - psize);
        pf->hdr = (psize + size) | (pf->hdr & TE_PREV_USED);
        te_footer((uintptr_t)pf, psize + size);
        te_set_prev_used((uintptr_t)pf + psize + size, false);
        return;
    }
    if (after == NULL && !(nh & TE_USED)) {
        /* the one above was the list's head */
        te_make_free(h, b, size, true, NULL);
        return;
    }
    te_make_free(h, b, size, true, (nh & TE_USED) ? te_before(h, b) : after);
}

static TE_IRAM void te_free(te_heap_t *h, void *p)
{
    TE_LOCK();
    te_free_locked(h, p);
    TE_UNLOCK();
}

static inline TE_IRAM size_t te_usable(const void *p)
{
    return TE_SIZE(*TE_AT((uintptr_t)p - TE_HDR)) - TE_HDR;
}

/* In place when it shrinks or the block above is free and big enough; NULL
 * otherwise, and the caller moves it. */
static TE_IRAM void *te_resize(te_heap_t *h, void *p, size_t size)
{
    if (size == 0 || size > 0x7FFFFFF0u) return NULL;
    const uint32_t need = te_need(size);
    const uintptr_t b = (uintptr_t)p - TE_HDR;
    void *r = NULL;
    TE_LOCK();
    uint32_t hdr = *TE_AT(b);
    uint32_t have = TE_SIZE(hdr);
    const uintptr_t nb = b + have;
    const uint32_t nh = *TE_AT(nb);
    if (need > have && !(nh & TE_USED) && have + TE_SIZE(nh) >= need) {
        te_free_t *n = (te_free_t *)nb;            /* swallow the one above */
        te_unlink(h, n);
        h->arena_free[te_arena_of(h, b)] -= TE_SIZE(nh);
        h->free_bytes -= TE_SIZE(nh);
        have += TE_SIZE(nh);
        *TE_AT(b) = have | (hdr & TE_FLAGS);
        te_set_prev_used(b + have, true);
        hdr = *TE_AT(b);
    }
    if (need <= have) {
        if (have - need >= TE_MIN) {               /* give the tail back */
            *TE_AT(b) = need | (hdr & TE_FLAGS);
            uintptr_t t = b + need;
            *TE_AT(t) = (have - need) | TE_USED | TE_PREV_USED;
            te_set_prev_used(t + (have - need), true);
            h->used_blocks++;
            te_free_locked(h, (void *)(t + TE_HDR));
        }
        r = p;
    }
    TE_UNLOCK();
    return r;
}

typedef void (*te_walk_fn)(void *ctx, const void *addr, size_t bytes, int used);

static void te_walk(const te_heap_t *h, te_walk_fn fn, void *ctx)
{
    for (int i = 0; i < h->arenas; i++) {
        uintptr_t b = h->arena[i].lo;
        while (b < h->arena[i].hi - TE_HDR) {
            const uint32_t hdr = *TE_AT(b);
            const uint32_t size = TE_SIZE(hdr);
            if (size == 0) break;
            fn(ctx, (const void *)(b + TE_HDR), size - TE_HDR, (hdr & TE_USED) ? 1 : 0);
            b += size;
        }
    }
}

static size_t te_largest(te_heap_t *h)
{
    size_t best = 0;
    TE_LOCK();
    for (te_free_t *f = h->head; f; f = f->next) {
        if (TE_SIZE(f->hdr) > best) best = TE_SIZE(f->hdr);
    }
    TE_UNLOCK();
    return best > TE_HDR ? best - TE_HDR : 0;
}

#endif /* TWOEND_CORE_H */
