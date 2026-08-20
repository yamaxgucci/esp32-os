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
 *
 * Zero for the stack and the heap means "whatever the system gives" - which is
 * the right answer for almost everything, and the reason this is the short form.
 */
#define AG_APP(app_name, app_version, app_author, app_flags)                 \
    AG_APP_SIZED(app_name, app_version, app_author, app_flags, 0, 0)

/*
 * Declares a loadable .SYS driver.  Same header as an application, with the
 * AG_AXE_DRIVER flag set so the loader calls ag_driver_init instead of ag_main.
 *
 *     AG_DRV("ECHO", "1.0", "you");
 *     ag_err_t ag_driver_init(void) { ...; return AG_OK; }
 */
#define AG_DRV(drv_name, drv_version, drv_author)                            \
    AG_APP((drv_name), (drv_version), (drv_author), AG_AXE_DRIVER)

/*
 * The same, for an application that knows how much stack or arena its own work
 * needs.  Both in bytes, either may be 0 to take the default.
 *
 *     AG_APP_SIZED("DEEP", "1.0", "you", 0, 32 * 1024, 4 * 1024 * 1024);
 *
 * Asking is not the same as taking the default: a size named here is required,
 * and the application is refused at startup if it cannot be had.  A default the
 * system cannot meet is quietly reduced instead.  So name a size when running
 * with less would fail anyway, and stay silent otherwise.
 *
 * The stack is clamped to 2..64 KB and comes out of internal SRAM for as long as
 * the process lives; the arena comes from extended memory.
 */
#define AG_APP_SIZED(app_name, app_version, app_author, app_flags,           \
                     app_stack, app_heap)                                    \
    const ag_api_t *g_ag_api = NULL;                                         \
    __attribute__((used, section(".ag_header")))                             \
    const ag_app_header_t __ag_app_header = {                                \
        .magic = AG_AXE_MAGIC,                                               \
        .abi_major = AG_ABI_MAJOR,                                           \
        .abi_minor = AG_ABI_MINOR,                                           \
        .flags = (app_flags),                                                \
        .stack_size = (app_stack),                                           \
        .heap_size = (app_heap),                                             \
        .name = (app_name),                                                  \
        .version = (app_version),                                            \
        .author = (app_author),                                              \
    }

/*
 * Keeps one constant in the code part of the image, which is internal SRAM.
 *
 * Constants normally travel with the data, in PSRAM: a font or a bitmap is read,
 * never executed, and the code arena is tens of kilobytes while PSRAM is
 * megabytes.  Use this for the exception - a small table read in a tight loop,
 * where the read latency matters more than the space it takes from the arena:
 *
 *     AG_HOT_RODATA const uint8_t k_gamma[256] = { ... };
 *
 * It says nothing in a built-in build: that code is already inside the kernel
 * image, which has its own linker script and no .hot_rodata in it, and a section
 * nobody places is a section the linker puts wherever it likes.
 */
#ifdef AG_BUILTIN
#define AG_HOT_RODATA
#else
#define AG_HOT_RODATA __attribute__((used, section(".hot_rodata")))
#endif

/* ---- sys ---------------------------------------------------------------- */

static inline void ag_exit(int code) { g_ag_api->sys->exit(code); }
static inline void ag_panic(const char *m) { g_ag_api->sys->panic(m); }
static inline void ag_sysinfo_get(ag_sysinfo_t *o) { g_ag_api->sys->info(o); }
static inline const char *ag_strerror(ag_err_t e)
{
    return g_ag_api->sys->strerror(e);
}
static inline void ag_heartbeat(void) { g_ag_api->sys->heartbeat(); }

/* ABI 0.19: register .SYS teardown during ag_driver_init (close TCP, etc.). */
static inline void ag_module_on_unload(void (*fn)(void))
{
    if (AG_HAS(g_ag_api->sys, module_on_unload)) {
        g_ag_api->sys->module_on_unload(fn);
    }
}

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
static inline void ag_cursor(bool visible) { g_ag_api->con->set_cursor(visible); }
static inline void ag_coninfo(ag_coninfo_t *o) { g_ag_api->con->info(o); }

/*
 * Straight into a cell of the screen, without moving the cursor and without any
 * chance of scrolling - which is what drawing a frame or a panel needs, and why
 * writing to the bottom right corner with ag_print is a mistake.
 */
static inline void ag_poke(uint16_t x, uint16_t y, char ch, uint8_t attr)
{
    g_ag_api->con->poke(x, y, ch, attr);
}
static inline void ag_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           char ch, uint8_t attr)
{
    g_ag_api->con->fill(x, y, w, h, ch, attr);
}

#define ag_printf(...) (g_ag_api->con->printf(__VA_ARGS__))

/*
 * The code page the screen's bytes are in: 437, 866 or 1251.  Ask before drawing
 * anything above ASCII - the byte for a Cyrillic letter is not the same in 866 as
 * in 1251, and 1251 has no box drawing at all.
 */
static inline uint16_t ag_codepage(void) { return g_ag_api->con->codepage(); }
static inline ag_err_t ag_set_codepage(uint16_t number)
{
    return g_ag_api->con->set_codepage(number);
}

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
/*
 * A file at an address, read only (ABI 0.32).  Ask with
 * AG_HAS(ag_api()->fs, map) first: an older kernel has no such entry.
 */
static inline ag_err_t ag_map(const char *p, const void **out, uint64_t *len)
{
    return g_ag_api->fs->map(p, out, len);
}

static inline ag_err_t ag_unmap(const void *p)
{
    return g_ag_api->fs->unmap(p);
}

static inline ag_err_t ag_stat(const char *p, ag_stat_t *o)
{
    return g_ag_api->fs->stat(p, o);
}
static inline ag_err_t ag_sync(ag_handle_t h) { return g_ag_api->fs->sync(h); }
static inline ag_err_t ag_unlink(const char *p)
{
    return g_ag_api->fs->unlink(p);
}
static inline ag_err_t ag_rename(const char *from, const char *to)
{
    return g_ag_api->fs->rename(from, to);
}
static inline ag_err_t ag_mkdir(const char *p) { return g_ag_api->fs->mkdir(p); }
static inline ag_err_t ag_rmdir(const char *p) { return g_ag_api->fs->rmdir(p); }

static inline ag_handle_t ag_opendir(const char *p)
{
    return g_ag_api->fs->opendir(p);
}
static inline ag_err_t ag_readdir(ag_handle_t h, ag_dirent_t *e)
{
    return g_ag_api->fs->readdir(h, e);
}
static inline ag_err_t ag_closedir(ag_handle_t h)
{
    return g_ag_api->fs->closedir(h);
}

static inline ag_err_t ag_getcwd(char *b, size_t n)
{
    return g_ag_api->fs->getcwd(b, n);
}
static inline ag_err_t ag_chdir(const char *p) { return g_ag_api->fs->chdir(p); }
static inline ag_err_t ag_mountinfo(const char *mount, ag_fsinfo_t *o)
{
    return g_ag_api->fs->mountinfo(mount, o);
}

/* ---- devices ------------------------------------------------------------ */

/*
 * A device handle is a file handle: ag_read, ag_write, ag_seek and ag_close all
 * work on it, and so does opening "D:\\sd0" with ag_open.  What this adds is
 * opening by bare name, asking what exists, and the two things a file has no
 * room for - ioctl and the class vtable.
 */
static inline ag_err_t ag_dev_enumerate(uint32_t index, ag_dev_class_t filter,
                                        ag_devinfo_t *o)
{
    return g_ag_api->dev->enumerate(index, filter, o);
}
static inline ag_handle_t ag_dev_open(const char *name)
{
    return g_ag_api->dev->open(name);
}
static inline ag_err_t ag_dev_close(ag_handle_t h)
{
    return g_ag_api->dev->close(h);
}
static inline int32_t ag_dev_read(ag_handle_t h, void *buf, size_t len)
{
    return g_ag_api->dev->read(h, buf, len);
}
static inline int32_t ag_dev_write(ag_handle_t h, const void *buf, size_t len)
{
    return g_ag_api->dev->write(h, buf, len);
}
static inline ag_err_t ag_dev_ioctl(ag_handle_t h, uint32_t cmd, void *arg,
                                    size_t arglen)
{
    return g_ag_api->dev->ioctl(h, cmd, arg, arglen);
}
static inline const void *ag_dev_ops(ag_handle_t h)
{
    return g_ag_api->dev->ops(h);
}

/*
 * Publish a device from ag_driver_init.  Outside that call the system refuses:
 * an ordinary application is not a driver.  The module that is loading becomes
 * the owner, so unloading it takes the device with it.
 */
static inline ag_err_t ag_dev_add(const ag_dev_add_t *desc)
{
    return g_ag_api->dev->add(desc);
}
static inline ag_err_t ag_dev_remove(const char *name)
{
    return g_ag_api->dev->remove(name);
}
static inline void *ag_dev_priv(ag_device_t *dev)
{
    return g_ag_api->dev->get_priv(dev);
}

/*
 * Which bus and address matched when this .SYS was loaded by probe.  NULL for
 * an ordinary `drv load`, and NULL outside ag_driver_init.
 */
static inline const ag_probe_hint_t *ag_probe_hint(void)
{
    return g_ag_api->dev->probe_hint();
}

/* ---- hardware, directly ------------------------------------------------- */

/*
 * Full trust: drive a pin, talk to a chip on a bus, no driver's permission
 * needed.  What is not allowed is taking what something else is using -
 * gpio_config answers -AG_EACCES for a pin the system needs and -AG_EBUSY for
 * one another process holds.  Reading any pin is always allowed.
 *
 * Everything a process takes comes back when it ends, interrupt handlers
 * included.
 */
static inline ag_err_t ag_gpio_config(int pin, int mode)
{
    return g_ag_api->io->gpio_config(pin, mode);
}
static inline void ag_gpio_write(int pin, int level)
{
    g_ag_api->io->gpio_write(pin, level);
}
static inline int ag_gpio_read(int pin) { return g_ag_api->io->gpio_read(pin); }

static inline ag_err_t ag_gpio_isr(int pin, int edge, ag_isr_fn fn, void *arg)
{
    return g_ag_api->io->gpio_isr(pin, edge, fn, arg);
}
static inline ag_err_t ag_gpio_isr_clear(int pin)
{
    return g_ag_api->io->gpio_isr_clear(pin);
}

/* AG_OK: something is there.  -AG_ENOENT: nothing at that address.
 * -AG_ENODEV: no such bus - it is not described in BOARD.CFG. */
static inline ag_err_t ag_i2c_probe(int bus, uint8_t addr)
{
    return g_ag_api->io->i2c_probe(bus, addr);
}
static inline ag_err_t ag_i2c_write(int bus, uint8_t addr, const void *buf,
                                    size_t len, uint32_t timeout_ms)
{
    return g_ag_api->io->i2c_write(bus, addr, buf, len, timeout_ms);
}
static inline ag_err_t ag_i2c_read(int bus, uint8_t addr, void *buf, size_t len,
                                   uint32_t timeout_ms)
{
    return g_ag_api->io->i2c_read(bus, addr, buf, len, timeout_ms);
}
/* Write then read without letting go of the bus - how a register is read. */
static inline ag_err_t ag_i2c_wrrd(int bus, uint8_t addr, const void *wbuf,
                                   size_t wlen, void *rbuf, size_t rlen,
                                   uint32_t timeout_ms)
{
    return g_ag_api->io->i2c_wrrd(bus, addr, wbuf, wlen, rbuf, rlen,
                                  timeout_ms);
}

/* cs < 0 leaves the chip select to the caller.  At most 1024 bytes at a time. */
static inline ag_err_t ag_spi_xfer(int bus, int cs, const void *tx, void *rx,
                                   size_t len)
{
    return g_ag_api->io->spi_xfer(bus, cs, tx, rx, len);
}

static inline int32_t ag_uart_write(int port, const void *buf, size_t len)
{
    return g_ag_api->io->uart_write(port, buf, len);
}
static inline int32_t ag_uart_read(int port, void *buf, size_t len,
                                   uint32_t timeout_ms)
{
    return g_ag_api->io->uart_read(port, buf, len, timeout_ms);
}

static inline ag_err_t ag_pwm_config(int pin, uint32_t freq_hz, uint8_t bits)
{
    return g_ag_api->io->pwm_config(pin, freq_hz, bits);
}
static inline ag_err_t ag_pwm_set(int pin, uint32_t duty)
{
    return g_ag_api->io->pwm_set(pin, duty);
}

/* ---- processes ---------------------------------------------------------- */

/* Runs another application and waits for it; returns its exit code. */
static inline int32_t ag_exec(const char *path, int argc, const char **argv)
{
    return g_ag_api->proc->exec(path, argc, argv);
}
static inline ag_pid_t ag_spawn(const char *path, int argc, const char **argv,
                                uint32_t flags)
{
    return g_ag_api->proc->spawn(path, argc, argv, flags);
}
static inline ag_err_t ag_wait(ag_pid_t pid, int32_t *code, uint32_t timeout_ms)
{
    return g_ag_api->proc->wait(pid, code, timeout_ms);
}
static inline ag_err_t ag_kill(ag_pid_t pid) { return g_ag_api->proc->kill(pid); }
static inline ag_pid_t ag_getpid(void) { return g_ag_api->proc->self(); }

/*
 * True once when the system has asked this process to stop.  Check it in long
 * loops: an application that does can be asked to stop and tidy up, one that
 * does not has to be killed.
 */
static inline bool ag_interrupted(void)
{
    return g_ag_api->proc->interrupted();
}

/*
 * True while this process's session slot has keyboard/display focus.
 * Outside focus, stop flush/swap and heavy redraw; sleep or yield until
 * AG_EV_FOCUS_GAINED (or this returns true again).
 */
static inline bool ag_focused(void)
{
    return g_ag_api->proc->focused != NULL && g_ag_api->proc->focused();
}

/*
 * Promises to call ag_heartbeat() at least every `ms`; the system stops this
 * process if it does not.  0 disarms.  Only arm it if the promise is true - a
 * long wait for input counts as not reporting.
 */
static inline void ag_watchdog(uint32_t ms) { g_ag_api->proc->watchdog(ms); }

/* ---- threads ------------------------------------------------------------ */

/*
 * Threads belong to the process: whatever it has not tidied up is stopped when
 * it ends.  They are FreeRTOS tasks, so they are preemptive and they cost a
 * stack out of internal memory - the scarce kind.  Four per process.
 */
static inline ag_thread_t ag_thread_create(void (*fn)(void *), void *arg,
                                           const char *name, size_t stack,
                                           int priority, uint32_t flags)
{
    return g_ag_api->task->create(fn, arg, name, stack, priority, flags);
}
static inline ag_err_t ag_thread_join(ag_thread_t t, uint32_t timeout_ms)
{
    return g_ag_api->task->join(t, timeout_ms);
}
static inline void ag_thread_exit(void) { g_ag_api->task->exit(); }
static inline void ag_yield(void) { g_ag_api->task->yield(); }

static inline ag_mutex_t ag_mutex_create(void)
{
    return g_ag_api->task->mutex_create();
}
static inline void ag_mutex_delete(ag_mutex_t m)
{
    g_ag_api->task->mutex_delete(m);
}
static inline bool ag_mutex_lock(ag_mutex_t m, uint32_t timeout_ms)
{
    return g_ag_api->task->mutex_lock(m, timeout_ms);
}
static inline void ag_mutex_unlock(ag_mutex_t m)
{
    g_ag_api->task->mutex_unlock(m);
}

static inline ag_sem_t ag_sem_create(uint32_t initial, uint32_t max)
{
    return g_ag_api->task->sem_create(initial, max);
}
static inline void ag_sem_delete(ag_sem_t s) { g_ag_api->task->sem_delete(s); }
static inline bool ag_sem_take(ag_sem_t s, uint32_t timeout_ms)
{
    return g_ag_api->task->sem_take(s, timeout_ms);
}
static inline void ag_sem_give(ag_sem_t s) { g_ag_api->task->sem_give(s); }

static inline ag_queue_t ag_queue_create(uint32_t items, size_t item_size)
{
    return g_ag_api->task->queue_create(items, item_size);
}
static inline void ag_queue_delete(ag_queue_t q)
{
    g_ag_api->task->queue_delete(q);
}
static inline bool ag_queue_send(ag_queue_t q, const void *item,
                                 uint32_t timeout_ms)
{
    return g_ag_api->task->queue_send(q, item, timeout_ms);
}
static inline bool ag_queue_recv(ag_queue_t q, void *item, uint32_t timeout_ms)
{
    return g_ag_api->task->queue_recv(q, item, timeout_ms);
}

/* ---- time --------------------------------------------------------------- */

static inline ag_time_t ag_micros(void) { return g_ag_api->time->us(); }
/*
 * CPU cycles of the core this runs on, 32-bit and wrapping every ~18 s at
 * 240 MHz.  Use it for inner loops that a microsecond is too coarse to see;
 * measure differences and keep the window short.
 */
static inline uint64_t ag_cycles(void) { return g_ag_api->time->cycles(); }
static inline uint32_t ag_millis(void) { return g_ag_api->time->ms(); }
static inline void ag_delay(uint32_t ms) { g_ag_api->time->delay_ms(ms); }
static inline void ag_delay_us(uint32_t us) { g_ag_api->time->delay_us(us); }

/* ---- input -------------------------------------------------------------- */

/*
 * Whole events, which is what an application that draws its own screen needs:
 * arrow and function keys have no character, so ag_getch cannot report them.
 * timeout_ms: 0 polls, UINT32_MAX waits.
 */
static inline bool ag_poll_event(ag_event_t *e, uint32_t timeout_ms)
{
    return g_ag_api->inp->poll(e, timeout_ms);
}
static inline void ag_flush_input(void) { g_ag_api->inp->flush(); }
static inline uint16_t ag_mods(void) { return g_ag_api->inp->mods(); }

/* ABI 0.18: inject POINTER/KEY/… into the same queue as ag_poll_event. */
static inline bool ag_inject_event(const ag_event_t *e)
{
    if (g_ag_api->inp == NULL || !AG_HAS(g_ag_api->inp, inject)) {
        return false;
    }
    return g_ag_api->inp->inject(e);
}

/* ---- graphics ----------------------------------------------------------- */

static inline ag_err_t ag_gfx_acquire(ag_gfxinfo_t *o)
{
    return g_ag_api->gfx->acquire(o);
}
/*
 * Put pixels the caller owns on the panel.  See gfx->present in abi.h; use
 * AG_HAS(ag_api()->gfx, present) before calling, because a kernel older than
 * ABI 0.31 has no such entry.
 */
static inline ag_err_t ag_gfx_present(const ag_blit_t *b)
{
    return g_ag_api->gfx->present(b);
}

static inline void ag_gfx_release(void) { g_ag_api->gfx->release(); }
static inline void ag_gfx_flush(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    g_ag_api->gfx->flush(x, y, w, h);
}
static inline void ag_gfx_swap(void) { g_ag_api->gfx->swap(); }
static inline void ag_gfx_clear(uint32_t color)
{
    g_ag_api->gfx->clear(color);
}
static inline void ag_gfx_fill_rect(int16_t x, int16_t y, uint16_t w, uint16_t h,
                                    uint32_t color)
{
    g_ag_api->gfx->fill_rect(x, y, w, h, color);
}
static inline void ag_gfx_blit(int16_t x, int16_t y, uint16_t w, uint16_t h,
                               const void *src, uint32_t src_stride,
                               ag_pixfmt_t src_fmt)
{
    g_ag_api->gfx->blit(x, y, w, h, src, src_stride, src_fmt);
}
static inline int32_t ag_gfx_text(int16_t x, int16_t y, const char *s,
                                  uint32_t fg, uint32_t bg)
{
    /* bg = AG_GFX_TRANS: draw fg bits only (no glyph boxes). */
    return g_ag_api->gfx->text(x, y, s, fg, bg);
}
static inline void ag_gfx_pixel(int16_t x, int16_t y, uint32_t color)
{
    g_ag_api->gfx->pixel(x, y, color);
}
static inline void ag_gfx_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                               uint32_t color)
{
    g_ag_api->gfx->line(x0, y0, x1, y1, color);
}
static inline void ag_gfx_circle(int16_t cx, int16_t cy, uint16_t r,
                                 uint32_t color)
{
    g_ag_api->gfx->circle(cx, cy, r, color);
}
static inline void ag_gfx_fill_circle(int16_t cx, int16_t cy, uint16_t r,
                                      uint32_t color)
{
    g_ag_api->gfx->fill_circle(cx, cy, r, color);
}
static inline void ag_gfx_poly_begin(void) { g_ag_api->gfx->poly_begin(); }
static inline ag_err_t ag_gfx_poly_vertex(int16_t x, int16_t y)
{
    return g_ag_api->gfx->poly_vertex(x, y);
}
static inline void ag_gfx_poly_fill(uint32_t color)
{
    g_ag_api->gfx->poly_fill(color);
}
static inline void ag_gfx_poly_stroke(uint32_t color)
{
    g_ag_api->gfx->poly_stroke(color);
}
static inline void ag_gfx_fill_convex(const ag_point_t *pts, int32_t n,
                                      uint32_t color)
{
    g_ag_api->gfx->fill_convex(pts, n, color);
}
static inline void ag_gfx_stroke_convex(const ag_point_t *pts, int32_t n,
                                        uint32_t color)
{
    g_ag_api->gfx->stroke_convex(pts, n, color);
}
static inline void ag_gfx_clip(int16_t x, int16_t y, uint16_t w, uint16_t h)
{
    g_ag_api->gfx->clip(x, y, w, h);
}
static inline void ag_gfx_clip_reset(void) { g_ag_api->gfx->clip_reset(); }
static inline void ag_gfx_stroke_rect(int16_t x, int16_t y, uint16_t w,
                                      uint16_t h, uint32_t color)
{
    g_ag_api->gfx->stroke_rect(x, y, w, h, color);
}
static inline void ag_gfx_fill_round_rect(int16_t x, int16_t y, uint16_t w,
                                          uint16_t h, uint16_t r,
                                          uint32_t color)
{
    g_ag_api->gfx->fill_round_rect(x, y, w, h, r, color);
}
static inline void ag_gfx_blit_key(int16_t x, int16_t y, uint16_t w, uint16_t h,
                                   const void *src, uint32_t src_stride,
                                   ag_pixfmt_t src_fmt, uint32_t key_rgb)
{
    g_ag_api->gfx->blit_key(x, y, w, h, src, src_stride, src_fmt, key_rgb);
}
static inline void ag_gfx_blit_bind(const void *src, uint32_t src_stride)
{
    g_ag_api->gfx->blit_bind(src, src_stride);
}
static inline void ag_gfx_blit_copy(int16_t x, int16_t y, uint16_t w,
                                    uint16_t h)
{
    g_ag_api->gfx->blit_copy(x, y, w, h);
}
static inline void ag_gfx_blit_keyed(int16_t x, int16_t y, uint16_t w,
                                     uint16_t h, uint32_t key_rgb)
{
    g_ag_api->gfx->blit_keyed(x, y, w, h, key_rgb);
}
static inline int32_t ag_gfx_text_fit(int16_t x, int16_t y, uint16_t w,
                                      const char *s, uint32_t fg, uint32_t bg)
{
    return g_ag_api->gfx->text_fit(x, y, w, s, fg, bg);
}
static inline void ag_gfx_blit_src_rect(int16_t sx, int16_t sy, uint16_t sw,
                                        uint16_t sh)
{
    g_ag_api->gfx->blit_src_rect(sx, sy, sw, sh);
}
static inline void ag_gfx_blit_scaled(int16_t dx, int16_t dy, uint16_t dw,
                                      uint16_t dh)
{
    g_ag_api->gfx->blit_scaled(dx, dy, dw, dh);
}
static inline void ag_gfx_blit_tiled(int16_t dx, int16_t dy, uint16_t dw,
                                     uint16_t dh)
{
    g_ag_api->gfx->blit_tiled(dx, dy, dw, dh);
}
static inline void ag_gfx_poly_uv(int16_t u, int16_t v)
{
    g_ag_api->gfx->poly_uv(u, v);
}
static inline void ag_gfx_poly_fill_tex(void)
{
    g_ag_api->gfx->poly_fill_tex();
}

/* ---- audio helpers (ABI 0.14+; built-in pcmnull) ------------------------- */

static inline int ag_audio_present(void)
{
    return g_ag_api->audio != NULL && g_ag_api->audio->present != NULL &&
           g_ag_api->audio->present();
}
static inline int ag_audio_is_hw(void)
{
    return g_ag_api->audio != NULL && g_ag_api->audio->is_hw != NULL &&
           g_ag_api->audio->is_hw();
}
static inline ag_err_t ag_audio_open(const ag_audio_fmt_t *fmt)
{
    if (g_ag_api->audio == NULL || g_ag_api->audio->open == NULL) {
        return -AG_ENOSYS;
    }
    return g_ag_api->audio->open(fmt);
}
static inline void ag_audio_close(void)
{
    if (g_ag_api->audio != NULL && g_ag_api->audio->close != NULL) {
        g_ag_api->audio->close();
    }
}
static inline int32_t ag_audio_write(const int16_t *pcm, int32_t frames)
{
    if (g_ag_api->audio == NULL || g_ag_api->audio->write == NULL) {
        return -AG_ENOSYS;
    }
    return g_ag_api->audio->write(pcm, frames);
}
static inline int32_t ag_audio_space(void)
{
    if (g_ag_api->audio == NULL || g_ag_api->audio->space == NULL) {
        return 0;
    }
    return g_ag_api->audio->space();
}

/* ---- input helpers ------------------------------------------------------ */

static inline bool ag_key(uint16_t keycode)
{
    if (g_ag_api->inp == NULL || g_ag_api->inp->key_pressed == NULL) {
        return false;
    }
    return g_ag_api->inp->key_pressed(keycode);
}
/*
 * Live pad byte from the input layer (HostFS PADPUSH today):
 * 0=pad0, 1=pad1, 2=sys, 3=pad0hi, 4=pad1hi.  0 if the snapshot is stale.
 */
static inline uint32_t ag_pad(int which)
{
    if (g_ag_api->inp == NULL || g_ag_api->inp->pad == NULL) {
        return 0;
    }
    return g_ag_api->inp->pad(which);
}
/* Level button on pad 0 (AG_BTN_*). Prefers live pad; else sticky key_pressed. */
static inline int32_t ag_btn(int id)
{
    if (g_ag_api->inp == NULL || g_ag_api->inp->btn == NULL) {
        return 0;
    }
    return g_ag_api->inp->btn(id);
}
/* Level button on pad 0 or 1 (ABI 0.11). */
static inline int32_t ag_btnp(int pad, int id)
{
    if (g_ag_api->inp == NULL) {
        return 0;
    }
    if (g_ag_api->inp->btnp != NULL) {
        return g_ag_api->inp->btnp(pad, id);
    }
    /* Older kernels: only pad 0 via btn(). */
    return (pad == 0 && g_ag_api->inp->btn != NULL) ? g_ag_api->inp->btn(id)
                                                    : 0;
}

/* ---- net (ABI 0.12; NULL when the build has no networking) -------------- */

static inline bool ag_net_is_ready(void)
{
    return g_ag_api->net != NULL && g_ag_api->net->ready != NULL &&
           g_ag_api->net->ready();
}

static inline ag_err_t ag_net_wait_ready(uint32_t timeout_ms)
{
    if (g_ag_api->net == NULL || g_ag_api->net->wait_ready == NULL) {
        return -AG_ENOSYS;
    }
    return g_ag_api->net->wait_ready(timeout_ms);
}

static inline ag_err_t ag_net_ifaddr(uint32_t *addr_out)
{
    if (g_ag_api->net == NULL || g_ag_api->net->ifaddr == NULL) {
        return -AG_ENOSYS;
    }
    return g_ag_api->net->ifaddr(addr_out);
}

static inline ag_handle_t ag_tcp_listen(uint16_t port)
{
    if (g_ag_api->net == NULL || g_ag_api->net->tcp_listen == NULL) {
        return (ag_handle_t)(-AG_ENOSYS);
    }
    return g_ag_api->net->tcp_listen(port);
}

static inline ag_handle_t ag_tcp_accept(ag_handle_t listen, uint32_t timeout_ms)
{
    if (g_ag_api->net == NULL || g_ag_api->net->tcp_accept == NULL) {
        return (ag_handle_t)(-AG_ENOSYS);
    }
    return g_ag_api->net->tcp_accept(listen, timeout_ms);
}

static inline ag_handle_t ag_tcp_connect(uint32_t addr, uint16_t port,
                                         uint32_t timeout_ms)
{
    if (g_ag_api->net == NULL || g_ag_api->net->tcp_connect == NULL) {
        return (ag_handle_t)(-AG_ENOSYS);
    }
    return g_ag_api->net->tcp_connect(addr, port, timeout_ms);
}

static inline int32_t ag_net_send(ag_handle_t sock, const void *buf, size_t len)
{
    if (g_ag_api->net == NULL || g_ag_api->net->send == NULL) {
        return -AG_ENOSYS;
    }
    return g_ag_api->net->send(sock, buf, len);
}

static inline int32_t ag_net_recv(ag_handle_t sock, void *buf, size_t len)
{
    if (g_ag_api->net == NULL || g_ag_api->net->recv == NULL) {
        return -AG_ENOSYS;
    }
    return g_ag_api->net->recv(sock, buf, len);
}

static inline ag_err_t ag_net_close(ag_handle_t sock)
{
    if (g_ag_api->net == NULL || g_ag_api->net->close == NULL) {
        return -AG_ENOSYS;
    }
    return g_ag_api->net->close(sock);
}

static inline ag_err_t ag_net_set_nonblock(ag_handle_t sock, bool on)
{
    if (g_ag_api->net == NULL || g_ag_api->net->set_nonblock == NULL) {
        return -AG_ENOSYS;
    }
    return g_ag_api->net->set_nonblock(sock, on);
}

#ifdef __cplusplus
}
#endif

#endif /* ARGON_ARGON_H */
