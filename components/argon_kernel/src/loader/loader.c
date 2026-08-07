/*
 * ArgonOS - placing an application image in memory.
 *
 * Reading the file, finding room for its two parts and relocating them.  What
 * happens after that - the task it runs on, the memory it is allowed to ask for,
 * and giving all of it back afterwards - belongs to the process layer in
 * src/proc/, so that nothing here has to know what a process is.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/loader.h>

#include <string.h>

#include <argon/arena.h>
#include <argon/log.h>
#include <argon/module.h>
#include <argon/proc.h>
#include <argon/vfs.h>

#include "esp_heap_caps.h"
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

    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)size,
                                               MALLOC_CAP_SPIRAM |
                                                   MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = (uint8_t *)heap_caps_malloc((size_t)size,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        ag_vfs_close(h);
        return -AG_ENOMEM;
    }

    size_t got = 0;
    while (got < (size_t)size) {
        const int32_t n = ag_vfs_read(h, buf + got, (size_t)size - got);
        if (n <= 0) {
            heap_caps_free(buf);
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

static uint8_t s_arena[AG_APP_ARENA_BYTES]
    __attribute__((aligned(16), section(".iram.bss.ag_app_arena")));

#define AG_LOADER_SLOTS (AG_PROC_MAX + AG_MODULE_MAX)

static ag_arena_block_t s_arena_blocks[AG_LOADER_SLOTS];
static ag_arena_t       s_code;

static void arena_ready(void)
{
    if (s_code.base == NULL) {
        ag_arena_init(&s_code, s_arena, sizeof(s_arena), s_arena_blocks,
                      AG_LOADER_SLOTS);
    }
}

size_t ag_loader_arena_size(void) { return sizeof(s_arena); }

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

static void copy_image(void *dst, const void *src, size_t bytes)
{
    if ((((uintptr_t)dst | (uintptr_t)src | (uintptr_t)bytes) & 3u) == 0) {
        uint32_t       *d = (uint32_t *)dst;
        const uint32_t *s = (const uint32_t *)src;
        for (size_t i = 0; i < bytes / 4u; i++) {
            d[i] = s[i];
        }
        return;
    }
    memcpy(dst, src, bytes);
}

static void *data_alloc(size_t bytes)
{
    void *p = heap_caps_aligned_alloc(16, bytes,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_aligned_alloc(16, bytes,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

static void *scratch_alloc(size_t bytes)
{
    void *p = heap_caps_aligned_alloc(16, bytes,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_aligned_alloc(16, bytes,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

static void release_image(ag_loaded_app_t *app)
{
    if (app->data_owned != NULL) {
        heap_caps_free(app->data_owned);
        app->data_owned = NULL;
    }
    if (app->code_scratch != NULL) {
        heap_caps_free(app->code_scratch);
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

    void *scratch = scratch_alloc(header->code.size);
    if (scratch == NULL) {
        ag_appfs_release(slot);
        return -AG_ENOMEM;
    }

    void *data = NULL;
    if (header->data.size > 0) {
        data = data_alloc(header->data.size);
        if (data == NULL) {
            heap_caps_free(scratch);
            ag_appfs_release(slot);
            return -AG_ENOMEM;
        }
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
        const ag_err_t err = place_arena(header, out);
        if (err == -AG_ENOMEM) {
            ag_log(AG_LOG_ERROR, "loader",
                   "%u bytes will not fit the arena: %u of %u bytes free, "
                   "largest block %u",
                   (unsigned)want, (unsigned)ag_arena_free_bytes(&s_code),
                   (unsigned)sizeof(s_arena),
                   (unsigned)ag_arena_largest_free(&s_code, 16));
        }
        return err;
    }

    if (contiguous) {
        ag_log(AG_LOG_ERROR, "loader",
               "%u bytes of contiguous image will not fit the arena (%u free); "
               "flash XIP cannot host contiguous data",
               (unsigned)want, (unsigned)ag_arena_largest_free(&s_code, 16));
        return -AG_ENOMEM;
    }

    ag_log(AG_LOG_INFO, "loader",
           "%u bytes of code exceed the arena (%u free); using flash XIP",
           (unsigned)header->code.size,
           (unsigned)ag_arena_largest_free(&s_code, 16));

    const ag_err_t err = place_xip(header, out);
    if (err != AG_OK) {
        ag_log(AG_LOG_ERROR, "loader",
               "flash XIP placement failed (%d); arena free %u, largest %u",
               (int)err, (unsigned)ag_arena_free_bytes(&s_code),
               (unsigned)ag_arena_largest_free(&s_code, 16));
    }
    return err;
}

ag_err_t ag_loader_load(const char *path, const char *cwd,
                        ag_loaded_app_t *out)
{
    if (path == NULL || out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));

    uint8_t *file = NULL;
    size_t   file_size = 0;
    ag_err_t err = read_whole(path, cwd, &file, &file_size);
    if (err != AG_OK) {
        return err;
    }

    const ag_axe_header_t *header = (const ag_axe_header_t *)file;
    err = ag_axe_validate(header, file_size, ag_axe_native_arch(),
                          AG_ABI_MAJOR, AG_ABI_MINOR);
    if (err != AG_OK) {
        heap_caps_free(file);
        return err;
    }

    out->header = *header;
    err = place_image(header, out);
    if (err != AG_OK) {
        heap_caps_free(file);
        memset(out, 0, sizeof(*out));
        return err;
    }

    /* Copy into the writable view of each part. */
    void *code_dst = (out->place.code_writable != NULL) ? out->place.code_writable
                                                         : out->place.code;
    copy_image(code_dst, file + header->code.offset, header->code.file_size);
    if (header->data.file_size > 0) {
        memcpy(out->place.data, file + header->data.offset,
               header->data.file_size);
    }

    err = ag_axe_apply(&out->header, &out->place,
                       (const uint32_t *)(file + header->reloc_offset),
                       header->reloc_count, &out->binding);
    heap_caps_free(file);

    if (err != AG_OK) {
        release_image(out);
        memset(out, 0, sizeof(*out));
        return err;
    }

    ag_axe_bind_api(&out->binding, ag_loader_api());

    if (out->code_from_xip) {
        err = ag_appfs_program((ag_appfs_slot_t *)out->xip_slot,
                               out->code_scratch, header->code.size);
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
        heap_caps_free(out->code_scratch);
        out->code_scratch = NULL;
        out->place.code_writable = NULL;

        /* Entry was computed against the predicted address; still valid. */
        ag_log(AG_LOG_INFO, "loader",
               "%s: %s v%s, code %u B XIP at %p, data %u B at %p, %u relocations",
               header->name, ag_axe_arch_name((ag_axe_arch_t)header->arch),
               header->version, (unsigned)header->code.size, out->place.code,
               (unsigned)header->data.size, out->place.data,
               (unsigned)out->binding.relocated);
    } else {
        ag_log(AG_LOG_INFO, "loader",
               "%s: %s v%s, code %u B at %p, data %u B at %p, %u relocations",
               header->name, ag_axe_arch_name((ag_axe_arch_t)header->arch),
               header->version, (unsigned)header->code.size, out->place.code,
               (unsigned)header->data.size, out->place.data,
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
