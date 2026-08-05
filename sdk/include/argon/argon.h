/*
 * ArgonOS - convenience header for application and driver authors.
 *
 * Everything here is a static inline wrapper over the syscall table, so the
 * generated code is a single indirect call with no extra frame.  Include this
 * instead of <argon/abi.h> when writing an application.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_ARGON_H
#define ARGON_ARGON_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Declares the application image header and the API pointer slot.  Put this
 * once at file scope in the application's main translation unit:
 *
 *     AG_APP("HELLO", "1.0", "you", 0);
 *     int ag_main(int argc, char **argv) { ag_print("hi\n"); return 0; }
 */
#define AG_APP(app_name, app_version, app_author, app_flags)                 \
    const ag_api_t *g_ag_api = NULL;                                         \
    __attribute__((used, section(".ag_header")))                             \
    const ag_app_header_t __ag_app_header = {                                \
        .magic = AG_AXE_MAGIC,                                               \
        .abi_major = AG_ABI_MAJOR,                                           \
        .abi_minor = AG_ABI_MINOR,                                           \
        .flags = (app_flags),                                                \
        .stack_size = 0,                                                     \
        .heap_size = 0,                                                      \
        .name = (app_name),                                                  \
        .version = (app_version),                                            \
        .author = (app_author),                                              \
    }

/* ---- sys ---------------------------------------------------------------- */

static inline void ag_exit(int code) { g_ag_api->sys->exit(code); }
static inline void ag_panic(const char *m) { g_ag_api->sys->panic(m); }
static inline void ag_sysinfo_get(ag_sysinfo_t *o) { g_ag_api->sys->info(o); }
static inline const char *ag_strerror(ag_err_t e)
{
    return g_ag_api->sys->strerror(e);
}
static inline void ag_heartbeat(void) { g_ag_api->sys->heartbeat(); }

#define ag_log(lvl, tag, ...) (g_ag_api->sys->log((lvl), (tag), __VA_ARGS__))

/* ---- memory ------------------------------------------------------------- */

static inline void *ag_malloc(size_t n) { return g_ag_api->mem->alloc(n); }
static inline void *ag_malloc_caps(size_t n, uint32_t caps)
{
    return g_ag_api->mem->alloc_caps(n, caps);
}
static inline void *ag_realloc(void *p, size_t n)
{
    return g_ag_api->mem->realloc(p, n);
}
static inline void ag_free(void *p) { g_ag_api->mem->free(p); }
static inline void ag_meminfo(ag_meminfo_t *o) { g_ag_api->mem->info(o); }

/* ---- console ------------------------------------------------------------ */

static inline int32_t ag_print(const char *s) { return g_ag_api->con->puts(s); }
static inline int32_t ag_getch(void) { return g_ag_api->con->getch(); }
static inline int32_t ag_kbhit(void) { return g_ag_api->con->kbhit(); }
static inline void ag_cls(void) { g_ag_api->con->cls(); }
static inline void ag_gotoxy(uint16_t x, uint16_t y)
{
    g_ag_api->con->gotoxy(x, y);
}
static inline void ag_color(uint8_t fg, uint8_t bg)
{
    g_ag_api->con->set_attr(AG_ATTR(fg, bg));
}
static inline int32_t ag_readline(char *b, size_t n)
{
    return g_ag_api->con->readline(b, n);
}

#define ag_printf(...) (g_ag_api->con->printf(__VA_ARGS__))

/* ---- files -------------------------------------------------------------- */

static inline ag_handle_t ag_open(const char *p, uint32_t f)
{
    return g_ag_api->fs->open(p, f);
}
static inline ag_err_t ag_close(ag_handle_t h) { return g_ag_api->fs->close(h); }
static inline int32_t ag_read(ag_handle_t h, void *b, size_t n)
{
    return g_ag_api->fs->read(h, b, n);
}
static inline int32_t ag_write(ag_handle_t h, const void *b, size_t n)
{
    return g_ag_api->fs->write(h, b, n);
}
static inline int64_t ag_seek(ag_handle_t h, int64_t off, int whence)
{
    return g_ag_api->fs->seek(h, off, whence);
}
static inline ag_err_t ag_stat(const char *p, ag_stat_t *o)
{
    return g_ag_api->fs->stat(p, o);
}

/* ---- time --------------------------------------------------------------- */

static inline ag_time_t ag_micros(void) { return g_ag_api->time->us(); }
static inline uint32_t ag_millis(void) { return g_ag_api->time->ms(); }
static inline void ag_delay(uint32_t ms) { g_ag_api->time->delay_ms(ms); }
static inline void ag_delay_us(uint32_t us) { g_ag_api->time->delay_us(us); }

/* ---- input -------------------------------------------------------------- */

static inline bool ag_poll_event(ag_event_t *e, uint32_t timeout_ms)
{
    return g_ag_api->inp->poll(e, timeout_ms);
}

/* ---- graphics ----------------------------------------------------------- */

static inline ag_err_t ag_gfx_acquire(ag_gfxinfo_t *o)
{
    return g_ag_api->gfx->acquire(o);
}
static inline void ag_gfx_release(void) { g_ag_api->gfx->release(); }
static inline void ag_gfx_flush(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    g_ag_api->gfx->flush(x, y, w, h);
}

#ifdef __cplusplus
}
#endif

#endif /* ARGON_ARGON_H */
