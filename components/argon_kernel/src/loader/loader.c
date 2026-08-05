/*
 * ArgonOS - loading and running an application.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/loader.h>

#include <setjmp.h>
#include <string.h>

#include <argon/log.h>
#include <argon/vfs.h>

#include "esp_heap_caps.h"

/*
 * A .AXE is read whole before anything is placed, so that a truncated file is
 * refused rather than half-loaded.
 */
#define AG_LOADER_MAX_FILE (1024u * 1024u)

/*
 * Where ag_exit() lands.  One application at a time for now, so one buffer is
 * enough; the process model replaces this with per-process state.
 */
static jmp_buf s_exit_jump;
static bool    s_running;
static int     s_exit_code;

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
 * The application arena: memory an image can be both executed from and written
 * to.
 *
 * On this family the instruction and data buses reach memory through different
 * address windows, and most memory is one or the other.  Flash and PSRAM in the
 * instruction window can be read and executed but not written, so bss and data
 * cannot live there.  The D/IRAM region is the only memory that is executable
 * and writable through one address, which is what a single contiguous image with
 * one relocation bias requires.
 *
 * ESP-IDF hands none of that region to the heap on this chip - MALLOC_CAP_EXEC
 * and MALLOC_CAP_IRAM_8BIT both report zero bytes available - so the arena is
 * reserved at link time instead.  It costs its full size whether an application
 * is running or not, which is why it is small and configurable.
 *
 * Larger applications need the image split in two, code in the instruction
 * window and data in the data window, with a relocation table that says which
 * of the two each word refers to.  That is the next step; see
 * docs/04-roadmap.md.
 */
#ifndef AG_APP_ARENA_BYTES
#define AG_APP_ARENA_BYTES (16u * 1024u)
#endif

static uint8_t s_arena[AG_APP_ARENA_BYTES]
    __attribute__((aligned(16), section(".iram1.ag_app_arena")));
static bool s_arena_taken;

static void *alloc_image(size_t bytes)
{
    if (s_arena_taken || bytes > sizeof(s_arena)) {
        return NULL;
    }
    s_arena_taken = true;
    return s_arena;
}

static void free_image(void *image)
{
    if (image == s_arena) {
        s_arena_taken = false;
    }
}

size_t ag_loader_arena_size(void) { return sizeof(s_arena); }
bool   ag_loader_arena_busy(void) { return s_arena_taken; }

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

    void *image = alloc_image(header->image_size);
    if (image == NULL) {
        ag_log(AG_LOG_ERROR, "loader",
               "image of %u bytes will not fit the %u byte application arena%s",
               (unsigned)header->image_size, (unsigned)sizeof(s_arena),
               s_arena_taken ? " (already in use)" : "");
        heap_caps_free(file);
        return -AG_ENOMEM;
    }

    memcpy(image, file + header->image_offset, header->file_size);

    out->header = *header;
    out->image = image;
    out->image_size = header->image_size;

    err = ag_axe_apply(image, header->image_size, &out->header,
                       (const uint32_t *)(file + header->reloc_offset),
                       header->reloc_count, &out->binding);
    heap_caps_free(file);

    if (err != AG_OK) {
        free_image(image);
        memset(out, 0, sizeof(*out));
        return err;
    }

    ag_axe_bind_api(&out->binding, ag_loader_api());

    /*
     * No cache maintenance here on purpose: internal RAM sits in front of the
     * cache, reachable from both the data and the instruction bus, so what was
     * written is what will be executed.  Images placed in PSRAM will need a
     * writeback and an instruction-cache invalidate before the jump - that
     * belongs with the PSRAM mapping, which needs a board to verify.
     */
    ag_log(AG_LOG_INFO, "loader", "%s: %s v%s, %u bytes at %p, %u relocations",
           header->name, ag_axe_arch_name((ag_axe_arch_t)header->arch),
           header->version, (unsigned)header->image_size, image,
           (unsigned)out->binding.relocated);
    return AG_OK;
}

void ag_loader_unload(ag_loaded_app_t *app)
{
    if (app != NULL && app->image != NULL) {
        free_image(app->image);
        memset(app, 0, sizeof(*app));
    }
}

void ag_loader_exit(int code)
{
    if (s_running) {
        s_exit_code = code;
        longjmp(s_exit_jump, 1);
    }
    /* Called with nothing running: there is nowhere to go, so ignore it. */
}

int ag_loader_run(ag_loaded_app_t *app, int argc, char **argv)
{
    if (app == NULL || app->binding.entry == NULL) {
        return -AG_EINVAL;
    }

    typedef int (*entry_fn)(int, char **);
    const entry_fn entry = (entry_fn)app->binding.entry;

    s_exit_code = 0;
    s_running = true;

    int result;
    if (setjmp(s_exit_jump) == 0) {
        result = entry(argc, argv);
    } else {
        result = s_exit_code; /* came back through ag_exit */
    }

    s_running = false;
    return result;
}
