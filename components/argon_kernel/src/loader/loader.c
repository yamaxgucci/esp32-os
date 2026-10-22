/*
 * ArgonOS - placing an application image in memory.
 *
 * Reading the file, finding room for its two parts and relocating them.  What
 * happens after that - the task it runs on, the memory it is allowed to ask for,
 * and giving all of it back afterwards - belongs to the process layer in
 * src/proc/, so that nothing here has to know what a process is.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/loader.h>

#include <string.h>

#include <argon/arena.h>
#include <argon/axesig.h>
#include <argon/log.h>
#include <argon/module.h>
#include <argon/proc.h>
#include <argon/path.h>
#include <argon/place.h>
#include <argon/port/place.h>
#include <argon/libc.h>
#include <argon/vfs.h>

#include <argon/port/mem.h>
#include <argon/port/execmem.h>
#include "loader/appfs.h"

/*
 * A .AXE is read whole before anything is placed, so that a truncated file is
 * refused rather than half-loaded.  The limit is generous because the data part
 * may carry a large initialised table; it is not a limit on how much memory an
 * application can use, only on how much of it arrives in the file.
 */
#define AG_LOADER_MAX_FILE (4u * 1024u * 1024u)

/* Reads a whole file, refusing anything implausible for an application. */
static ag_err_t read_whole(const char *path, const char *cwd, uint8_t **out,
                           size_t *out_size)
{
    const ag_handle_t h = ag_vfs_open(path, cwd, AG_O_RDONLY);
    if (h < 0) {
        return h;
    }

    const int64_t size = ag_vfs_seek(h, 0, AG_SEEK_END);
    if (size < (int64_t)sizeof(ag_axe_header_t) ||
        size > (int64_t)AG_LOADER_MAX_FILE) {
        ag_vfs_close(h);
        return -AG_EFORMAT;
    }
    ag_vfs_seek(h, 0, AG_SEEK_SET);

    uint8_t *buf = (uint8_t *)ag_port_alloc((size_t)size,
                                               AG_MEM_SLOW |
                                                   AG_MEM_BYTE);
    if (buf == NULL) {
        buf = (uint8_t *)ag_port_alloc((size_t)size,
                                          AG_MEM_FAST | AG_MEM_BYTE);
    }
    if (buf == NULL) {
        ag_vfs_close(h);
        return -AG_ENOMEM;
    }

    size_t got = 0;
    while (got < (size_t)size) {
        const int32_t n = ag_vfs_read(h, buf + got, (size_t)size - got);
        if (n <= 0) {
            ag_port_free(buf);
            ag_vfs_close(h);
            return (n < 0) ? n : -AG_EFORMAT;
        }
        got += (size_t)n;
    }
    ag_vfs_close(h);

    *out = buf;
    *out_size = got;
    return AG_OK;
}

/*
 * The code arena: memory an image can be both executed from and written to.
 *
 * On this family the instruction and data buses reach memory through different
 * address windows, and most memory is one or the other.  Flash and PSRAM in the
 * instruction window can be read and executed but not written, so an image
 * cannot be placed there through the same address it runs from.  The D/IRAM
 * region is the only memory that is executable and writable at once, which is
 * what placing code needs for the common case.
 *
 * When the arena is too small, R-1 relocates into a PSRAM scratch buffer,
 * programs the appfs flash partition, and executes through an instruction-window
 * mmap - write-then-XIP, because flash is not writable via the I-bus.
 */
#ifndef CONFIG_ARGON_APP_ARENA_KB
#define CONFIG_ARGON_APP_ARENA_KB 64
#endif
#define AG_APP_ARENA_BYTES ((size_t)CONFIG_ARGON_APP_ARENA_KB * 1024u)
#define AG_APP_ARENA_MIN_KB 4u

static uint8_t s_arena[AG_APP_ARENA_BYTES]
    __attribute__((aligned(16), section(".iram.bss.ag_app_arena")));

#define AG_LOADER_SLOTS (AG_PROC_MAX + AG_MODULE_MAX)

static ag_arena_block_t s_arena_blocks[AG_LOADER_SLOTS];
static ag_arena_t       s_code;
/* Usable bytes from the linked ceiling; set from SYSTEM.CFG before first load. */
static size_t           s_arena_usable = AG_APP_ARENA_BYTES;

static void arena_ready(void)
{
    if (s_code.base == NULL) {
        ag_arena_init(&s_code, s_arena, s_arena_usable, s_arena_blocks,
                      AG_LOADER_SLOTS);
    }
}

#if defined(CONFIG_ARGON_APP_ARENA_PSRAM)
/*
 * S-1: a second, larger arena that lives in PSRAM and is mapped into the
 * instruction window, so code executes from it while the internal-SRAM arena
 * above stays small and out of the radios' way.
 *
 * The allocator works on the writable (data-window) alias, because that is what
 * it hands out and what the loader writes through; the exec alias is the same
 * block seen through the instruction window, a fixed distance away, so the
 * executable address of any allocation is (that allocation) + s_psram_bias.
 */
#include <argon/port/execmem.h>

#ifndef CONFIG_ARGON_APP_ARENA_PSRAM_KB
#define CONFIG_ARGON_APP_ARENA_PSRAM_KB 512
#endif
#define AG_PSRAM_ARENA_BYTES ((size_t)CONFIG_ARGON_APP_ARENA_PSRAM_KB * 1024u)

static ag_arena_block_t s_psram_blocks[AG_LOADER_SLOTS];
static ag_arena_t       s_psram;
static uint8_t         *s_psram_write;   /* writable base (data window)    */
static intptr_t         s_psram_bias;    /* exec = write + bias            */
static ag_port_map_t    s_psram_map;
static bool             s_psram_tried;

/* True once the PSRAM arena is mapped and usable. */
static bool psram_ready(void)
{
    if (s_psram.base != NULL) {
        return true;
    }
    if (s_psram_tried) {
        return false; /* asked once, the port said no */
    }
    s_psram_tried = true;

    void        *exec = NULL;
    void        *write = NULL;
    const ag_err_t err =
        ag_port_execmem_map(AG_PSRAM_ARENA_BYTES, &exec, &write, &s_psram_map);
    if (err != AG_OK) {
        ag_log(AG_LOG_WARN, "loader",
               "PSRAM code arena unavailable (%d); using internal SRAM",
               (int)err);
        return false;
    }
    s_psram_write = (uint8_t *)write;
    s_psram_bias = (intptr_t)((uint8_t *)exec - (uint8_t *)write);
    ag_arena_init(&s_psram, write, AG_PSRAM_ARENA_BYTES, s_psram_blocks,
                  AG_LOADER_SLOTS);
    ag_log(AG_LOG_INFO, "loader", "PSRAM code arena: %u KB",
           (unsigned)CONFIG_ARGON_APP_ARENA_PSRAM_KB);
    return true;
}
#endif /* CONFIG_ARGON_APP_ARENA_PSRAM */

size_t ag_loader_set_arena_kb(uint32_t kb)
{
    if (s_code.base != NULL) {
        return s_arena_usable;
    }
    if (kb < AG_APP_ARENA_MIN_KB) {
        kb = AG_APP_ARENA_MIN_KB;
    }
    if (kb > (uint32_t)CONFIG_ARGON_APP_ARENA_KB) {
        kb = (uint32_t)CONFIG_ARGON_APP_ARENA_KB;
    }
    s_arena_usable = (size_t)kb * 1024u;
    return s_arena_usable;
}

size_t ag_loader_arena_size(void) { return s_arena_usable; }

size_t ag_loader_arena_free(void)
{
    arena_ready();
    return ag_arena_free_bytes(&s_code);
}

size_t ag_loader_arena_largest(void)
{
    arena_ready();
    return ag_arena_largest_free(&s_code, 16);
}

bool ag_loader_arena_busy(void)
{
    arena_ready();
    return ag_arena_blocks(&s_code) > 0;
}

/*
 * Placing an image into the code arena, which is instruction memory.
 *
 * Every store here is an aligned 32-bit word, and that is a requirement, not a
 * speed trick.  On the original ESP32 the arena lives in SRAM0, reached only
 * through the instruction bus, and that bus does not do byte or half-word
 * accesses: a single-byte store raises LoadStoreError.  memcpy ends an odd
 * length with exactly those stores, so the obvious fallback would fault on a
 * ragged image and be invisible on every image whose length happened to be a
 * multiple of four.  (The S3 has no such restriction, which is why this went
 * unnoticed - the same reason MALLOC_CAP_EXEC reports zero on the ESP32, trap
 * 3.)  The source is an ordinary buffer and may be read any way at all.
 *
 * The tail is read-modify-written: arena blocks are 16-aligned, so the word
 * holding a ragged end is inside the arena and its spare bytes belong to no
 * one.
 */
static void copy_image(void *dst, const void *src, size_t bytes)
{
    uint32_t      *d = (uint32_t *)dst; /* arena blocks are 16-aligned */
    const uint8_t *s = (const uint8_t *)src;
    const size_t   words = bytes / 4u;

    if (((uintptr_t)s & 3u) == 0) {
        const uint32_t *sw = (const uint32_t *)src;
        for (size_t i = 0; i < words; i++) {
            d[i] = sw[i];
        }
    } else {
        for (size_t i = 0; i < words; i++) {
            uint32_t w;
            memcpy(&w, s + i * 4u, sizeof(w));
            d[i] = w;
        }
    }

    const size_t tail = bytes & 3u;
    if (tail != 0) {
        uint32_t w = d[words];
        uint8_t  b[4];

        memcpy(b, &w, sizeof(b));
        for (size_t i = 0; i < tail; i++) {
            b[i] = s[words * 4u + i];
        }
        memcpy(&w, b, sizeof(w));
        d[words] = w;
    }
}

/*
 * An application's writable memory - its data part, its relocation scratch, and
 * (in proc.c) its arena - does not need to be DMA-capable, but the radio's
 * buffers do, and on a no-PSRAM board they are both carved from the same
 * internal RAM.  Placed in the general internal heap, a resident application
 * splits the one large DMA-capable region the Wi-Fi driver needs a ~36 KB
 * contiguous slice of, and the driver then fails with ENOMEM even though far
 * more than 36 KB is free.  So the order is: PSRAM first (an S3 puts all of this
 * off-chip and the question never arises), then byte-accessible D/IRAM (which
 * the DMA path does not use), and only then ordinary internal RAM as a last
 * resort.  This keeps the DMA-capable DRAM whole for the radio.
 */
static void *data_alloc(size_t bytes)
{
    /* Room for the guard, and the guard written into it: see guard_arm. */
    const size_t want = bytes + AG_APP_GUARD_BYTES;

    /* [memory] app.data: psram, iram by default.  "iram" means the D/IRAM
     * the DMA path does not use first, then ordinary internal RAM. */
    ag_place_kind_t kinds[AG_PLACE_KINDS];
    const unsigned  n = ag_place_order("app.data", kinds);
    for (unsigned k = 0; k < n; k++) {
        void *p = NULL;
        if (kinds[k] == AG_PLACE_PSRAM) {
            p = ag_port_alloc_aligned(16, want, AG_MEM_SLOW | AG_MEM_BYTE);
        } else if (kinds[k] == AG_PLACE_IRAM) {
            p = ag_port_alloc_aligned(16, want, AG_MEM_IRAM8);
            if (p == NULL) {
                p = ag_port_alloc_aligned(16, want, AG_MEM_FAST | AG_MEM_BYTE);
            }
        }
        if (p != NULL) {
            ag_place_note("app.data", kinds[k], (int32_t)want);
            return p;
        }
    }
    return NULL;
}

/*
 * The pattern, and where it went.
 *
 * `end` is the first byte past the application's data.  For an image with its
 * own data allocation that is inside the block data_alloc reserved; for a
 * contiguous image it is inside the code allocation, which place_arena sizes
 * with the same allowance.  Either way the bytes belong to us, which is the
 * whole point - a guard written into somebody else's block would be the very
 * corruption it is meant to catch.
 */
static void guard_arm(ag_loaded_app_t *out, void *end)
{
    if (end == NULL) {
        out->guard = NULL;
        return;
    }
    memset(end, AG_APP_GUARD_BYTE, AG_APP_GUARD_BYTES);
    out->guard = end;
}

bool ag_loader_guard_broken(const ag_loaded_app_t *app, size_t *bytes_past)
{
    if (app == NULL || app->guard == NULL) {
        return false;
    }
    const uint8_t *g = (const uint8_t *)app->guard;
    size_t         last = 0;
    bool           broken = false;

    for (size_t i = 0; i < AG_APP_GUARD_BYTES; i++) {
        if (g[i] != AG_APP_GUARD_BYTE) {
            broken = true;
            last = i + 1u;
        }
    }
    if (broken && bytes_past != NULL) {
        /*
         * At least this far: the furthest disturbed byte inside the guard.  A
         * write that cleared the whole guard went further still, and the guard
         * cannot say how much - what it can say is that it happened and who
         * did it, which is what was missing.
         */
        *bytes_past = last;
    }
    return broken;
}

static void *scratch_alloc(size_t bytes)
{
    void *p = ag_port_alloc_aligned(16, bytes, AG_MEM_SLOW | AG_MEM_BYTE);
    if (p == NULL) {
        p = ag_port_alloc_aligned(16, bytes, AG_MEM_IRAM8);
    }
    if (p == NULL) {
        p = ag_port_alloc_aligned(16, bytes, AG_MEM_FAST | AG_MEM_BYTE);
    }
    return p;
}

static void release_image(ag_loaded_app_t *app)
{
    if (app->data_owned != NULL) {
        ag_place_note("app.data",
                      ag_port_ptr_is_slow(app->data_owned) ? AG_PLACE_PSRAM
                                                           : AG_PLACE_IRAM,
                      -(int32_t)(app->header.data.size + AG_APP_GUARD_BYTES));
        ag_port_free(app->data_owned);
        app->data_owned = NULL;
    }
    if (app->place.code != NULL) {
        ag_place_note("app.code",
                      app->code_from_xip     ? AG_PLACE_FLASH
                      : app->code_from_psram ? AG_PLACE_PSRAM
                                             : AG_PLACE_IRAM,
                      app->code_from_xip
                          ? -(int32_t)app->header.code.size
                          : -(int32_t)app->place.code_capacity);
    }
    if (app->code_scratch != NULL) {
        ag_port_free(app->code_scratch);
        app->code_scratch = NULL;
    }
    if (app->xip_slot != NULL) {
        ag_appfs_release((ag_appfs_slot_t *)app->xip_slot);
        app->xip_slot = NULL;
    }
#if defined(CONFIG_ARGON_APP_ARENA_PSRAM)
    if (app->code_from_psram) {
        /* The PSRAM arena tracks the writable alias, not the exec one; the
         * whole-arena mapping stays for the next image. */
        if (app->place.code_writable != NULL &&
            !ag_arena_free(&s_psram, app->place.code_writable)) {
            ag_log(AG_LOG_ERROR, "loader", "PSRAM arena does not own %p",
                   app->place.code_writable);
        }
    } else
#endif
    if (app->code_heap != NULL) {
        ag_port_execmem_free(app->code_heap);
        app->code_heap = NULL;
    } else if (app->place.code != NULL && !app->code_from_xip) {
        if (!ag_arena_free(&s_code, app->place.code)) {
            ag_log(AG_LOG_ERROR, "loader", "arena does not own %p",
                   app->place.code);
        }
    }
    memset(&app->place, 0, sizeof(app->place));
    app->code_from_xip = false;
    app->code_from_psram = false;
}

static ag_err_t place_data(const ag_axe_header_t *header, ag_loaded_app_t *out,
                           void *code, size_t code_bytes);

/*
 * IRAM arena path: code is writable and executable in place.
 * Contiguous images always take this path (data must sit next to code).
 */
static ag_err_t place_arena(const ag_axe_header_t *header, ag_loaded_app_t *out)
{
    arena_ready();

    const bool contiguous = (header->flags & AG_AXE_CONTIGUOUS) != 0;
    const size_t code_bytes =
        contiguous ? (size_t)header->code.size + header->data.size +
                         AG_APP_GUARD_BYTES
                   : (size_t)header->code.size;

    void *code = ag_arena_alloc(&s_code, code_bytes, 16);
    if (code == NULL) {
        ag_log(AG_LOG_ERROR, "loader",
               "%u bytes of code will not fit the arena: %u of %u free, "
               "largest block %u",
               (unsigned)code_bytes, (unsigned)ag_arena_free_bytes(&s_code),
               (unsigned)sizeof(s_arena),
               (unsigned)ag_arena_largest_free(&s_code, 16));
        return -AG_ENOMEM;
    }

    const ag_err_t err = place_data(header, out, code, code_bytes);
    if (err != AG_OK) {
        (void)ag_arena_free(&s_code, code);
    }
    return err;
}

/*
 * The half of placing an image that does not depend on where its code went:
 * the code block is given, and the data goes inside it (contiguous) or into
 * an allocation of its own.  On failure `out->place` is cleared and the code
 * block is the caller's to give back.
 */
static ag_err_t place_data(const ag_axe_header_t *header, ag_loaded_app_t *out,
                           void *code, size_t code_bytes)
{
    const bool contiguous = (header->flags & AG_AXE_CONTIGUOUS) != 0;

    out->place.code = code;
    out->place.code_capacity = code_bytes;
    out->place.code_writable = NULL;
    out->code_from_xip = false;

    if (header->data.size == 0) {
        return AG_OK;
    }

    if (contiguous) {
        out->place.data = (uint8_t *)code + header->code.size;
        out->place.data_capacity = header->data.size;
        guard_arm(out, (uint8_t *)out->place.data + header->data.size);
        return AG_OK;
    }

    void *data = data_alloc(header->data.size);
    if (data == NULL) {
        /*
         * Said here, with the numbers of the allocation that actually failed.
         * Both failures in this function used to arrive at the caller as a
         * bare -AG_ENOMEM and be reported as "the code will not fit the
         * arena", which on the first board with no PSRAM sent the search to
         * the arena - the one thing that had room to spare.  The data part
         * does not come out of the arena at all.
         */
        ag_log(AG_LOG_ERROR, "loader",
               "%u bytes of data will not fit: %u free outside the arena, "
               "largest block %u",
               (unsigned)header->data.size,
               (unsigned)(ag_port_mem_free(AG_MEM_SLOW) +
                          ag_port_mem_free(AG_MEM_FAST)),
               (unsigned)ag_port_mem_largest(AG_MEM_FAST));
        memset(&out->place, 0, sizeof(out->place));
        return -AG_ENOMEM;
    }

    out->place.data = data;
    out->place.data_capacity = header->data.size;
    out->data_owned = data;
    guard_arm(out, (uint8_t *)data + header->data.size);
    return AG_OK;
}

/*
 * CODE FROM THE INTERNAL HEAP (9 Oct 2026).
 *
 * The link-time arena is internal SRAM reserved for code whether a program
 * runs or not.  On the S3-Zero it was cut to 16 KB to give that memory back -
 * and then DX7, 29 KB of code, went to the PSRAM arena while 250 KB of
 * internal SRAM stood free, which is the memory that had been freed for
 * exactly such programs (Maxim, 9 Oct).  So a program that does not fit the
 * arena takes its code from the internal heap when the port can make heap
 * memory executable (the S3), and gives it back when it ends.
 *
 * Only while that leaves what is kept for arriving devices
 * (ag_proc_keep_internal) and AG_LOADER_HEAP_CODE_FLOOR on top: a radio
 * brought up later wants one ~36 KB contiguous block of the same memory, and
 * a program's code must not be what refuses it.  Past that the PSRAM arena,
 * then flash, as before.
 */
#define AG_LOADER_HEAP_CODE_FLOOR (40u * 1024u)

static bool heap_code_room(size_t bytes)
{
    /* The ESP32's instruction RAM is nobody's data memory: the heap's own
     * answer is the only one that matters there. */
    if (!ag_port_execmem_from_data()) {
        return true;
    }
    return ag_port_mem_free(AG_MEM_FAST) >=
           bytes + 16u + AG_LOADER_HEAP_CODE_FLOOR + ag_proc_kept_internal();
}

static ag_err_t place_heap(const ag_axe_header_t *header, ag_loaded_app_t *out)
{
    const bool   contiguous = (header->flags & AG_AXE_CONTIGUOUS) != 0;
    /* A contiguous image's data lives in the code block, and the ESP32's
     * instruction RAM takes no byte stores. */
    if (contiguous && !ag_port_execmem_from_data()) {
        return -AG_ENOTSUP;
    }
    const size_t code_bytes =
        contiguous ? (size_t)header->code.size + header->data.size +
                         AG_APP_GUARD_BYTES
                   : (size_t)header->code.size;

    void *raw = NULL;
    void *writable = NULL;
    void *code = ag_port_execmem_alloc(code_bytes, &writable, &raw);
    if (code == NULL) {
        return -AG_ENOMEM;
    }
    const ag_err_t err = place_data(header, out, code, code_bytes);
    if (err != AG_OK) {
        ag_port_execmem_free(raw);
        return err;
    }
    /* Filled through the data bus, run through the instruction bus. */
    out->place.code_writable = writable;
    out->code_heap = raw;
    return AG_OK;
}

#if defined(CONFIG_ARGON_APP_ARENA_PSRAM)
/*
 * S-1: code in the PSRAM arena, data in ordinary PSRAM.
 *
 * The code block is written through its data-window address and executed
 * through the instruction-window one; the loader's place struct carries both,
 * so this only has to hand out the pair.  Split images only, for the same
 * reason flash XIP is: a contiguous image's data sits inside the code block and
 * would have to be written through an address the processor is fetching from.
 */
static ag_err_t place_psram(const ag_axe_header_t *header, ag_loaded_app_t *out)
{
    if ((header->flags & AG_AXE_CONTIGUOUS) != 0) {
        return -AG_ENOTSUP;
    }

    void *writable = ag_arena_alloc(&s_psram, header->code.size, 16);
    if (writable == NULL) {
        return -AG_ENOMEM;
    }

    void *data = NULL;
    if (header->data.size > 0) {
        data = data_alloc(header->data.size);
        if (data == NULL) {
            (void)ag_arena_free(&s_psram, writable);
            ag_log(AG_LOG_ERROR, "loader",
                   "%u bytes of data will not fit outside the arena",
                   (unsigned)header->data.size);
            return -AG_ENOMEM;
        }
    }

    out->place.code = (uint8_t *)writable + s_psram_bias; /* exec view */
    out->place.code_capacity = header->code.size;
    out->place.code_writable = writable;                  /* write view */
    out->place.data = data;
    out->place.data_capacity = header->data.size;
    out->data_owned = data;
    guard_arm(out, (uint8_t *)data + header->data.size);
    out->code_from_psram = true;
    return AG_OK;
}
#endif /* CONFIG_ARGON_APP_ARENA_PSRAM */

/*
 * R-1: relocate into PSRAM, program appfs, execute from flash XIP.
 * Split images only - contiguous needs writable data beside code.
 */
static ag_err_t place_xip(const ag_axe_header_t *header, ag_loaded_app_t *out)
{
    if ((header->flags & AG_AXE_CONTIGUOUS) != 0) {
        return -AG_ENOTSUP;
    }

    ag_appfs_slot_t *slot = NULL;
    void            *exec = NULL;
    ag_err_t err = ag_appfs_reserve(header->code.size, &slot, &exec);
    if (err != AG_OK) {
        return err;
    }

    /*
     * Data first, scratch second, and the order is the point.
     *
     * The scratch is freed as soon as the code has been written to flash, while
     * the data stays for as long as the process does.  Taking the scratch first
     * puts the long-lived block above the short-lived one, so when the scratch
     * goes it leaves a hole with allocations on both sides - and the largest free
     * block afterwards is that hole rather than the whole remainder.
     *
     * Measured: an application whose arena request of 33 KB was refused with
     * 35 KB free, because the free memory was in two pieces of twenty-two and
     * thirteen.  Reversed, the scratch sits between the data and the free tail
     * and merges into it when released.
     */
    void *data = NULL;
    if (header->data.size > 0) {
        data = data_alloc(header->data.size);
        if (data == NULL) {
            ag_appfs_release(slot);
            return -AG_ENOMEM;
        }
    }

    /*
     * No code-size scratch here any more: the streamed path (xip_load_chunked)
     * relocates and programs the appfs slot a page at a time with a small
     * rolling buffer, which is what lets a large image load when little
     * contiguous memory is left - e.g. after the radio is already up.  The
     * signed path (load_whole) still needs a whole-code buffer, so it allocates
     * one for itself.
     */
    out->place.code = exec;
    out->place.code_capacity = header->code.size;
    out->place.code_writable = NULL;
    out->place.data = data;
    out->place.data_capacity = header->data.size;
    out->data_owned = data;
    guard_arm(out, (uint8_t *)data + header->data.size);
    out->code_scratch = NULL;
    out->xip_slot = slot;
    out->code_from_xip = true;
    return AG_OK;
}

/*
 * Chunked flash XIP: relocate and program the reserved appfs slot one page at a
 * time.  Code relocations and the API-table binding for words in the code part
 * are applied to each page before it is written; data relocations (and an
 * api_slot that lives in the data part) are applied to the data buffer in RAM.
 * The only transient buffer is one page, not the whole code, so the load fits in
 * far less memory.
 */
#define AG_XIP_CHUNK 4096u

static ag_err_t read_at(ag_handle_t h, uint64_t at, void *dst, size_t bytes);

static ag_err_t xip_load_chunked(ag_handle_t h, const ag_axe_header_t *header,
                                 ag_loaded_app_t *out)
{
    const uint32_t code_base = header->code.base;
    const uint32_t data_base = header->data.base;
    const uint32_t code_addr = (uint32_t)(uintptr_t)out->place.code;
    const uint32_t data_bias =
        (out->place.data != NULL)
            ? (uint32_t)(uintptr_t)out->place.data - data_base
            : 0u;
    const uint32_t code_bias = code_addr - code_base;
    const uint32_t data_addr = (uint32_t)(uintptr_t)out->place.data;
    const uint32_t api_val = (uint32_t)(uintptr_t)ag_loader_api();
    const uint32_t api_at = header->api_slot;
    const bool     api_in_code =
        api_at >= code_base && api_at + 4u <= code_base + header->code.size;

    /*
     * The relocation table, whole, and it grows with the image: a 449 KB
     * application brought 5109 of them, which is 20 KB.  It used to be asked
     * of internal SRAM, and that is the one budget flash XIP is chosen to
     * avoid - the image only got this far because the arena could not hold it.
     * With a 16 KB task stack already taken out of internal memory there were
     * 15 KB left in one piece, and the load failed with -ENOMEM after the
     * appfs slot had been reserved.
     *
     * scratch_alloc asks extended memory first, which is where a table this
     * size belongs; it is read once and then walked per page, so its latency
     * is not on any path that matters.
     */
    uint32_t *rel = NULL;
    if (header->reloc_count > 0) {
        const size_t rbytes = (size_t)header->reloc_count * sizeof(uint32_t);
        rel = (uint32_t *)scratch_alloc(rbytes);
        if (rel == NULL) {
            return -AG_ENOMEM;
        }
        const ag_err_t e = read_at(h, header->reloc_offset, rel, rbytes);
        if (e != AG_OK) {
            ag_port_free(rel);
            return e;
        }
    }

    /*
     * The instruction relocations, all of them at once.
     *
     * Thirteen kilobytes for the desktop shell, against the whole image this
     * path exists to avoid holding - and they cannot be streamed alongside the
     * code, because the pages are programmed in order while the table is
     * sorted by nothing the pages agree with.
     */
    const uint32_t inum = ag_axe_ireloc_count(header);
    ag_axe_ireloc_t *irel = NULL;
    if (inum > 0) {
        const size_t ibytes = (size_t)inum * sizeof(ag_axe_ireloc_t);
        irel = (ag_axe_ireloc_t *)ag_port_alloc(ibytes,
                                                AG_MEM_FAST | AG_MEM_BYTE);
        if (irel == NULL) {
            ag_port_free(rel);
            return -AG_ENOMEM;
        }
        const ag_err_t e = read_at(h, ag_axe_ireloc_offset(header), irel,
                                   ibytes);
        if (e != AG_OK) {
            ag_port_free(irel);
            ag_port_free(rel);
            return e;
        }
    }

    uint8_t *buf = (uint8_t *)ag_port_alloc(AG_XIP_CHUNK, AG_MEM_FAST | AG_MEM_BYTE);
    if (buf == NULL) {
        ag_port_free(irel);
        ag_port_free(rel);
        return -AG_ENOMEM;
    }

    /* Data part into RAM, its bss zeroed (data_alloc does not clear). */
    ag_err_t err = AG_OK;
    uint32_t n = 0;
    if (out->place.data != NULL && header->data.file_size > 0) {
        err = read_at(h, header->data.offset, out->place.data,
                      header->data.file_size);
    }
    if (err == AG_OK && out->place.data != NULL &&
        header->data.size > header->data.file_size) {
        memset((uint8_t *)out->place.data + header->data.file_size, 0,
               header->data.size - header->data.file_size);
    }

    for (uint32_t off = 0; err == AG_OK && off < header->code.size;
         off += n) {
        n = header->code.size - off;
        if (n > AG_XIP_CHUNK) {
            n = AG_XIP_CHUNK;
        }

        /*
         * Never cut an instruction in half.
         *
         * A word relocation is four-byte aligned and a page is four-byte
         * aligned, so one can never straddle the boundary.  An instruction
         * can: compressed instructions make two-byte alignment legal, so a
         * four-byte instruction may begin two bytes before the end of a page.
         * Patching half of it here and half in the next page would need the
         * other half's original bytes, which are in the other page.
         *
         * So the page stops short instead.  Only the very last instruction of
         * a full page can be affected, so this costs four bytes of a page and
         * happens rarely; programming a shorter run is something appfs allows,
         * and the next page picks up where this one stopped.
         */
        if (n == AG_XIP_CHUNK) {
            const uint32_t edge = off + n - 2u;
            for (uint32_t i = 0; i < inum; i++) {
                if (AG_AXE_I_OFFSET(irel[i].site) == edge) {
                    n -= 4u;
                    break;
                }
            }
        }
        uint32_t fn = 0;
        if (off < header->code.file_size) {
            fn = header->code.file_size - off;
            if (fn > n) {
                fn = n;
            }
            err = read_at(h, header->code.offset + off, buf, fn);
            if (err != AG_OK) {
                break;
            }
        }
        if (fn < n) {
            memset(buf + fn, 0, n - fn);
        }
        if (api_in_code && api_at >= code_base + off &&
            api_at + 4u <= code_base + off + n) {
            memcpy(buf + (api_at - code_base - off), &api_val, 4);
        }
        for (uint32_t i = 0; i < header->reloc_count; i++) {
            const uint32_t entry = rel[i];
            if (entry & AG_AXE_R_IN_DATA) {
                continue; /* the word lives in the data part */
            }
            const uint32_t at = AG_AXE_R_OFFSET(entry);
            if (at < off || at + 4u > off + n) {
                continue;
            }
            uint32_t w;
            memcpy(&w, buf + (at - off), 4);
            w += (entry & AG_AXE_R_TO_DATA) ? data_bias : code_bias;
            memcpy(buf + (at - off), &w, 4);
        }
        for (uint32_t i = 0; i < inum; i++) {
            const uint32_t at = AG_AXE_I_OFFSET(irel[i].site);
            if (at < off || at + 4u > off + n) {
                continue;
            }
            if (!ag_axe_ireloc_apply(header, buf, header->code.file_size,
                                     code_addr, data_addr, off, irel[i])) {
                err = -AG_EFORMAT;
                break;
            }
        }
        err = ag_appfs_program_at((ag_appfs_slot_t *)out->xip_slot, off, buf, n);
    }
    ag_port_free(buf);
    ag_port_free(irel);

    if (err == AG_OK && out->place.data != NULL) {
        for (uint32_t i = 0; i < header->reloc_count; i++) {
            const uint32_t entry = rel[i];
            if (!(entry & AG_AXE_R_IN_DATA)) {
                continue;
            }
            const uint32_t at = AG_AXE_R_OFFSET(entry);
            if (at + 4u > header->data.size) {
                continue;
            }
            uint32_t w;
            memcpy(&w, (uint8_t *)out->place.data + at, 4);
            w += (entry & AG_AXE_R_TO_DATA) ? data_bias : code_bias;
            memcpy((uint8_t *)out->place.data + at, &w, 4);
        }
        if (!api_in_code && api_at >= data_base &&
            api_at + 4u <= data_base + header->data.size) {
            memcpy((uint8_t *)out->place.data + (api_at - data_base), &api_val,
                   4);
        }
    }
    ag_port_free(rel);

    if (err != AG_OK) {
        return err;
    }

    /* Entry point in flash; API already bound above, so the orchestrator's
     * bind (api_slot NULL) is a no-op and it must not program again. */
    out->binding.entry =
        (void *)(uintptr_t)(code_addr + (header->entry - code_base));
    out->binding.api_slot = NULL;
    out->binding.data_base = (uintptr_t)out->place.data;
    out->xip_programmed = true;
    return AG_OK;
}

static ag_err_t place_image(const ag_axe_header_t *header, ag_loaded_app_t *out)
{
    arena_ready();

    const bool contiguous = (header->flags & AG_AXE_CONTIGUOUS) != 0;
    /*
     * AG_AXE_WANT_XIP inverts the "arena when it fits" default: the application
     * would rather run from flash and leave the arena's internal SRAM free for
     * what cannot come from flash (a radio bring-up's ~36 KB contiguous block).
     * Honoured only for a non-contiguous image - flash cannot host a contiguous
     * image's data, so a contiguous one takes the arena regardless of the flag.
     */
    const bool want_xip =
        (header->flags & AG_AXE_WANT_XIP) != 0 && !contiguous;
    const size_t want =
        contiguous ? (size_t)header->code.size + header->data.size
                   : (size_t)header->code.size;

    /*
     * [memory] app.code (argon/place.h): iram, psram, flash by default.  iram
     * is the link-time arena, then the internal heap for as long as the
     * program runs; psram the PSRAM arena where this build has one; flash is
     * XIP from the appfs partition.  An image that asked for flash XIP goes
     * straight there, as it always did.
     */
    ag_place_kind_t kinds[AG_PLACE_KINDS];
    const unsigned  n = want_xip ? 0u : ag_place_order("app.code", kinds);
    bool            flash_allowed = want_xip;
    for (unsigned k = 0; k < n && !flash_allowed; k++) {
        switch (kinds[k]) {
        case AG_PLACE_IRAM:
            if (ag_arena_largest_free(&s_code, 16) >= want) {
                /* place_arena says which of its two allocations failed; there
                 * is no one sentence that covers both, and guessing produced
                 * the wrong one. */
                const ag_err_t aerr = place_arena(header, out);
                if (aerr == AG_OK) {
                    ag_place_note("app.code", AG_PLACE_IRAM,
                                  (int32_t)out->place.code_capacity);
                }
                return aerr;
            }
            /* Internal SRAM the program can have for as long as it runs. */
            if (heap_code_room(want) && place_heap(header, out) == AG_OK) {
                ag_place_note("app.code", AG_PLACE_IRAM,
                              (int32_t)out->place.code_capacity);
                return AG_OK;
            }
            break;
        case AG_PLACE_PSRAM:
#if defined(CONFIG_ARGON_APP_ARENA_PSRAM)
            /*
             * S-1: the PSRAM arena, mapped into the instruction window.
             * Slower than internal SRAM, far faster than flash for a working
             * set that does not fit the cache.
             */
            /* Mapped only for code that can go into it: the arena maps
             * once for the life of the system, 64 KB-aligned, and on
             * 8 Oct 2026 the hole under it kept Fallout's 6000 KB arena
             * from starting although its 788 KB of code never goes there
             * (the heap rounds a request that large up to 6016). */
            if (!contiguous &&
                (size_t)header->code.size <= AG_PSRAM_ARENA_BYTES &&
                psram_ready() &&
                ag_arena_largest_free(&s_psram, 16) >=
                    (size_t)header->code.size &&
                place_psram(header, out) == AG_OK) {
                ag_place_note("app.code", AG_PLACE_PSRAM,
                              (int32_t)out->place.code_capacity);
                return AG_OK;
            }
#endif
            break;
        case AG_PLACE_FLASH:
            flash_allowed = true;
            break;
        default:
            break;
        }
    }
    if (!flash_allowed) {
        char wanted[32];
        ag_place_format(ag_place_policy("app.code"), wanted, sizeof(wanted));
        ag_log(AG_LOG_ERROR, "loader",
               "%u bytes of code: app.code = %s, and none of them has room",
               (unsigned)want, wanted);
        return -AG_ENOMEM;
    }

    if (want_xip && ag_arena_largest_free(&s_code, 16) >= want) {
        ag_log(AG_LOG_INFO, "loader",
               "%u bytes of code would fit the arena (%u free) but the image "
               "asked for flash XIP; honouring it",
               (unsigned)header->code.size,
               (unsigned)ag_arena_largest_free(&s_code, 16));
    }

    if (contiguous) {
        ag_log(AG_LOG_ERROR, "loader",
               "%u bytes of contiguous image will not fit the arena (%u free); "
               "flash XIP cannot host contiguous data",
               (unsigned)want, (unsigned)ag_arena_largest_free(&s_code, 16));
        return -AG_ENOMEM;
    }

    /*
     * Both budgets, because flash XIP moves the problem from one to the other:
     * the code no longer needs the arena, and instead needs a scratch buffer of
     * its own size in ordinary memory, alongside the image's data.  On a machine
     * where those two together are most of the heap, the number that matters is
     * this one and it is worth printing before the attempt rather than after.
     */
    if (!want_xip) {
        ag_log(AG_LOG_INFO, "loader",
               "%u bytes of code exceed the arena (%u free); flash XIP needs "
               "%u data, heap has %u free, largest %u",
               (unsigned)header->code.size,
               (unsigned)ag_arena_largest_free(&s_code, 16),
               (unsigned)header->data.size,
               (unsigned)ag_port_mem_free(AG_MEM_FAST | AG_MEM_BYTE),
               (unsigned)ag_port_mem_largest(AG_MEM_FAST | AG_MEM_BYTE));
    }

    const ag_err_t err = place_xip(header, out);
    if (err == AG_OK) {
        ag_place_note("app.code", AG_PLACE_FLASH, (int32_t)header->code.size);
    }
    if (err != AG_OK) {
        /*
         * The heap, not the arena.  Flash XIP has already given up on the arena
         * by the time it is tried, and what it needs instead is ordinary memory:
         * a scratch buffer the size of the code, to relocate in before writing
         * to flash, plus the image's data.  Reporting the arena here sent me
         * looking at the wrong number entirely.
         */
        ag_log(AG_LOG_ERROR, "loader",
               "flash XIP placement failed (%d); needed %u scratch + %u data, "
               "heap has %u free, largest %u",
               (int)err, (unsigned)header->code.size,
               (unsigned)header->data.size,
               (unsigned)ag_port_mem_free(AG_MEM_FAST | AG_MEM_BYTE),
               (unsigned)ag_port_mem_largest(AG_MEM_FAST | AG_MEM_BYTE));
    }
    return err;
}

/*
 * Reads exactly `bytes` from `at` in the file.
 *
 * Short reads are an error rather than something to retry around: the sizes come
 * from the image's own header, so a read that stops early means the file does
 * not match what it says about itself.
 */
static ag_err_t read_at(ag_handle_t h, uint64_t at, void *dst, size_t bytes)
{
    if (ag_vfs_seek(h, (int64_t)at, AG_SEEK_SET) < 0) {
        return -AG_EFORMAT;
    }
    uint8_t *p = (uint8_t *)dst;
    size_t   got = 0;
    while (got < bytes) {
        const int32_t n = ag_vfs_read(h, p + got, bytes - got);
        if (n <= 0) {
            return (n < 0) ? (ag_err_t)n : -AG_EFORMAT;
        }
        got += (size_t)n;
    }
    return AG_OK;
}

/*
 * The code part, from the file into its place, a chunk at a time.
 *
 * Through copy_image rather than straight into the destination because the
 * destination may be the IRAM arena, which accepts only aligned 32-bit stores -
 * see copy_image.  Chunks are a multiple of four so that only the very last one
 * can have a tail, which is the one case copy_image handles by reading a word,
 * changing part of it and writing it back.
 */
static ag_err_t stream_code(ag_handle_t h, const ag_axe_part_t *part, void *dst)
{
    uint8_t  buf[512];
    uint8_t *out = (uint8_t *)dst;
    size_t   done = 0;

    if (ag_vfs_seek(h, (int64_t)part->offset, AG_SEEK_SET) < 0) {
        return -AG_EFORMAT;
    }
    while (done < part->file_size) {
        size_t want = part->file_size - done;
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }
        size_t got = 0;
        while (got < want) {
            const int32_t n = ag_vfs_read(h, buf + got, want - got);
            if (n <= 0) {
                return (n < 0) ? (ag_err_t)n : -AG_EFORMAT;
            }
            got += (size_t)n;
        }
        copy_image(out + done, buf, want);
        done += want;
    }
    return AG_OK;
}

/*
 * Loads an image without ever holding the whole file.
 *
 * The obvious way - read the file, then place it, then copy out of it - keeps
 * three copies of the image alive at the same time: the file, the code (or, for
 * flash execution, a scratch buffer the size of the code) and the data.  On a
 * machine with room that costs nothing worth naming.  On a board with sixty
 * kilobytes of byte-addressable memory free it is the difference between loading
 * and not: a twenty-five kilobyte application needed seventy-five, and every
 * number in the failure was about the wrong thing.
 *
 * Read in the order the parts sit in the file and nothing is held twice.  What
 * still has to be held whole is the relocation table, because relocations are
 * applied after both parts are in place and in no particular order - but that is
 * four bytes each, under two kilobytes for the largest image here.
 *
 * A signed image takes the older path: verifying a signature means hashing every
 * byte, and hashing what has already been scattered into two places is a
 * different piece of work.  Signing is optional and rare; running out of memory
 * is neither.
 */
static ag_err_t load_streamed(ag_handle_t h, const ag_axe_header_t *header,
                              ag_loaded_app_t *out)
{
    out->header = *header;

    ag_err_t err = place_image(header, out);
    if (err != AG_OK) {
        memset(out, 0, sizeof(*out));
        return err;
    }

    /* Flash XIP loads a page at a time (small scratch); arena loads in place. */
    if (out->code_from_xip) {
        err = xip_load_chunked(h, header, out);
        if (err != AG_OK) {
            release_image(out);
            memset(out, 0, sizeof(*out));
        }
        return err;
    }

    void *code_dst = (out->place.code_writable != NULL)
                         ? out->place.code_writable
                         : out->place.code;
    err = stream_code(h, &header->code, code_dst);
    if (err == AG_OK && header->data.file_size > 0) {
        err = read_at(h, header->data.offset, out->place.data,
                      header->data.file_size);
    }

    uint32_t *rel = NULL;
    if (err == AG_OK && header->reloc_count > 0) {
        const size_t bytes = (size_t)header->reloc_count * sizeof(uint32_t);
        rel = (uint32_t *)ag_port_alloc(bytes, AG_MEM_FAST | AG_MEM_BYTE);
        if (rel == NULL) {
            err = -AG_ENOMEM;
        } else {
            err = read_at(h, header->reloc_offset, rel, bytes);
        }
    }

    const uint32_t inum = ag_axe_ireloc_count(header);
    ag_axe_ireloc_t *irel = NULL;
    if (err == AG_OK && inum > 0) {
        const size_t ibytes = (size_t)inum * sizeof(ag_axe_ireloc_t);
        irel = (ag_axe_ireloc_t *)ag_port_alloc(ibytes,
                                                AG_MEM_FAST | AG_MEM_BYTE);
        if (irel == NULL) {
            err = -AG_ENOMEM;
        } else {
            err = read_at(h, ag_axe_ireloc_offset(header), irel, ibytes);
        }
    }

    if (err == AG_OK) {
        err = ag_axe_apply(&out->header, &out->place, rel,
                           header->reloc_count, irel, inum, &out->binding);
    }
    ag_port_free(irel);
    ag_port_free(rel);

#if defined(CONFIG_ARGON_APP_ARENA_PSRAM)
    /*
     * The code was written and relocated through the data window; make it
     * visible to the instruction fetch before anything jumps into it - push the
     * data cache to PSRAM, drop the stale instruction cache.  Only for a
     * PSRAM-placed image: the internal arena is already coherent, and flash XIP
     * has its own path.
     */
    if (err == AG_OK && out->code_from_psram) {
        ag_port_execmem_sync(out->place.code_writable, out->place.code_capacity);
    }
#endif

    if (err != AG_OK) {
        release_image(out);
        memset(out, 0, sizeof(*out));
    }
    return err;
}

/* The older path, for an image whose signature has to be checked. */
static ag_err_t load_whole(const char *path, const char *cwd,
                           const ag_axe_header_t *header, ag_loaded_app_t *out)
{
    uint8_t *file = NULL;
    size_t   file_size = 0;
    ag_err_t err = read_whole(path, cwd, &file, &file_size);
    if (err != AG_OK) {
        return err;
    }

    err = ag_axe_check_sig(file, file_size);
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, "loader", "%s: bad signature (%d)", path,
               (int)err);
        ag_port_free(file);
        return err;
    }

    out->header = *header;
    err = place_image(header, out);
    if (err != AG_OK) {
        ag_port_free(file);
        memset(out, 0, sizeof(*out));
        return err;
    }

    /*
     * The signed path relocates the whole image in memory (it has just held the
     * whole file to hash it), so for flash XIP it needs a code-size scratch that
     * place_xip no longer allocates - the chunked streamed path is what avoids
     * that, and it cannot verify a signature.  Allocate the scratch here.
     */
    if (out->code_from_xip) {
        void *scratch = scratch_alloc(header->code.size);
        if (scratch == NULL) {
            ag_port_free(file);
            release_image(out);
            memset(out, 0, sizeof(*out));
            return -AG_ENOMEM;
        }
        out->place.code_writable = scratch;
        out->code_scratch = scratch;
    }

    void *code_dst = (out->place.code_writable != NULL)
                         ? out->place.code_writable
                         : out->place.code;
    copy_image(code_dst, file + header->code.offset, header->code.file_size);
    if (header->data.file_size > 0) {
        memcpy(out->place.data, file + header->data.offset,
               header->data.file_size);
    }

    err = ag_axe_apply(&out->header, &out->place,
                       (const uint32_t *)(file + header->reloc_offset),
                       header->reloc_count,
                       (const ag_axe_ireloc_t *)(void *)
                           (file + ag_axe_ireloc_offset(header)),
                       ag_axe_ireloc_count(header), &out->binding);
    ag_port_free(file);

    if (err != AG_OK) {
        release_image(out);
        memset(out, 0, sizeof(*out));
    }
    return err;
}

/*
 * The header alone, without loading anything.
 *
 * The stack an image runs on has to be decided before the task that loads it
 * exists, which is a chicken and egg only if the header cannot be read
 * separately.  It can: it is the first hundred and something bytes of the file.
 *
 * Until this existed the task was created with the kernel default and the
 * header's request was merely *warned* about after the fact - so an image asking
 * for more stack quietly ran with less, and one asking for less quietly paid for
 * more.  On a board where eight kilobytes is the difference between an
 * application starting and not, the second half of that mattered too.
 */
ag_err_t ag_loader_peek(const char *path, const char *cwd,
                        ag_axe_header_t *out)
{
    if (path == NULL || out == NULL) {
        return -AG_EINVAL;
    }

    const ag_handle_t h = ag_vfs_open(path, cwd, AG_O_RDONLY);
    if (h < 0) {
        return h;
    }

    const int64_t size = ag_vfs_seek(h, 0, AG_SEEK_END);
    if (size < (int64_t)sizeof(*out) || size > (int64_t)AG_LOADER_MAX_FILE) {
        ag_vfs_close(h);
        return -AG_EFORMAT;
    }

    const ag_err_t err = read_at(h, 0, out, sizeof(*out));
    ag_vfs_close(h);
    if (err != AG_OK) {
        return err;
    }
    return ag_axe_validate(out, (size_t)size, ag_axe_native_arch(),
                           AG_ABI_MAJOR, AG_ABI_MINOR);
}

/*
 * Why an image would not load, in words, once.
 *
 * It lived in the shell, which meant the shell's `run` explained itself and
 * everything else printed a number.  On a board that mattered: an image built
 * for the other instruction set is refused with -AG_ENOTSUP, and what reached
 * the screen was "nodehirv.axe (pid 2) returned -45".  Correct, useless, and
 * indistinguishable from a program that chose to return -45.
 *
 * Here rather than in the shell because these are the loader's own answers,
 * and a second copy of the table is a second copy to drift.
 */
/*
 * MY OWN BUILD OF THAT PROGRAM, found by what it IS rather than by what it is
 * called.
 *
 * A cluster spanning two instruction sets passes messages and shares devices
 * without noticing the line; code stops at it dead.  The answer is not to
 * carry one binary that runs on both - there is no such thing for Xtensa and
 * RISC-V - but to stop sending code at all: send the program's IDENTITY and
 * let each board use the copy it can execute.
 *
 * The identity is already in every image: name, version, author and the
 * instruction set, in the header, and until now nothing read them for this.
 *
 * Searched rather than indexed because the set is small - a board's C: holds
 * tens of images, not thousands - and because an index is a second thing to
 * keep true.  If that ever stops being cheap, the place to put the index is
 * here, behind this function, and nothing above it changes.
 */
ag_err_t ag_loader_find_by_name(const char *want, char *out, size_t cap)
{
    if (want == NULL || want[0] == '\0' || out == NULL || cap == 0) {
        return -AG_EINVAL;
    }

    const ag_handle_t d = ag_vfs_opendir("/sys", "/sys");
    if (d < 0) {
        return -AG_ENOENT;
    }

    ag_err_t found = -AG_ENOENT;
    /*
     * A bytecode build of the same program, kept aside in case there is no
     * native one.  That is the cascade's third rung and it costs this much:
     * the second-best answer is remembered while the best one is looked for,
     * and only used if the search ends without it.
     *
     * Second-best on purpose.  Bytecode runs five to fifty times slower, so a
     * board that has its own build must never be given the portable one -
     * which is exactly what would happen if this took the first match.
     */
    char     bc[AG_PATH_MAX];
    bool     have_bc = false;

    for (;;) {
        ag_dirent_t e;
        if (ag_vfs_readdir(d, &e) != AG_OK) {
            break;
        }
        if ((e.st.attr & (uint32_t)AG_A_DIR) != 0u) {
            continue;
        }

        char path[AG_PATH_MAX];
        if (snprintf(path, sizeof(path), "/sys/%s", e.name) >= (int)sizeof(path)) {
            continue;
        }

        ag_axe_header_t h;
        const ag_err_t  pk = ag_loader_peek(path, "/sys", &h);
        /*
         * -AG_ENOTSUP is "read and understood, not for this machine", which is
         * what a bytecode image always answers - ag_axe_native_arch() is never
         * AG_ARCH_BC.  Anything else that cannot be read is not an image.
         */
        if (pk != AG_OK && pk != -AG_ENOTSUP) {
            continue;
        }
        if (ag_path_icmp(h.name, want) != 0) {
            continue;
        }

        if ((ag_axe_arch_t)h.arch == ag_axe_native_arch()) {
            ag_strlcpy(out, path, cap);
            found = AG_OK;
            break;
        }
        if ((ag_axe_arch_t)h.arch == AG_ARCH_BC && !have_bc) {
            ag_strlcpy(bc, path, sizeof(bc));
            have_bc = true;
        }
        /*
         * And anything else is an image of the OTHER instruction set, which is
         * exactly what sent us looking: taking one would be a loop.
         */
    }
    ag_vfs_closedir(d);

    if (found != AG_OK && have_bc) {
        ag_strlcpy(out, bc, cap);
        found = AG_OK;
    }
    return found;
}

const char *ag_loader_why(ag_err_t err)
{
    switch (-err) {
    case AG_ENOENT:  return "file not found";
    case AG_EFORMAT: return "not an ArgonOS application";
    case AG_ENOTSUP: return "built for a different processor";
    case AG_EABI:    return "built for a different version of this system";
    case AG_ENOMEM:  return "not enough memory (task stack / heap / code arena)";
    case AG_EBUSY:   return "too many applications are loaded";
    case AG_ERANGE:  return "too many arguments";
    case AG_EINVAL:  return "is a driver; use 'drv load'";
    case AG_EEXIST:  return "already loaded";
    case AG_ENFILE:  return "too many modules are loaded";
    default:         return "could not be loaded";
    }
}

ag_err_t ag_loader_load(const char *path, const char *cwd,
                        ag_loaded_app_t *out)
{
    if (path == NULL || out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));

    const ag_handle_t h = ag_vfs_open(path, cwd, AG_O_RDONLY);
    if (h < 0) {
        return h;
    }

    const int64_t size = ag_vfs_seek(h, 0, AG_SEEK_END);
    if (size < (int64_t)sizeof(ag_axe_header_t) ||
        size > (int64_t)AG_LOADER_MAX_FILE) {
        ag_vfs_close(h);
        return -AG_EFORMAT;
    }

    ag_axe_header_t header;
    ag_err_t err = read_at(h, 0, &header, sizeof(header));
    if (err != AG_OK) {
        ag_vfs_close(h);
        return err;
    }

    err = ag_axe_validate(&header, (size_t)size, ag_axe_native_arch(),
                          AG_ABI_MAJOR, AG_ABI_MINOR);
    if (err != AG_OK) {
        ag_vfs_close(h);
        return err;
    }

    if (ag_axe_is_signed(&header)) {
        ag_vfs_close(h);
        err = load_whole(path, cwd, &header, out);
    } else {
        err = load_streamed(h, &header, out);
        ag_vfs_close(h);
    }
    if (err != AG_OK) {
        return err;
    }

    const ag_axe_header_t *const hdr = &out->header;
    (void)hdr;

    ag_axe_bind_api(&out->binding, ag_loader_api());

    if (out->code_from_xip) {
        /* The chunked streamed path has already relocated and programmed the
         * slot; the signed whole-image path has not, so program it now. */
        if (!out->xip_programmed) {
            err = ag_appfs_program((ag_appfs_slot_t *)out->xip_slot,
                                   out->code_scratch, out->header.code.size);
            if (err != AG_OK) {
                release_image(out);
                memset(out, 0, sizeof(*out));
                return err;
            }
        }

        const void *mapped = NULL;
        err = ag_appfs_mmap((ag_appfs_slot_t *)out->xip_slot, &mapped);
        if (err != AG_OK) {
            release_image(out);
            memset(out, 0, sizeof(*out));
            return err;
        }

        if (mapped != out->place.code) {
            ag_log(AG_LOG_ERROR, "loader",
                   "XIP address moved: predicted %p, mapped %p",
                   out->place.code, mapped);
            release_image(out);
            memset(out, 0, sizeof(*out));
            return -AG_EIO;
        }

        /* Scratch is no longer needed; execution uses the mmap. */
        ag_port_free(out->code_scratch);
        out->code_scratch = NULL;
        out->place.code_writable = NULL;

        /* Entry was computed against the predicted address; still valid. */
        ag_log(AG_LOG_INFO, "loader",
               "%s: %s v%s, code %u B XIP at %p, data %u B at %p, %u relocations",
               out->header.name, ag_axe_arch_name((ag_axe_arch_t)out->header.arch),
               out->header.version, (unsigned)out->header.code.size, out->place.code,
               (unsigned)out->header.data.size, out->place.data,
               (unsigned)out->binding.relocated);
    } else {
        ag_log(AG_LOG_INFO, "loader",
               "%s: %s v%s, code %u B at %p, data %u B at %p, %u relocations",
               out->header.name, ag_axe_arch_name((ag_axe_arch_t)out->header.arch),
               out->header.version, (unsigned)out->header.code.size, out->place.code,
               (unsigned)out->header.data.size, out->place.data,
               (unsigned)out->binding.relocated);
    }

    return AG_OK;
}

void ag_loader_unload(ag_loaded_app_t *app)
{
    if (app != NULL &&
        (app->place.code != NULL || app->xip_slot != NULL)) {
        release_image(app);
        memset(app, 0, sizeof(*app));
    }
}
