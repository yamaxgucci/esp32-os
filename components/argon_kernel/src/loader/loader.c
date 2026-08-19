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
#include <argon/vfs.h>

#include <argon/port/mem.h>
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

static void *data_alloc(size_t bytes)
{
    void *p = ag_port_alloc_aligned(16, bytes,
                                      AG_MEM_SLOW | AG_MEM_BYTE);
    if (p == NULL) {
        p = ag_port_alloc_aligned(16, bytes,
                                    AG_MEM_FAST | AG_MEM_BYTE);
    }
    return p;
}

static void *scratch_alloc(size_t bytes)
{
    void *p = ag_port_alloc_aligned(16, bytes,
                                      AG_MEM_SLOW | AG_MEM_BYTE);
    if (p == NULL) {
        p = ag_port_alloc_aligned(16, bytes,
                                    AG_MEM_FAST | AG_MEM_BYTE);
    }
    return p;
}

static void release_image(ag_loaded_app_t *app)
{
    if (app->data_owned != NULL) {
        ag_port_free(app->data_owned);
        app->data_owned = NULL;
    }
    if (app->code_scratch != NULL) {
        ag_port_free(app->code_scratch);
        app->code_scratch = NULL;
    }
    if (app->xip_slot != NULL) {
        ag_appfs_release((ag_appfs_slot_t *)app->xip_slot);
        app->xip_slot = NULL;
    }
    if (app->place.code != NULL && !app->code_from_xip) {
        if (!ag_arena_free(&s_code, app->place.code)) {
            ag_log(AG_LOG_ERROR, "loader", "arena does not own %p",
                   app->place.code);
        }
    }
    memset(&app->place, 0, sizeof(app->place));
    app->code_from_xip = false;
}

/*
 * IRAM arena path: code is writable and executable in place.
 * Contiguous images always take this path (data must sit next to code).
 */
static ag_err_t place_arena(const ag_axe_header_t *header, ag_loaded_app_t *out)
{
    arena_ready();

    const bool contiguous = (header->flags & AG_AXE_CONTIGUOUS) != 0;
    const size_t code_bytes =
        contiguous ? (size_t)header->code.size + header->data.size
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
        (void)ag_arena_free(&s_code, code);
        memset(&out->place, 0, sizeof(out->place));
        return -AG_ENOMEM;
    }

    out->place.data = data;
    out->place.data_capacity = header->data.size;
    out->data_owned = data;
    return AG_OK;
}

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

    void *scratch = scratch_alloc(header->code.size);
    if (scratch == NULL) {
        ag_port_free(data);
        ag_appfs_release(slot);
        return -AG_ENOMEM;
    }

    out->place.code = exec;
    out->place.code_capacity = header->code.size;
    out->place.code_writable = scratch;
    out->place.data = data;
    out->place.data_capacity = header->data.size;
    out->data_owned = data;
    out->code_scratch = scratch;
    out->xip_slot = slot;
    out->code_from_xip = true;
    return AG_OK;
}

static ag_err_t place_image(const ag_axe_header_t *header, ag_loaded_app_t *out)
{
    arena_ready();

    const bool contiguous = (header->flags & AG_AXE_CONTIGUOUS) != 0;
    const size_t want =
        contiguous ? (size_t)header->code.size + header->data.size
                   : (size_t)header->code.size;

    if (ag_arena_largest_free(&s_code, 16) >= want) {
        /* place_arena says which of its two allocations failed; there is no
         * one sentence that covers both, and guessing produced the wrong one. */
        return place_arena(header, out);
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
    ag_log(AG_LOG_INFO, "loader",
           "%u bytes of code exceed the arena (%u free); flash XIP needs "
           "%u scratch + %u data, heap has %u free, largest %u",
           (unsigned)header->code.size,
           (unsigned)ag_arena_largest_free(&s_code, 16),
           (unsigned)header->code.size, (unsigned)header->data.size,
           (unsigned)ag_port_mem_free(AG_MEM_FAST | AG_MEM_BYTE),
           (unsigned)ag_port_mem_largest(AG_MEM_FAST | AG_MEM_BYTE));

    const ag_err_t err = place_xip(header, out);
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

    if (err == AG_OK) {
        err = ag_axe_apply(&out->header, &out->place, rel,
                           header->reloc_count, &out->binding);
    }
    ag_port_free(rel);

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
                       header->reloc_count, &out->binding);
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
        err = ag_appfs_program((ag_appfs_slot_t *)out->xip_slot,
                               out->code_scratch, out->header.code.size);
        if (err != AG_OK) {
            release_image(out);
            memset(out, 0, sizeof(*out));
            return err;
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
