/*
 * ArgonOS port: ESP-IDF - the two-ended heap in place of TLSF
 * (CONFIG_ARGON_HEAP_TWOEND).  The allocator is twoend_core.h; this file
 * gives it the machine's memory and puts it under malloc.
 *
 * At start-up, before anything else allocates, every free piece of
 * byte-addressable internal RAM is taken from ESP-IDF's heap in one block
 * each and handed to the two-ended heap.  From then on every request that
 * memory can serve - malloc, calloc, realloc, heap_caps_* for internal, DMA,
 * 8-bit or 32-bit memory, aligned or not - is served there: small ones from
 * the top, large ones from the bottom.  ESP-IDF's heap keeps what it had
 * already handed out and the memory only it can serve: IRAM for code.
 *
 * The heap's entry points are wrapped by the linker (CMakeLists), so every
 * component of the image goes through this without knowing.  Freeing and
 * resizing recognise our blocks by their address.  The questions `mem` and
 * the loader ask - how much is free, how large the largest piece, a walk of
 * the blocks - are wrapped too, so they see these blocks and not the big
 * used ones ESP-IDF thinks it lent.
 */
#include "sdkconfig.h"

#if CONFIG_ARGON_HEAP_TWOEND

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

#include "argon/port/mem.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

#define TE_IRAM     IRAM_ATTR
#define TE_LOCK()   portENTER_CRITICAL_SAFE(&s_lock)
#define TE_UNLOCK() portEXIT_CRITICAL_SAFE(&s_lock)
#include "twoend_core.h"

void  *__real_heap_caps_malloc_base(size_t size, uint32_t caps);
void  *__real_heap_caps_calloc_base(size_t n, size_t size, uint32_t caps);
void  *__real_heap_caps_realloc_base(void *ptr, size_t size, uint32_t caps);
void  *__real_heap_caps_aligned_alloc_base(size_t alignment, size_t size,
                                           uint32_t caps);
void   __real_heap_caps_free(void *ptr);
size_t __real_heap_caps_get_allocated_size(void *ptr);
size_t __real_heap_caps_get_free_size(uint32_t caps);
size_t __real_heap_caps_get_largest_free_block(uint32_t caps);
size_t __real_heap_caps_get_minimum_free_size(uint32_t caps);
void   __real_heap_caps_walk(uint32_t caps, heap_caps_walker_cb_t fn, void *ud);

/* What this memory is: internal DRAM, bytes and words, reachable by DMA. */
#define TE_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_32BIT | \
                 MALLOC_CAP_DMA | MALLOC_CAP_DEFAULT)
/* A piece smaller than this is left to ESP-IDF: an arena costs a slot. */
#define GRAB_MIN (2u * 1024u)

static te_heap_t s_heap;
static bool      s_on;
static struct { void *mem; size_t bytes; } s_grab[TE_MAX_ARENAS];

static inline bool IRAM_ATTR ours(const void *p)
{
    return s_on && p != NULL && te_owns(&s_heap, p);
}

static inline bool IRAM_ATTR can(uint32_t caps)
{
    return s_on && (caps & ~(uint32_t)TE_CAPS) == 0;
}

/* Every free piece of GRAB_MIN or more ESP-IDF's heap has now. */
static void grab(void)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    for (int i = 0; i < TE_MAX_ARENAS; i++) {
        if (s_grab[i].mem != NULL) {
            continue;
        }
        const size_t n = __real_heap_caps_get_largest_free_block(caps);
        if (n < GRAB_MIN) {
            break;
        }
        void *m = __real_heap_caps_malloc_base(n, caps);
        if (m == NULL) {
            break;
        }
        TE_LOCK();
        const bool added = te_add_arena(&s_heap, m, n);
        TE_UNLOCK();
        if (!added) {
            __real_heap_caps_free(m);
            break;
        }
        s_grab[i].mem = m;
        s_grab[i].bytes = n;
        s_on = true;
    }
}

__attribute__((constructor(101))) static void twoend_start(void)
{
    s_heap.small_max = CONFIG_ARGON_HEAP_TWOEND_SMALL;
    grab();
}

void ag_port_twoend_take_rest(void)
{
    grab();
}

static inline IRAM_ATTR void *took(void *p, size_t size, uint32_t caps)
{
#if CONFIG_ARGON_HEAP_WHO
    if (p != NULL) {
        esp_heap_trace_alloc_hook(p, size, caps);
    }
#else
    (void)size;
    (void)caps;
#endif
    return p;
}

void *IRAM_ATTR __wrap_heap_caps_malloc_base(size_t size, uint32_t caps)
{
    if (can(caps)) {
        void *p = te_alloc(&s_heap, size);
        if (p != NULL) {
            return took(p, size, caps);
        }
    }
    return __real_heap_caps_malloc_base(size, caps);
}

void *IRAM_ATTR __wrap_heap_caps_aligned_alloc_base(size_t alignment,
                                                    size_t size, uint32_t caps)
{
    if (can(caps)) {
        void *p = te_alloc_aligned(&s_heap, alignment, size);
        if (p != NULL) {
            return took(p, size, caps);
        }
    }
    return __real_heap_caps_aligned_alloc_base(alignment, size, caps);
}

void *IRAM_ATTR __wrap_heap_caps_calloc_base(size_t n, size_t size,
                                             uint32_t caps)
{
    size_t bytes;
    if (__builtin_mul_overflow(n, size, &bytes)) {
        return NULL;
    }
    if (can(caps)) {
        void *p = te_alloc(&s_heap, bytes);
        if (p != NULL) {
            memset(p, 0, bytes);
            return took(p, bytes, caps);
        }
    }
    return __real_heap_caps_calloc_base(n, size, caps);
}

void IRAM_ATTR __wrap_heap_caps_free(void *ptr)
{
    if (ours(ptr)) {
        te_free(&s_heap, ptr);
#if CONFIG_ARGON_HEAP_WHO
        esp_heap_trace_free_hook(ptr);
#endif
        return;
    }
    __real_heap_caps_free(ptr);
}

void *IRAM_ATTR __wrap_heap_caps_realloc_base(void *ptr, size_t size,
                                              uint32_t caps)
{
    if (ptr == NULL) {
        return __wrap_heap_caps_malloc_base(size, caps);
    }
    if (size == 0) {
        __wrap_heap_caps_free(ptr);
        return NULL;
    }
    if (!ours(ptr)) {
        return __real_heap_caps_realloc_base(ptr, size, caps);
    }
    if (can(caps)) {
        void *r = te_resize(&s_heap, ptr, size);
        if (r != NULL) {
#if CONFIG_ARGON_HEAP_WHO
            esp_heap_trace_free_hook(ptr);
            esp_heap_trace_alloc_hook(r, size, caps);
#endif
            return r;
        }
    }
    void *n = __wrap_heap_caps_malloc_base(size, caps);
    if (n == NULL) {
        return NULL;
    }
    const size_t old = te_usable(ptr);
    memcpy(n, ptr, old < size ? old : size);
    __wrap_heap_caps_free(ptr);
    return n;
}

size_t __wrap_heap_caps_get_allocated_size(void *ptr)
{
    if (ours(ptr)) {
        return te_usable(ptr);
    }
    return __real_heap_caps_get_allocated_size(ptr);
}

/* The arenas are used blocks to ESP-IDF; their free memory is added here. */
size_t __wrap_heap_caps_get_free_size(uint32_t caps)
{
    const size_t real = __real_heap_caps_get_free_size(caps);
    return can(caps) ? real + s_heap.free_bytes : real;
}

size_t __wrap_heap_caps_get_largest_free_block(uint32_t caps)
{
    const size_t real = __real_heap_caps_get_largest_free_block(caps);
    if (!can(caps)) {
        return real;
    }
    const size_t mine = te_largest(&s_heap);
    return mine > real ? mine : real;
}

size_t __wrap_heap_caps_get_minimum_free_size(uint32_t caps)
{
    const size_t real = __real_heap_caps_get_minimum_free_size(caps);
    return can(caps) ? real + s_heap.min_free : real;
}

/* A walk shows our blocks where ESP-IDF would show the arena as one. */
typedef struct {
    heap_caps_walker_cb_t fn;
    void                 *ud;
    walker_heap_into_t    heap;
} walk_ctx_t;

static void walk_one(void *ctx, const void *addr, size_t bytes, int used)
{
    walk_ctx_t *w = (walk_ctx_t *)ctx;
    walker_block_info_t b = {.ptr = (void *)addr, .size = bytes, .used = used != 0};
    (void)w->fn(w->heap, b, w->ud);
}

static bool walk_real(walker_heap_into_t heap, walker_block_info_t block, void *ud)
{
    walk_ctx_t *w = (walk_ctx_t *)ud;
    for (int i = 0; i < TE_MAX_ARENAS && s_grab[i].mem != NULL; i++) {
        if (block.ptr == s_grab[i].mem) {
            const uintptr_t lo = (uintptr_t)s_grab[i].mem;
            const uintptr_t hi = lo + s_grab[i].bytes;
            for (int a = 0; a < s_heap.arenas; a++) {
                if (s_heap.arena[a].lo >= lo && s_heap.arena[a].hi <= hi) {
                    w->heap = heap;
                    te_heap_t one = s_heap;      /* a view of just that arena */
                    one.arena[0] = s_heap.arena[a];
                    one.arenas = 1;
                    TE_LOCK();
                    te_walk(&one, walk_one, w);
                    TE_UNLOCK();
                }
            }
            return true;
        }
    }
    return w->fn(heap, block, w->ud);
}

void __wrap_heap_caps_walk(uint32_t caps, heap_caps_walker_cb_t fn, void *ud)
{
    if (!s_on) {
        __real_heap_caps_walk(caps, fn, ud);
        return;
    }
    walk_ctx_t w = {.fn = fn, .ud = ud};
    __real_heap_caps_walk(caps, walk_real, &w);
}

bool ag_port_twoend(ag_port_twoend_info_t *out)
{
    if (!s_on) {
        return false;
    }
    out->total = (uint32_t)s_heap.total;
    out->free = (uint32_t)s_heap.free_bytes;
    out->low = (uint32_t)s_heap.min_free;
    out->blocks = s_heap.used_blocks;
    out->arenas = (uint32_t)s_heap.arenas;
    out->small_max = (uint32_t)s_heap.small_max;
    for (int i = 0; i < s_heap.arenas && i < 8; i++) {
        out->lo[i] = (uint32_t)s_heap.arena[i].lo;
        out->hi[i] = (uint32_t)s_heap.arena[i].hi;
    }
    return true;
}

#endif /* CONFIG_ARGON_HEAP_TWOEND */
