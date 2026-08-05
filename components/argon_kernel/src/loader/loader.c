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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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
 * what placing code needs.
 *
 * ESP-IDF hands none of that region to the heap on this chip - MALLOC_CAP_EXEC
 * and MALLOC_CAP_IRAM_8BIT both report zero bytes available - so the arena is
 * reserved at link time instead.  It costs its full size whether an application
 * is running or not, because the executable window and the data window reach the
 * same physical SRAM, which is why it is configurable rather than large:
 * CONFIG_ARGON_APP_ARENA_KB.
 *
 * Only code lives here.  Data and bss are a separate part of the image and go
 * to PSRAM, so what this bounds is how much code an application has, not how
 * much memory it uses.  Megabytes of code need the code part in the instruction
 * window of PSRAM as well; that is spike S-1 and it needs a board.
 */
#ifndef CONFIG_ARGON_APP_ARENA_KB
#define CONFIG_ARGON_APP_ARENA_KB 64
#endif
#define AG_APP_ARENA_BYTES ((size_t)CONFIG_ARGON_APP_ARENA_KB * 1024u)

/*
 * In IRAM's bss rather than its data: the section is NOLOAD, so the arena costs
 * address space but not a single byte of flash and nothing to copy at boot.  Its
 * contents at startup are of no interest - an image is written over all of it
 * before anything runs from it.
 */
static uint8_t s_arena[AG_APP_ARENA_BYTES]
    __attribute__((aligned(16), section(".iram.bss.ag_app_arena")));
static bool s_arena_taken;

static void *arena_take(size_t bytes)
{
    if (s_arena_taken || bytes > sizeof(s_arena)) {
        return NULL;
    }
    s_arena_taken = true;
    return s_arena;
}

static void arena_release(void *p)
{
    if (p == s_arena) {
        s_arena_taken = false;
    }
}

size_t ag_loader_arena_size(void) { return sizeof(s_arena); }
bool   ag_loader_arena_busy(void) { return s_arena_taken; }

/*
 * The data part goes to extended memory: it has to be writable, not executable,
 * and PSRAM is both large and reached through the data bus.  No cache
 * maintenance is needed for it - the application writes and reads it through
 * that same bus - which is what makes this step safe to build without a board.
 */
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

/*
 * Places both parts of a validated image.  A contiguous image gets one
 * allocation with the data immediately after the code, because its code reaches
 * its data by distance rather than by address.
 */
static ag_err_t place_image(const ag_axe_header_t *header, ag_loaded_app_t *out)
{
    const bool contiguous = (header->flags & AG_AXE_CONTIGUOUS) != 0;
    const size_t code_bytes =
        contiguous ? (size_t)header->code.size + header->data.size
                   : (size_t)header->code.size;

    void *code = arena_take(code_bytes);
    if (code == NULL) {
        ag_log(AG_LOG_ERROR, "loader",
               "%u bytes of code will not fit the %u byte arena%s",
               (unsigned)code_bytes, (unsigned)sizeof(s_arena),
               s_arena_taken ? " (already in use)" : "");
        return -AG_ENOMEM;
    }

    out->place.code = code;
    out->place.code_capacity = code_bytes;

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
        ag_log(AG_LOG_ERROR, "loader",
               "no memory for %u bytes of application data",
               (unsigned)header->data.size);
        arena_release(code);
        memset(&out->place, 0, sizeof(out->place));
        return -AG_ENOMEM;
    }

    out->place.data = data;
    out->place.data_capacity = header->data.size;
    out->data_owned = data;
    return AG_OK;
}

static void release_image(ag_loaded_app_t *app)
{
    if (app->data_owned != NULL) {
        heap_caps_free(app->data_owned);
    }
    arena_release(app->place.code);
    memset(&app->place, 0, sizeof(app->place));
    app->data_owned = NULL;
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

    memcpy(out->place.code, file + header->code.offset, header->code.file_size);
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

    /*
     * No cache maintenance here on purpose.  The code went to internal RAM,
     * which sits in front of the cache and is reachable from both the data and
     * the instruction bus, so what was written is what will be executed; the
     * data went to PSRAM through the data bus, which is the bus that will read
     * it.  Code placed in the instruction window of PSRAM will need a writeback
     * and an instruction-cache invalidate before the jump - that belongs with
     * the PSRAM mapping, which needs a board to verify.
     */
    ag_log(AG_LOG_INFO, "loader",
           "%s: %s v%s, code %u B at %p, data %u B at %p, %u relocations",
           header->name, ag_axe_arch_name((ag_axe_arch_t)header->arch),
           header->version, (unsigned)header->code.size, out->place.code,
           (unsigned)header->data.size, out->place.data,
           (unsigned)out->binding.relocated);
    return AG_OK;
}

void ag_loader_unload(ag_loaded_app_t *app)
{
    if (app != NULL && app->place.code != NULL) {
        release_image(app);
        memset(app, 0, sizeof(*app));
    }
}

/* ------------------------------------------------------------------------ */
/* Running                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * What one run needs, on the caller's stack for as long as the run lasts.  One
 * application at a time for now, so one of these is in flight at a time; the
 * process model turns it into a table.
 */
typedef struct {
    ag_loaded_app_t  *app;
    int               argc;
    char            **argv;
    int               result;
    int               exit_code;
    jmp_buf           exit_jump;
    TaskHandle_t      task;
    SemaphoreHandle_t done;
} app_run_t;

static app_run_t *s_current;

/* Bounds on what an image may ask for; the default comes from the config. */
#define AG_APP_STACK_MIN (2u * 1024u)
#define AG_APP_STACK_MAX (64u * 1024u)
#ifndef CONFIG_ARGON_APP_STACK_KB
#define CONFIG_ARGON_APP_STACK_KB 16
#endif

static uint32_t stack_bytes(const ag_axe_header_t *header)
{
    uint32_t want = header->stack_size;
    if (want == 0) {
        want = (uint32_t)CONFIG_ARGON_APP_STACK_KB * 1024u;
    }
    if (want < AG_APP_STACK_MIN) {
        want = AG_APP_STACK_MIN;
    }
    if (want > AG_APP_STACK_MAX) {
        want = AG_APP_STACK_MAX;
    }
    return want;
}

static void app_task(void *arg)
{
    app_run_t *run = (app_run_t *)arg;

    /*
     * Taken here rather than from xTaskCreate so that it is set before anything
     * of the application runs: ag_exit compares against it to know it is being
     * called from the application and not from somewhere else.
     */
    run->task = xTaskGetCurrentTaskHandle();

    typedef int (*entry_fn)(int, char **);
    const entry_fn entry = (entry_fn)run->app->binding.entry;

    if (setjmp(run->exit_jump) == 0) {
        run->result = entry(run->argc, run->argv);
    } else {
        run->result = run->exit_code; /* came back through ag_exit */
    }

    /*
     * How much of the stack was never touched, while the task still exists.  An
     * application that comes close is worth telling its author about.
     */
    const uint32_t spare =
        (uint32_t)uxTaskGetStackHighWaterMark(NULL);

    SemaphoreHandle_t done = run->done;
    s_current = NULL;

    ag_log(AG_LOG_DEBUG, "loader", "%s: returned %d, %u stack bytes unused",
           run->app->header.name, run->result, (unsigned)spare);

    /* Nothing may touch `run` after this: the waiter owns it again. */
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}

void ag_loader_exit(int code)
{
    app_run_t *run = s_current;

    if (run != NULL && run->task == xTaskGetCurrentTaskHandle()) {
        run->exit_code = code;
        longjmp(run->exit_jump, 1);
    }
    /* Called with nothing running: there is nowhere to go, so ignore it. */
}

int ag_loader_run(ag_loaded_app_t *app, int argc, char **argv)
{
    if (app == NULL || app->binding.entry == NULL) {
        return -AG_EINVAL;
    }
    if (s_current != NULL) {
        return -AG_EBUSY;
    }

    app_run_t run;
    memset(&run, 0, sizeof(run));
    run.app = app;
    run.argc = argc;
    run.argv = argv;

    run.done = xSemaphoreCreateBinary();
    if (run.done == NULL) {
        return -AG_ENOMEM;
    }

    const char *name = (app->header.name[0] != '\0') ? app->header.name : "app";
    s_current = &run;

    /*
     * Pinned to the core the shell is on, deliberately.  Nothing in the kernel
     * is written for two cores executing it at once, and an application calls
     * into the kernel constantly; giving it a core of its own belongs with the
     * process model, where the locking is designed rather than hoped for.
     *
     * Same priority as the caller: an application should not be able to starve
     * the system it asked to run it, nor be starved by it.
     */
    BaseType_t created = xTaskCreatePinnedToCore(
        app_task, name, (uint32_t)stack_bytes(&app->header), &run,
        uxTaskPriorityGet(NULL), NULL, xPortGetCoreID());

    if (created != pdPASS) {
        s_current = NULL;
        vSemaphoreDelete(run.done);
        ag_log(AG_LOG_ERROR, "loader", "%s: no memory for a %u byte stack", name,
               (unsigned)stack_bytes(&app->header));
        return -AG_ENOMEM;
    }

    xSemaphoreTake(run.done, portMAX_DELAY);
    vSemaphoreDelete(run.done);
    return run.result;
}
