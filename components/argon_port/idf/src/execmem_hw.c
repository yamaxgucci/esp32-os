/*
 * ArgonOS port: ESP-IDF - a code arena that lives in PSRAM, mapped into both
 * the data and the instruction bus so the loader can write it and the
 * processor can run it.
 *
 * The contract and the reason are in argon/port/execmem.h.  What is here is the
 * ESP32-S3 mechanics: the external-memory MMU maps 64 KB pages, PSRAM can carry
 * MMU_MEM_CAP_EXEC, and the same physical page reached through the data window
 * is writable.  So one PSRAM allocation, its physical address handed to
 * esp_mmu_map as a second (executable) view, gives the two aliases the loader
 * needs.
 *
 * A build without PSRAM, or a chip whose PSRAM does not reach the instruction
 * bus, compiles the stub at the bottom and answers -AG_ENOTSUP, and the loader
 * keeps its internal-SRAM arena.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/execmem.h>

#include "sdkconfig.h"

#if defined(CONFIG_SPIRAM) && defined(SOC_MMU_PAGE_SIZE)
#define AG_EXECMEM_HAVE 1
#endif

#if defined(CONFIG_SPIRAM)

#include <string.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mmu_map.h"
#include "soc/soc_caps.h"

#define TAG "execmem"

/* The external-memory MMU maps in pages this big; a mapping must be a whole
 * number of them and start on one. */
#ifndef CONFIG_MMU_PAGE_SIZE
#define AG_MMU_PAGE 0x10000u
#else
#define AG_MMU_PAGE CONFIG_MMU_PAGE_SIZE
#endif

/*
 * The writable allocation behind one mapping, kept so unmap can both return the
 * instruction-window view to the MMU and free the PSRAM.  One is enough: the
 * arena maps once for the life of the system.
 */
static void *s_writable_base;

static size_t align_up(size_t v, size_t a)
{
    return (v + a - 1u) & ~(a - 1u);
}

ag_err_t ag_port_execmem_map(size_t bytes, void **exec, void **writable,
                             ag_port_map_t *out)
{
    if (exec == NULL || writable == NULL || out == NULL || bytes == 0) {
        return -AG_EINVAL;
    }

    const size_t size = align_up(bytes, AG_MMU_PAGE);

    /*
     * A page-aligned PSRAM block.  The data window is a linear map, so a
     * 64 KB-aligned virtual address is a 64 KB-aligned physical one, which is
     * what esp_mmu_map needs for the executable view.
     */
    void *w = heap_caps_aligned_alloc(AG_MMU_PAGE, size, MALLOC_CAP_SPIRAM);
    if (w == NULL) {
        ESP_LOGW(TAG, "no %u KB of PSRAM for the code arena",
                 (unsigned)(size / 1024u));
        return -AG_ENOMEM;
    }

    esp_paddr_t   paddr = 0;
    mmu_target_t  target = MMU_TARGET_PSRAM0;
    esp_err_t     err = esp_mmu_vaddr_to_paddr(w, &paddr, &target);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "vaddr_to_paddr: %s", esp_err_to_name(err));
        heap_caps_free(w);
        return -AG_EIO;
    }

    void *x = NULL;
    err = esp_mmu_map(paddr, size, MMU_TARGET_PSRAM0,
                      MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ,
                      ESP_MMU_MMAP_FLAG_PADDR_SHARED, &x);
    if (err != ESP_OK) {
        /* The physical page is already mapped writable; PADDR_SHARED is meant
         * to allow a second view of it, but say so plainly if it is refused. */
        ESP_LOGW(TAG, "mmu_map exec: %s", esp_err_to_name(err));
        heap_caps_free(w);
        return -AG_ENOTSUP;
    }

    s_writable_base = w;
    *writable = w;
    *exec = x;
    *out = (ag_port_map_t)(uintptr_t)x;
    ESP_LOGI(TAG, "code arena in PSRAM: %u KB, write %p exec %p",
             (unsigned)(size / 1024u), w, x);
    return AG_OK;
}

void ag_port_execmem_sync(void *writable, size_t bytes)
{
    if (writable == NULL || bytes == 0) {
        return;
    }
    /*
     * Two steps, and both matter.  The bytes were written through the data
     * cache, so push them to PSRAM (C2M); then the instruction cache may hold
     * the old contents of these addresses, so drop them (INVALIDATE).  The
     * range is whatever the loader wrote and is not cache-line aligned, so
     * allow the unaligned form rather than rejecting it.
     */
    (void)esp_cache_msync(writable, bytes,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                          ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    (void)esp_cache_msync(writable, bytes,
                          ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                          ESP_CACHE_MSYNC_FLAG_TYPE_INST |
                          ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void ag_port_execmem_unmap(ag_port_map_t h)
{
    if (h != 0) {
        (void)esp_mmu_unmap((void *)(uintptr_t)h);
    }
    if (s_writable_base != NULL) {
        heap_caps_free(s_writable_base);
        s_writable_base = NULL;
    }
}

#else /* no PSRAM: nothing to map */

ag_err_t ag_port_execmem_map(size_t bytes, void **exec, void **writable,
                             ag_port_map_t *out)
{
    (void)bytes;
    (void)exec;
    (void)writable;
    (void)out;
    return -AG_ENOTSUP;
}

void ag_port_execmem_sync(void *writable, size_t bytes)
{
    (void)writable;
    (void)bytes;
}

void ag_port_execmem_unmap(ag_port_map_t h) { (void)h; }

#endif /* CONFIG_SPIRAM */

/*
 * CODE FROM THE INTERNAL HEAP (9 Oct 2026).
 *
 * On the S3 instruction RAM and data RAM are one SRAM seen through two buses,
 * and with memory protection off IDF's heap hands out a block of it as code:
 * heap_caps_malloc(MALLOC_CAP_EXEC) returns the instruction-bus address, and
 * heap_caps_free takes it back.  The loader used to reserve a fixed arena at
 * link time for this, held whether a program ran or not; this is the same
 * memory asked for when a program loads and given back when it ends.
 *
 * MALLOC_CAP_RETENTION is there to exclude RTC fast memory, which also says
 * EXEC but sits on a slow bus: of the regions that can execute, only the
 * D/IRAM carries it.  The block is over-allocated by 16 because the heap's
 * EXEC path returns a word past the start (it keeps the data-bus address
 * there), and the loader wants code 16-aligned.
 *
 * The loader writes through the data-bus alias of the block and the code runs
 * through the instruction-bus one.  Writing through the instruction bus works
 * too - the link-time arena has always been filled that way - but stores there
 * are half the speed: 40 KB took 400-460 us against 215 us through the data
 * bus, measured on the S3-Zero (9 Oct 2026).
 */
#if CONFIG_IDF_TARGET_ESP32S3 && !CONFIG_ESP_SYSTEM_MEMPROT_FEATURE
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

void *ag_port_execmem_alloc(size_t bytes, void **writable, void **raw)
{
    *writable = NULL;
    *raw = NULL;
    void *p = heap_caps_malloc(bytes + 16u, MALLOC_CAP_EXEC | MALLOC_CAP_RETENTION);
    if (p == NULL) {
        return NULL;
    }
    if (!esp_ptr_in_diram_iram(p)) {
        heap_caps_free(p);
        return NULL;
    }
    *raw = p;
    void *const exec = (void *)(((uintptr_t)p + 15u) & ~(uintptr_t)15u);
    *writable = esp_ptr_diram_iram_to_dram(exec);
    return exec;
}

void ag_port_execmem_free(void *raw) { heap_caps_free(raw); }

bool ag_port_execmem_from_data(void) { return true; }

#elif CONFIG_IDF_TARGET_ESP32
/*
 * The original ESP32 (9 Oct 2026, the CYD): what IDF leaves of the 128 KB of
 * instruction RAM goes to the heap as its own region - 66 KB on the CYD,
 * "heap_init: At 4008F770 len 00010890 (66 KiB): IRAM" - which nothing but
 * code can use: it takes 32-bit accesses only.  So a program's code can run
 * from there instead of from flash, and it costs no data memory at all.
 *
 * Only that region.  The D/IRAM above it also says EXEC, but it is data RAM
 * seen backwards through the instruction bus, and on this board data RAM is
 * what everything else is short of.  Written through the address it runs from,
 * in 32-bit words, as the link-time arena in the same memory always was.
 */
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

void *ag_port_execmem_alloc(size_t bytes, void **writable, void **raw)
{
    *writable = NULL;
    *raw = NULL;
    void *p = heap_caps_malloc(bytes + 16u, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
    if (p == NULL) {
        return NULL;
    }
    if (!esp_ptr_in_iram(p) || esp_ptr_in_diram_iram(p)) {
        heap_caps_free(p);
        return NULL;
    }
    *raw = p;
    void *const exec = (void *)(((uintptr_t)p + 15u) & ~(uintptr_t)15u);
    *writable = exec;
    return exec;
}

void ag_port_execmem_free(void *raw) { heap_caps_free(raw); }

bool ag_port_execmem_from_data(void) { return false; }

#else /* other chips: not tried, so not offered */

void *ag_port_execmem_alloc(size_t bytes, void **writable, void **raw)
{
    (void)bytes;
    *writable = NULL;
    *raw = NULL;
    return NULL;
}

void ag_port_execmem_free(void *raw) { (void)raw; }

bool ag_port_execmem_from_data(void) { return true; }

#endif
