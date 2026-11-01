/*
 * ArgonOS port: ESP-IDF - small blocks live apart (CONFIG_ARGON_SMALL_ZONE_KB).
 *
 * What cuts the heap into pieces is not the big blocks, it is the little ones
 * that outlive them.  Measured on the CYD (29 Oct 2026, tools/heapwho.py): a
 * FreeRTOS mutex is 84 bytes, and they are made when first needed - the SHA
 * engine's lock when the radio first hashes, newlib's locks on first use, a
 * semaphore for each task that first calls the radio - in the middle of
 * whatever large blocks are being taken at that moment.  The large ones go;
 * the 84 bytes stay until the next reset, a wall between two holes.  After a
 * session with the radio switched off the largest free piece of the 113 KB
 * DRAM region was 11.8 KB; laid out again with every block of 128 bytes or
 * less elsewhere it would have been 22.3 KB.  Free memory is the same either
 * way - what changes is how much of it is in one piece.
 *
 * So the small ones get a heap of their own: one block taken at start-up,
 * before anything else is, and run as a heap inside it.  Every request for
 * ordinary internal memory of ARGON_SMALL_ZONE_MAX bytes or less is served
 * there; when it is full, from the ordinary heap, as before.  Anyone can ask:
 * the zone is the heap's own entry points, wrapped by the linker (CMakeLists),
 * so ESP-IDF's components, the radio's blobs and the kernel all go through it
 * without knowing.
 *
 * Wrapped: heap_caps_malloc_base, _calloc_base, _realloc_base, heap_caps_free
 * and heap_caps_get_allocated_size - every path from malloc, calloc, realloc,
 * free and heap_caps_* in other files.  Aligned requests are left alone; a
 * block of the zone is recognised by its address wherever it is given back.
 */
#include "sdkconfig.h"

#if CONFIG_ARGON_SMALL_ZONE_KB > 0

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "multi_heap.h"

#include "argon/port/mem.h"

void *__real_heap_caps_malloc_base(size_t size, uint32_t caps);
void *__real_heap_caps_calloc_base(size_t n, size_t size, uint32_t caps);
void *__real_heap_caps_realloc_base(void *ptr, size_t size, uint32_t caps);
void  __real_heap_caps_free(void *ptr);
size_t __real_heap_caps_get_allocated_size(void *ptr);

/* What the zone's memory is: internal DRAM, bytes and words, reachable by
 * DMA.  A request asking for nothing outside that may be served from it. */
#define ZONE_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_32BIT | \
                   MALLOC_CAP_DMA | MALLOC_CAP_DEFAULT)

static multi_heap_handle_t s_zone;
static uintptr_t           s_lo, s_hi;
static portMUX_TYPE        s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t            s_spilled;   /* small, but the zone was full */
static uint32_t            s_lent;      /* big, but the heap was full and the zone was not */

static inline bool IRAM_ATTR in_zone(const void *p)
{
    return (uintptr_t)p >= s_lo && (uintptr_t)p < s_hi;
}

static inline bool IRAM_ATTR zone_can(uint32_t caps)
{
    return s_zone != NULL && (caps & ~(uint32_t)ZONE_CAPS) == 0;
}

static inline bool IRAM_ATTR for_zone(size_t size, uint32_t caps)
{
    return size != 0 && size <= CONFIG_ARGON_SMALL_ZONE_MAX && zone_can(caps);
}

/* First, so that it is the first block of the heap and walls nothing in. */
__attribute__((constructor(101))) static void zone_start(void)
{
    const size_t bytes = (size_t)CONFIG_ARGON_SMALL_ZONE_KB * 1024u;
    void *mem = __real_heap_caps_malloc_base(bytes, MALLOC_CAP_INTERNAL |
                                                    MALLOC_CAP_8BIT |
                                                    MALLOC_CAP_DMA);
    if (mem == NULL) {
        return;
    }
    multi_heap_handle_t h = multi_heap_register(mem, bytes);
    if (h == NULL) {
        __real_heap_caps_free(mem);
        return;
    }
    multi_heap_set_lock(h, &s_lock);
    s_lo = (uintptr_t)mem;
    s_hi = (uintptr_t)mem + bytes;
    s_zone = h;
}

static void *IRAM_ATTR zone_alloc(size_t size, uint32_t caps)
{
    void *p = multi_heap_malloc(s_zone, (size + 3u) & ~3u);
    if (p == NULL) {
        return NULL;
    }
#if CONFIG_ARGON_HEAP_WHO
    esp_heap_trace_alloc_hook(p, size, caps);
#else
    (void)caps;
#endif
    return p;
}

/*
 * Small requests try the zone first; anything the ordinary heap refuses tries
 * it last.  What the zone has free is memory like any other once the heap is
 * out of it - kept back from the big requests, it was the 2 KB the emulator of
 * a 64 KB machine was refused with 3 KB sitting idle in the zone (the CYD,
 * 29 Oct 2026).  A big block lent there comes back by its address like the
 * small ones.
 */
void *IRAM_ATTR __wrap_heap_caps_malloc_base(size_t size, uint32_t caps)
{
    const bool small = for_zone(size, caps);
    if (small) {
        void *p = zone_alloc(size, caps);
        if (p != NULL) {
            return p;
        }
        s_spilled++;
    }
    void *p = __real_heap_caps_malloc_base(size, caps);
    if (p == NULL && !small && size != 0 && zone_can(caps)) {
        p = zone_alloc(size, caps);
        if (p != NULL) {
            s_lent++;
        }
    }
    return p;
}

void *IRAM_ATTR __wrap_heap_caps_calloc_base(size_t n, size_t size,
                                             uint32_t caps)
{
    size_t bytes;
    if (__builtin_mul_overflow(n, size, &bytes)) {
        return NULL;
    }
    const bool small = for_zone(bytes, caps);
    if (small) {
        void *p = zone_alloc(bytes, caps);
        if (p != NULL) {
            memset(p, 0, bytes);
            return p;
        }
        s_spilled++;
    }
    void *p = __real_heap_caps_calloc_base(n, size, caps);
    if (p == NULL && !small && bytes != 0 && zone_can(caps)) {
        p = zone_alloc(bytes, caps);
        if (p != NULL) {
            memset(p, 0, bytes);
            s_lent++;
        }
    }
    return p;
}

void IRAM_ATTR __wrap_heap_caps_free(void *ptr)
{
    if (ptr != NULL && in_zone(ptr)) {
        multi_heap_free(s_zone, ptr);
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
    if (ptr == NULL || !in_zone(ptr)) {
        /* a small block grown out of the ordinary heap stays there: moving
         * it would save nothing the next reset does not */
        return __real_heap_caps_realloc_base(ptr, size, caps);
    }
    if (size == 0) {
        __wrap_heap_caps_free(ptr);
        return NULL;
    }
    if (for_zone(size, caps)) {
        void *r = multi_heap_realloc(s_zone, ptr, (size + 3u) & ~3u);
        if (r != NULL) {
#if CONFIG_ARGON_HEAP_WHO
            esp_heap_trace_free_hook(ptr);
            esp_heap_trace_alloc_hook(r, size, caps);
#endif
            return r;
        }
    }
    /* grown past the zone's sizes, or no room left in it */
    void *n = __real_heap_caps_malloc_base(size, caps);
    if (n == NULL) {
        return NULL;
    }
    const size_t old = multi_heap_get_allocated_size(s_zone, ptr);
    memcpy(n, ptr, old < size ? old : size);
    __wrap_heap_caps_free(ptr);
    return n;
}

size_t __wrap_heap_caps_get_allocated_size(void *ptr)
{
    if (ptr != NULL && in_zone(ptr)) {
        return multi_heap_get_allocated_size(s_zone, ptr);
    }
    return __real_heap_caps_get_allocated_size(ptr);
}

bool ag_port_small_zone(ag_port_zone_info_t *out)
{
    if (s_zone == NULL) {
        return false;
    }
    multi_heap_info_t i;
    multi_heap_get_info(s_zone, &i);
    out->total = (uint32_t)(s_hi - s_lo);
    out->free = (uint32_t)i.total_free_bytes;
    out->low = (uint32_t)i.minimum_free_bytes;
    out->blocks = (uint32_t)i.allocated_blocks;
    out->max = CONFIG_ARGON_SMALL_ZONE_MAX;
    out->spilled = s_spilled;
    out->lent = s_lent;
    return true;
}

#endif /* CONFIG_ARGON_SMALL_ZONE_KB > 0 */
