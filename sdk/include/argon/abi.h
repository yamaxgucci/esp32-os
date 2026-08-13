/*
 * ArgonOS - public application binary interface.
 *
 * This header is the single source of truth for the contract between the
 * kernel and third-party applications (.AXE) and drivers (.SYS).  It is
 * compiled into both sides, so it must stay free of any kernel-private or
 * ESP-IDF-private type.
 *
 * Compatibility rules (see docs/00-architecture.md, section 4):
 *   - every sub-table starts with `size`, so callers can feature-probe;
 *   - new functions are appended to the END of a sub-table  -> minor + 1;
 *   - new sub-tables are appended to the END of ag_api_t     -> minor + 1;
 *   - anything else (signature change, removal, reorder)     -> major + 1.
 *
 * This header is Apache-2.0 (see LICENSING.md).  Using only this ABI does not
 * make an application a derivative of the GPL kernel.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_ABI_H
#define ARGON_ABI_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minor goes up when the table grows: a new call at the end of a subtable, or a
 * subtable that stops being NULL.  An image built against an older minor keeps
 * running, which is why the loader compares them rather than demanding a match.
 *
 * 0.2 added the proc and task subtables, and proc->interrupted with them.
 * 0.3 added con->codepage and con->set_codepage at the end of the con subtable.
 * 0.4 made dev stop being NULL: the device registry, /dev, and the ioctl
 *     numbering with it.
 * 0.5 made io stop being NULL: pins, interrupts and the buses.  io->adc_read is
 *     NULL unless the build asked for the ADC - see AG_HAS.
 * 0.6 appended add / remove / get_priv on dev: a .SYS module can publish a
 *     device from ag_driver_init and take it back on unload.
 * 0.7 appended probe_hint on dev: during a probe-driven load, ag_driver_init
 *     can ask which bus and address matched.
 * 0.8 made gfx stop being NULL: soft RGB565 framebuffer, acquire/release,
 *     primitives, and built-in 8x16 text.
 * 0.9 added soft-draw: pixel/line/circle/fill_circle/poly_* / fill_convex.
 * 0.10 appended inp->btn / inp->pad: HostFS PADPUSH live buttons (SMS/Asteroids).
 * 0.11 appended inp->btnp and extended ag_btn (C/START/X/Y/Z/MODE); pad()
 *      also returns high bytes (which 3/4).  Input layer + /dev/joy0.
 * 0.12 made net stop being NULL (when the build enables networking): TCP
 *      listen/accept/connect/send/recv plus ready/wait_ready/ifaddr.
 * 0.13 appended net->set_nonblock.
 * 0.14 added api->audio (I2S or discard stub) and AG_AXE_NEEDS_AUDIO.
 * 0.15 pcmnull built-in; AG_IOC_AUDIO_GETFMT/SETFMT; I2S/virt via .SYS drivers.
 * 0.16 appended gfx clip / clip_reset / stroke_rect / fill_round_rect.
 * 0.17 appended gfx blit_key + stateful blit_bind / blit_copy / blit_keyed
 *      (RGB565 chroma blit; CC-friendly ≤6-arg path).
 * 0.18 appended inp->inject: push ag_event_t into the console/input queue
 *      (MOUSEVIRT /dev/mouse0 → POINTER_* for gfx apps).
 * 0.19 appended sys->module_on_unload so .SYS can close TCP listens on reload.
 * 0.22 appended optional per-open session ops on ag_dev_ops (multi-open devices
 *      such as /dev/pcmmix); built-in software PCM mixer.
 * 0.23 defined ag_display_ops_t; soft fb0 publishes it via dev->ops(h).
 * 0.24 soft gfx: ag_gfx_text(bg=AG_GFX_TRANS) skips off-bits; blit() blends
 *      AG_PIX_ARGB8888 (LE bytes B,G,R,A) onto the RGB565 surface.  No new
 *      vtable slots.
 * 0.25 appended gfx->text_fit: 8×16 text clipped to a pixel width, with
 *      "..." when the string does not fit.
 * 0.26 appended blit_src_rect / blit_scaled / blit_tiled (nearest RGB565 on
 *      the bound source) and poly_uv / poly_fill_tex (affine UV).
 */
#define AG_ABI_MAJOR 0u
#define AG_ABI_MINOR 26u

/* ------------------------------------------------------------------------ */
/* Basic types                                                              */
/* ------------------------------------------------------------------------ */

/* Negative on failure (-AG_Exxx), >= 0 on success. */
typedef int32_t ag_err_t;

typedef int32_t ag_handle_t; /* file / device / socket handle, < 0 is invalid */
typedef int32_t ag_pid_t;
typedef uint64_t ag_time_t; /* microseconds since boot */

#define AG_INVALID_HANDLE ((ag_handle_t)-1)

enum ag_errno {
    AG_OK = 0,
    AG_EPERM = 1,     /* operation not permitted                */
    AG_ENOENT = 2,    /* no such file, device or symbol         */
    AG_EINTR = 4,     /* interrupted by signal / stop request   */
    AG_EIO = 5,       /* hardware or media I/O error            */
    AG_EBADF = 9,     /* invalid handle                         */
    AG_EAGAIN = 11,   /* would block / try again                */
    AG_ENOMEM = 12,   /* out of memory                          */
    AG_EACCES = 13,   /* access denied                          */
    AG_EBUSY = 16,    /* resource is exclusively held           */
    AG_EEXIST = 17,   /* already exists                         */
    AG_ENODEV = 19,   /* no such device                         */
    AG_ENOTDIR = 20,  /* not a directory                        */
    AG_EISDIR = 21,   /* is a directory                         */
    AG_EINVAL = 22,   /* invalid argument                       */
    AG_ENFILE = 23,   /* handle table full                      */
    AG_ENOSPC = 28,   /* no space left on device                */
    AG_EROFS = 30,    /* read-only filesystem                   */
    AG_ERANGE = 34,   /* value out of range                     */
    AG_ENOSYS = 38,   /* not implemented in this build/profile  */
    AG_ENOTSUP = 45,  /* not supported by this device           */
    AG_ETIMEDOUT = 60,/* operation timed out                    */
    AG_EABI = 90,     /* ABI mismatch                           */
    AG_EFORMAT = 91,  /* malformed executable / file format     */
    AG_EKILLED = 92,  /* process was terminated by supervisor   */
};

/* ------------------------------------------------------------------------ */
/* Feature probing                                                          */
/* ------------------------------------------------------------------------ */

/*
 * True when `tbl` exists, is new enough to contain `fn`, and `fn` is non-NULL.
 * Usage:  if (AG_HAS(ag_api()->fs, rename)) { ... }
 */
#define AG_HAS(tbl, fn)                                                     \
    ((tbl) != NULL &&                                                       \
     (tbl)->size >= (uint32_t)(__builtin_offsetof(__typeof__(*(tbl)), fn) + \
                               sizeof((tbl)->fn)) &&                        \
     (tbl)->fn != NULL)

/* ------------------------------------------------------------------------ */
/* sys - identity, logging, lifetime                                        */
/* ------------------------------------------------------------------------ */

typedef enum {
    AG_LOG_ERROR = 0,
    AG_LOG_WARN = 1,
    AG_LOG_INFO = 2,
    AG_LOG_DEBUG = 3,
    AG_LOG_TRACE = 4,
} ag_log_level_t;

typedef struct {
    char     os_name[16];    /* "ArgonOS"                                   */
    char     os_version[16]; /* "0.1.0"                                     */
    char     build[24];      /* git describe                                */
    char     chip[16];       /* "esp32s3"                                   */
    char     board[24];      /* board pack id                               */
    char     profile[8];     /* "full" | "lite" | "nano"                    */
    uint32_t cpu_hz;
    uint8_t  cpu_cores;
    uint8_t  app_core; /* core index dedicated to the foreground app        */
    uint16_t abi_major;
    uint16_t abi_minor;
} ag_sysinfo_t;

typedef struct ag_sys_api {
    uint32_t size;

    void (*info)(ag_sysinfo_t *out);
    void (*log)(ag_log_level_t lvl, const char *tag, const char *fmt, ...);
    void (*vlog)(ag_log_level_t lvl, const char *tag, const char *fmt,
                 va_list ap);

    /* Terminate the calling process.  Does not return. */
    void (*exit)(int code);
    /* Abort with a message; produces a crash record.  Does not return. */
    void (*panic)(const char *msg);

    /* Optional / experimental entry points, looked up by name. */
    void *(*sym)(const char *name);

    /* Human readable text for an ag_errno value. */
    const char *(*strerror)(ag_err_t err);

    /* Tell the supervisor the process is alive (per-process watchdog). */
    void (*heartbeat)(void);

    /*
     * ABI 0.19: during ag_driver_init only — register teardown (close listen
     * sockets, etc.) before the image is unmapped on drv unload/replace.
     */
    void (*module_on_unload)(void (*fn)(void));
} ag_sys_api_t;

/* ------------------------------------------------------------------------ */
/* mem - per-process allocator                                              */
/* ------------------------------------------------------------------------ */

enum ag_mem_caps {
    AG_MEM_DEFAULT = 0,      /* process arena (PSRAM when available)        */
    AG_MEM_FAST = 1u << 0,   /* internal SRAM, low latency                  */
    AG_MEM_DMA = 1u << 1,    /* DMA capable                                 */
    AG_MEM_EXEC = 1u << 2,   /* executable                                  */
    AG_MEM_ZERO = 1u << 3,   /* zero-initialised                            */
    AG_MEM_ALIGN32 = 1u << 4,/* 32-byte aligned (cache line)                */
};

typedef struct {
    size_t arena_total;
    size_t arena_free;
    size_t arena_largest;
    size_t fast_free;   /* internal SRAM available to this process          */
    size_t system_free; /* kernel-side free memory, informational           */
} ag_meminfo_t;

typedef struct ag_mem_api {
    uint32_t size;

    void *(*alloc)(size_t bytes);
    void *(*alloc_caps)(size_t bytes, uint32_t caps);
    void *(*realloc)(void *ptr, size_t bytes);
    void (*free)(void *ptr);
    size_t (*usable_size)(const void *ptr);
    void (*info)(ag_meminfo_t *out);
} ag_mem_api_t;

/* ------------------------------------------------------------------------ */
/* fs - virtual filesystem                                                  */
/* ------------------------------------------------------------------------ */

enum ag_open_flags {
    AG_O_RDONLY = 0,
    AG_O_WRONLY = 1u << 0,
    AG_O_RDWR = 1u << 1,
    AG_O_CREATE = 1u << 2,
    AG_O_TRUNC = 1u << 3,
    AG_O_APPEND = 1u << 4,
    AG_O_EXCL = 1u << 5,
};

enum ag_seek_whence { AG_SEEK_SET = 0, AG_SEEK_CUR = 1, AG_SEEK_END = 2 };

enum ag_file_attr {
    AG_A_DIR = 1u << 0,
    AG_A_READONLY = 1u << 1,
    AG_A_HIDDEN = 1u << 2,
    AG_A_SYSTEM = 1u << 3,
};

/*
 * How long a path and a name can be.  Part of the contract rather than a kernel
 * detail: an application that calls getcwd has to size a buffer, and guessing is
 * how buffers get overrun.
 */
#define AG_PATH_MAX 256
#define AG_NAME_MAX 64

typedef struct {
    uint64_t size;
    uint64_t mtime; /* unix seconds, 0 when unknown                         */
    uint32_t attr;  /* ag_file_attr bitmask                                 */
} ag_stat_t;

typedef struct {
    char       name[256];
    ag_stat_t  st;
} ag_dirent_t;

typedef struct {
    char     fs[8];   /* "fat", "lfs", "ram", "dev"                         */
    uint64_t total;
    uint64_t free;
    bool     read_only;
    bool     removable;
} ag_fsinfo_t;

typedef struct ag_fs_api {
    uint32_t size;

    ag_handle_t (*open)(const char *path, uint32_t flags);
    ag_err_t (*close)(ag_handle_t h);
    int32_t (*read)(ag_handle_t h, void *buf, size_t len);
    int32_t (*write)(ag_handle_t h, const void *buf, size_t len);
    int64_t (*seek)(ag_handle_t h, int64_t off, int whence);
    ag_err_t (*sync)(ag_handle_t h);
    ag_err_t (*truncate)(ag_handle_t h, uint64_t len);

    ag_err_t (*stat)(const char *path, ag_stat_t *out);
    ag_err_t (*unlink)(const char *path);
    ag_err_t (*rename)(const char *from, const char *to);
    ag_err_t (*mkdir)(const char *path);
    ag_err_t (*rmdir)(const char *path);

    ag_handle_t (*opendir)(const char *path);
    ag_err_t (*readdir)(ag_handle_t h, ag_dirent_t *out);
    ag_err_t (*closedir)(ag_handle_t h);

    ag_err_t (*getcwd)(char *buf, size_t len);
    ag_err_t (*chdir)(const char *path);

    ag_err_t (*mountinfo)(const char *mount, ag_fsinfo_t *out);
} ag_fs_api_t;

/* ------------------------------------------------------------------------ */
/* con - text console (multiplexed across UART / telnet / local display)    */
/* ------------------------------------------------------------------------ */

enum ag_color {
    AG_BLACK = 0, AG_BLUE, AG_GREEN, AG_CYAN,
    AG_RED, AG_MAGENTA, AG_BROWN, AG_LGRAY,
    AG_DGRAY, AG_LBLUE, AG_LGREEN, AG_LCYAN,
    AG_LRED, AG_LMAGENTA, AG_YELLOW, AG_WHITE,
};

/* Attribute byte, CGA-compatible layout: bg << 4 | fg. */
#define AG_ATTR(fg, bg) ((uint8_t)(((bg) << 4) | ((fg) & 0x0f)))

typedef struct {
    uint16_t cols;
    uint16_t rows;
    uint16_t cur_x;
    uint16_t cur_y;
    uint8_t  attr;
    bool     has_local_display;
} ag_coninfo_t;

typedef struct ag_con_api {
    uint32_t size;

    int32_t (*write)(const char *buf, size_t len);
    int32_t (*puts)(const char *s);
    int32_t (*printf)(const char *fmt, ...);
    int32_t (*vprintf)(const char *fmt, va_list ap);

    /* Blocking read of one decoded character; < 0 on error. */
    int32_t (*getch)(void);
    /* Non-blocking: number of characters ready. */
    int32_t (*kbhit)(void);
    /* Blocking read of a line with basic editing; returns length. */
    int32_t (*readline)(char *buf, size_t len);

    void (*cls)(void);
    void (*gotoxy)(uint16_t x, uint16_t y);
    void (*set_attr)(uint8_t attr);
    void (*set_cursor)(bool visible);
    void (*info)(ag_coninfo_t *out);

    /* Direct text-cell access, DOS video-memory style. */
    void (*poke)(uint16_t x, uint16_t y, char ch, uint8_t attr);
    void (*fill)(uint16_t x, uint16_t y, uint16_t w, uint16_t h, char ch,
                 uint8_t attr);

    /*
     * The code page of the screen: 437, 866 or 1251.  A cell holds one byte, and
     * this says what that byte means - an application that draws Cyrillic has to
     * know which bytes to write, and one that ships its own box drawing has to
     * know whether the page has any.
     *
     * set_codepage returns -AG_EINVAL for a number the system does not know, and
     * changing the page does not rewrite what is already on the screen.
     */
    uint16_t (*codepage)(void);
    ag_err_t (*set_codepage)(uint16_t number);
} ag_con_api_t;

/* ------------------------------------------------------------------------ */
/* inp - normalised input events                                            */
/* ------------------------------------------------------------------------ */

typedef enum {
    AG_EV_NONE = 0,
    AG_EV_KEY_DOWN,
    AG_EV_KEY_UP,
    AG_EV_CHAR,
    AG_EV_POINTER_DOWN,
    AG_EV_POINTER_UP,
    AG_EV_POINTER_MOVE,
    AG_EV_WHEEL,
    AG_EV_DEVICE_ADDED,
    AG_EV_DEVICE_REMOVED,
    AG_EV_MEDIA_INSERTED,
    AG_EV_MEDIA_REMOVED,
    AG_EV_FOCUS_GAINED,
    AG_EV_FOCUS_LOST,
    AG_EV_QUIT, /* supervisor politely asks the process to stop            */
} ag_event_type_t;

enum ag_keymod {
    AG_MOD_SHIFT = 1u << 0,
    AG_MOD_CTRL = 1u << 1,
    AG_MOD_ALT = 1u << 2,
    AG_MOD_GUI = 1u << 3,
    AG_MOD_CAPS = 1u << 4,
    AG_MOD_NUM = 1u << 5,
};

typedef struct {
    ag_event_type_t type;
    ag_time_t       ts;
    union {
        struct {
            uint16_t keycode; /* USB HID usage id, layout independent      */
            uint32_t unicode; /* 0 when the key produces no character      */
            uint16_t mods;
            bool     repeat;
        } key;
        struct {
            int16_t  x, y;
            int16_t  dx, dy;
            uint8_t  buttons;
            uint8_t  slot; /* multi-touch finger index                     */
        } ptr;
        struct {
            char name[24];
        } dev;
    };
} ag_event_t;

/*
 * Button ids for inp->btn / btnp (level state, not edges).
 * Low six match the classic pad0/pad1 byte; PAUSE/QUIT are sys bits.
 * C..MODE live in the high byte of the 6-byte PADPUSH snapshot (ABI 0.11).
 */
enum ag_btn {
    AG_BTN_UP = 0,
    AG_BTN_DOWN = 1,
    AG_BTN_LEFT = 2,
    AG_BTN_RIGHT = 3,
    AG_BTN_B1 = 4, /* fire / A — sms.cfg pad0.b1 */
    AG_BTN_B2 = 5, /* B */
    AG_BTN_PAUSE = 6,
    AG_BTN_QUIT = 7,
    AG_BTN_C = 8,
    AG_BTN_START = 9,
    AG_BTN_X = 10,
    AG_BTN_Y = 11,
    AG_BTN_Z = 12,
    AG_BTN_MODE = 13,
};

typedef struct ag_inp_api {
    uint32_t size;

    /* timeout_ms: 0 = poll, UINT32_MAX = block forever. */
    bool (*poll)(ag_event_t *out, uint32_t timeout_ms);
    void (*flush)(void);
    bool (*key_pressed)(uint16_t keycode);
    uint16_t (*mods)(void);
    /*
     * Live pad byte from the input layer (HostFS PADPUSH today):
     * which 0=pad0, 1=pad1, 2=sys, 3=pad0hi, 4=pad1hi.
     * 0 if the snapshot is missing/stale.  Prefer btn()/btnp() from games.
     */
    uint32_t (*pad)(int which);
    /* Level button on pad 0: 1 if held.  Live pad when available, else sticky. */
    int32_t (*btn)(int id);
    /* Level button on pad 0 or 1 (ABI 0.11).  Same fallback rules as btn(). */
    int32_t (*btnp)(int pad, int id);
    /*
     * ABI 0.18: inject a fully-formed event into the same queue as poll().
     * Used by MOUSEVIRT.SYS (and later USB HID) for POINTER and WHEEL events.
     * Returns false if the queue is full or the event type is refused.
     */
    bool (*inject)(const ag_event_t *ev);
} ag_inp_api_t;

/* ------------------------------------------------------------------------ */
/* gfx - framebuffer access to the local display                            */
/* ------------------------------------------------------------------------ */

typedef enum {
    AG_PIX_NONE = 0,
    AG_PIX_MONO1,    /* 1 bpp, packed                                      */
    AG_PIX_RGB565,
    AG_PIX_RGB565_BE,
    AG_PIX_RGB888,
    AG_PIX_ARGB8888, /* packed LE: B,G,R,A per pixel; blit() blends onto RGB565 */
} ag_pixfmt_t;

/*
 * Not a 0x00RRGGBB colour.  Pass as ag_gfx_text() bg to leave off-bits alone
 * (labels over skins / sprites).  0 still means opaque black.
 */
#define AG_GFX_TRANS 0xFFFFFFFFu

typedef struct {
    uint16_t    width;
    uint16_t    height;
    ag_pixfmt_t fmt;
    uint32_t    stride;    /* bytes per row                                */
    void       *fb;        /* NULL until acquired                          */
    bool        double_buf;
    bool        direct;    /* true when fb is scanned out without a flush  */
} ag_gfxinfo_t;

typedef struct {
    int16_t x;
    int16_t y;
} ag_point_t;

typedef struct ag_gfx_api {
    uint32_t size;

    /* Take over the local display; text console is suspended on it. */
    ag_err_t (*acquire)(ag_gfxinfo_t *out);
    void (*release)(void);
    /* Push a dirty rectangle to the panel (no-op for `direct` displays). */
    void (*flush)(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void (*swap)(void); /* double buffered displays                        */

    void (*clear)(uint32_t color);
    void (*fill_rect)(int16_t x, int16_t y, uint16_t w, uint16_t h,
                      uint32_t color);
    void (*blit)(int16_t x, int16_t y, uint16_t w, uint16_t h, const void *src,
                 uint32_t src_stride, ag_pixfmt_t src_fmt);
    /* Built-in 8×16 font; returns advance in pixels.  bg=AG_GFX_TRANS: fg only. */
    int32_t (*text)(int16_t x, int16_t y, const char *s, uint32_t fg,
                    uint32_t bg);
    void (*backlight)(uint8_t percent);

    /* Soft-draw primitives (RGB colour 0x00RRGGBB). Require acquire. */
    void (*pixel)(int16_t x, int16_t y, uint32_t color);
    void (*line)(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                 uint32_t color);
    void (*circle)(int16_t cx, int16_t cy, uint16_t r, uint32_t color);
    void (*fill_circle)(int16_t cx, int16_t cy, uint16_t r, uint32_t color);
    /*
     * Stateful convex polygon (CC-friendly, no pointer args):
     * poly_begin → poly_vertex… → poly_fill / poly_stroke.
     */
    void (*poly_begin)(void);
    ag_err_t (*poly_vertex)(int16_t x, int16_t y);
    void (*poly_fill)(uint32_t color);
    void (*poly_stroke)(uint32_t color);
    void (*fill_convex)(const ag_point_t *pts, int32_t n, uint32_t color);
    void (*stroke_convex)(const ag_point_t *pts, int32_t n, uint32_t color);

    /* ABI 0.16: soft-draw clip + rectangles (RGB 0x00RRGGBB). Require acquire. */
    void (*clip)(int16_t x, int16_t y, uint16_t w, uint16_t h);
    void (*clip_reset)(void);
    void (*stroke_rect)(int16_t x, int16_t y, uint16_t w, uint16_t h,
                        uint32_t color);
    void (*fill_round_rect)(int16_t x, int16_t y, uint16_t w, uint16_t h,
                            uint16_t r, uint32_t color);

    /*
     * ABI 0.17: chroma-key blit.  Soft path supports RGB565 / RGB565_BE.
     * key_rgb is 0x00RRGGBB (same as fill colours); matching source pixels
     * are skipped.  Native one-shot; Argon CC uses blit_bind + blit_keyed
     * instead (call-arg limit).
     */
    void (*blit_key)(int16_t x, int16_t y, uint16_t w, uint16_t h,
                     const void *src, uint32_t src_stride, ag_pixfmt_t src_fmt,
                     uint32_t key_rgb);
    /*
     * Stateful RGB565 (LE) blit for CC: blit_bind → blit_copy / blit_keyed.
     * blit_keyed key is 0x00RRGGBB.
     */
    void (*blit_bind)(const void *src, uint32_t src_stride);
    void (*blit_copy)(int16_t x, int16_t y, uint16_t w, uint16_t h);
    void (*blit_keyed)(int16_t x, int16_t y, uint16_t w, uint16_t h,
                       uint32_t key_rgb);
    /*
     * ABI 0.25: 8×16 text in at most `w` pixels (one line; `\n` ends).
     * If the string is wider, the tail is replaced with "...".  Same fg/bg
     * / AG_GFX_TRANS rules as text().  Returns advance actually drawn.
     */
    int32_t (*text_fit)(int16_t x, int16_t y, uint16_t w, const char *s,
                        uint32_t fg, uint32_t bg);
    /*
     * ABI 0.26: source rectangle in the buffer from blit_bind (RGB565 LE).
     * blit_scaled stretches it nearest-neighbour into the dest rect.
     * blit_tiled repeats it (modulo) across the dest rect.
     */
    void (*blit_src_rect)(int16_t sx, int16_t sy, uint16_t sw, uint16_t sh);
    void (*blit_scaled)(int16_t dx, int16_t dy, uint16_t dw, uint16_t dh);
    void (*blit_tiled)(int16_t dx, int16_t dy, uint16_t dw, uint16_t dh);
    /*
     * UV in source pixels for the next poly_vertex.  poly_fill_tex fills the
     * convex polygon by affine-mapping the bound blit_src_rect.  Vertices
     * without poly_uv get a bounding-box map onto that rect.
     */
    void (*poly_uv)(int16_t u, int16_t v);
    void (*poly_fill_tex)(void);
} ag_gfx_api_t;

/*
 * Class vtable for AG_DEV_DISPLAY devices, returned by dev->ops(h).
 * Compact subset of ag_gfx_api_t keyed by the open handle — for drivers that
 * publish a panel as /dev/name rather than (or as well as) the global gfx
 * singleton.  Soft fb0 implements this; api->gfx remains the convenience path.
 */
typedef struct ag_display_ops {
    uint32_t size;

    /* Geometry/format without taking the display; out->fb is NULL. */
    ag_err_t (*info)(ag_handle_t h, ag_gfxinfo_t *out);
    ag_err_t (*acquire)(ag_handle_t h, ag_gfxinfo_t *out);
    void (*release)(ag_handle_t h);
    void (*flush)(ag_handle_t h, uint16_t x, uint16_t y, uint16_t w,
                  uint16_t hgt);
    void (*swap)(ag_handle_t h);
} ag_display_ops_t;

/* ------------------------------------------------------------------------ */
/* dev - device manager                                                     */
/* ------------------------------------------------------------------------ */

typedef enum {
    AG_DEV_ANY = 0,
    AG_DEV_BUS,
    AG_DEV_BLOCK,
    AG_DEV_CHAR,
    AG_DEV_DISPLAY,
    AG_DEV_INPUT,
    AG_DEV_SENSOR,
    AG_DEV_NET,
    AG_DEV_GPIO,
    AG_DEV_AUDIO,
    AG_DEV_STORAGE,
    AG_DEV_MOTOR,
} ag_dev_class_t;

enum ag_dev_flags {
    AG_DEVF_EXCLUSIVE = 1u << 0, /* one holder at a time; second open is EBUSY */
    AG_DEVF_HOTPLUG = 1u << 1,   /* can disappear while open                   */
    AG_DEVF_DMA = 1u << 2,       /* transfers can go straight to DMA memory    */
    AG_DEVF_READONLY = 1u << 3,  /* opening for writing fails with EROFS       */
    AG_DEVF_BUSY = 1u << 4,      /* reported by enumerate: somebody has it open */
};

/* Same width as the name field of ag_devinfo_t; a path component in /dev. */
#define AG_DEV_NAME_MAX 24

typedef struct {
    char           name[AG_DEV_NAME_MAX];
    char           driver[AG_DEV_NAME_MAX];
    ag_dev_class_t cls;
    uint32_t       flags;
} ag_devinfo_t;

/*
 * ioctl command numbers.  The class is in the high half, so a command meant for
 * a disk that reaches a sensor fails instead of meaning something else there.
 * AG_DEV_ANY commands are the ones every device answers.
 */
#define AG_IOC(cls, nr) ((uint32_t)(((uint32_t)(cls) << 16) | (uint16_t)(nr)))

enum ag_ioctl_cmd {
    AG_IOC_INFO = AG_IOC(AG_DEV_ANY, 1),  /* arg: ag_devinfo_t              */
    AG_IOC_RESET = AG_IOC(AG_DEV_ANY, 2), /* arg: NULL                      */
    AG_IOC_FLUSH = AG_IOC(AG_DEV_ANY, 3), /* arg: NULL                      */

    AG_IOC_GEOMETRY = AG_IOC(AG_DEV_STORAGE, 1), /* arg: ag_geometry_t      */

    /* PCM devices (/dev/pcmnull, loadable pcmvirt, …): arg ag_audio_fmt_t */
    AG_IOC_AUDIO_GETFMT = AG_IOC(AG_DEV_AUDIO, 1),
    AG_IOC_AUDIO_SETFMT = AG_IOC(AG_DEV_AUDIO, 2),
    /* Optional: arg ag_audio_stats_t (pcmvirt/pcmmix; pcmnull returns zeros). */
    AG_IOC_AUDIO_GETSTATS = AG_IOC(AG_DEV_AUDIO, 3),
};

typedef struct {
    uint32_t sector_size; /* smallest unit the media reads and writes      */
    uint64_t sectors;
} ag_geometry_t;

/*
 * Opaque here; the kernel's struct starts with the fields a driver needs, and
 * get_priv() is how a .SYS reaches the pointer it passed to add().  Built-in
 * drivers see the full struct through the kernel header.
 */
typedef struct ag_device ag_device_t;

/*
 * What a driver supplies for one device.  Any entry may be NULL; the registry
 * then answers -AG_ENOTSUP rather than crashing.  Called with the registry lock
 * held - do not call back into add/remove or the filesystem from here.
 */
typedef struct ag_dev_ops {
    ag_err_t (*open)(ag_device_t *dev, uint32_t flags);
    ag_err_t (*close)(ag_device_t *dev);
    int32_t (*read)(ag_device_t *dev, void *buf, size_t len, uint64_t off);
    int32_t (*write)(ag_device_t *dev, const void *buf, size_t len,
                     uint64_t off);
    ag_err_t (*ioctl)(ag_device_t *dev, uint32_t cmd, void *arg, size_t arglen);
    uint64_t (*size)(ag_device_t *dev);
    /*
     * ABI 0.22: optional per-open session.  When open_session is non-NULL the
     * filesystem open path uses these instead of open/close/read/write/ioctl,
     * so several holders can each keep private state (e.g. mixer client rings).
     * Older drivers leave these NULL and keep the classic single-state path.
     */
    ag_err_t (*open_session)(ag_device_t *dev, uint32_t flags, void **session);
    ag_err_t (*close_session)(ag_device_t *dev, void *session);
    int32_t (*read_session)(ag_device_t *dev, void *session, void *buf,
                            size_t len, uint64_t off);
    int32_t (*write_session)(ag_device_t *dev, void *session, const void *buf,
                             size_t len, uint64_t off);
    ag_err_t (*ioctl_session)(ag_device_t *dev, void *session, uint32_t cmd,
                              void *arg, size_t arglen);
} ag_dev_ops_t;

/*
 * How a .SYS publishes a device from ag_driver_init.  The owner is filled in by
 * the loader: unload of that module revokes everything it added.  add() outside
 * of ag_driver_init is refused - an ordinary application is not a driver.
 */
typedef struct {
    const char         *name;   /* required, unique, no path separators     */
    const char         *driver; /* who implements it, for `dev` and the log */
    ag_dev_class_t      cls;
    uint32_t            flags;
    const ag_dev_ops_t *ops;
    const void         *class_ops;
    void               *priv;
} ag_dev_add_t;

/*
 * What matched when the module was loaded by probe, not by an explicit
 * `drv load`.  NULL outside ag_driver_init, and NULL for a forced load.
 * The driver uses bus/addr to talk to the chip; when has_id is set the kernel
 * already checked the ID register before loading, so the match is real.
 */
typedef struct {
    int     bus;    /* I2C bus number as in BOARD.CFG / io             */
    uint8_t addr;   /* 7-bit address that answered                     */
    bool    has_id; /* kernel verified id_reg reads as id_val          */
    uint8_t id_reg;
    uint8_t id_val;
} ag_probe_hint_t;

/*
 * A device is a byte addressable object with a name, and the handle carries the
 * position - so fs->read, fs->write and fs->seek work on "/dev/sd0" exactly as
 * they do on a file, and `copy` needs no special case for a device.  This
 * sub-table is the other way in: it opens by bare name, and it carries the two
 * things a file has no room for, ioctl and the class vtable.
 *
 * 0.6 appended add / remove / get_priv for loadable drivers.
 * 0.7 appended probe_hint for probe-driven loads.
 */
typedef struct ag_dev_api {
    uint32_t size;

    /* Iterate devices; index from 0 until AG_ENOENT. */
    ag_err_t (*enumerate)(uint32_t index, ag_dev_class_t filter,
                          ag_devinfo_t *out);
    ag_handle_t (*open)(const char *name);
    ag_err_t (*close)(ag_handle_t h);
    int32_t (*read)(ag_handle_t h, void *buf, size_t len);
    int32_t (*write)(ag_handle_t h, const void *buf, size_t len);
    ag_err_t (*ioctl)(ag_handle_t h, uint32_t cmd, void *arg, size_t arglen);
    /* Class specific operations table, or NULL. */
    const void *(*ops)(ag_handle_t h);

    /* Publish / withdraw a device.  Only legal inside ag_driver_init. */
    ag_err_t (*add)(const ag_dev_add_t *desc);
    ag_err_t (*remove)(const char *name);
    /* The priv pointer the driver passed to add(), for use inside ops. */
    void *(*get_priv)(ag_device_t *dev);

    /* Bus/address that matched, or NULL when this load was not from probe. */
    const ag_probe_hint_t *(*probe_hint)(void);
} ag_dev_api_t;

/* ------------------------------------------------------------------------ */
/* io - direct hardware access (full trust, DOS style)                      */
/* ------------------------------------------------------------------------ */

enum ag_gpio_mode {
    AG_GPIO_IN = 0,
    AG_GPIO_OUT,
    AG_GPIO_OUT_OD,
    AG_GPIO_IN_PULLUP,
    AG_GPIO_IN_PULLDOWN,
};

enum ag_gpio_edge { AG_EDGE_RISING = 1, AG_EDGE_FALLING = 2, AG_EDGE_BOTH = 3 };

typedef void (*ag_isr_fn)(void *arg);

typedef struct ag_io_api {
    uint32_t size;

    ag_err_t (*gpio_config)(int pin, int mode);
    void (*gpio_write)(int pin, int level);
    int (*gpio_read)(int pin);
    ag_err_t (*gpio_isr)(int pin, int edge, ag_isr_fn fn, void *arg);
    ag_err_t (*gpio_isr_clear)(int pin);

    ag_err_t (*i2c_write)(int bus, uint8_t addr, const void *buf, size_t len,
                          uint32_t timeout_ms);
    ag_err_t (*i2c_read)(int bus, uint8_t addr, void *buf, size_t len,
                         uint32_t timeout_ms);
    ag_err_t (*i2c_wrrd)(int bus, uint8_t addr, const void *wbuf, size_t wlen,
                         void *rbuf, size_t rlen, uint32_t timeout_ms);
    ag_err_t (*i2c_probe)(int bus, uint8_t addr);

    ag_err_t (*spi_xfer)(int bus, int cs, const void *tx, void *rx, size_t len);

    int32_t (*uart_write)(int port, const void *buf, size_t len);
    int32_t (*uart_read)(int port, void *buf, size_t len, uint32_t timeout_ms);
    ag_err_t (*uart_config)(int port, uint32_t baud, int databits, int parity,
                            int stopbits);

    int32_t (*adc_read)(int channel);
    ag_err_t (*pwm_config)(int pin, uint32_t freq_hz, uint8_t resolution_bits);
    ag_err_t (*pwm_set)(int pin, uint32_t duty);
} ag_io_api_t;

/* ------------------------------------------------------------------------ */
/* time                                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint16_t year;
    uint8_t  month;  /* 1..12 */
    uint8_t  day;    /* 1..31 */
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  weekday; /* 0 = Sunday */
} ag_datetime_t;

typedef struct ag_time_api {
    uint32_t size;

    ag_time_t (*us)(void);  /* microseconds since boot                     */
    uint32_t (*ms)(void);   /* milliseconds since boot                     */
    uint64_t (*cycles)(void);

    void (*delay_ms)(uint32_t ms);
    void (*delay_us)(uint32_t us); /* busy-wait below ~1 ms                */

    ag_err_t (*get_datetime)(ag_datetime_t *out);
    ag_err_t (*set_datetime)(const ag_datetime_t *dt);

    /* Periodic callback on a kernel timer task; returns a handle. */
    ag_handle_t (*timer_create)(uint32_t period_us, bool periodic,
                                void (*fn)(void *), void *arg);
    ag_err_t (*timer_delete)(ag_handle_t h);
} ag_time_api_t;

/* ------------------------------------------------------------------------ */
/* task - threads and synchronisation inside a process                      */
/* ------------------------------------------------------------------------ */

typedef void *ag_thread_t;
typedef void *ag_mutex_t;
typedef void *ag_sem_t;
typedef void *ag_queue_t;

enum ag_thread_flags {
    AG_THREAD_APP_CORE = 0,      /* run on the application core (default)   */
    AG_THREAD_SYS_CORE = 1u << 0,/* run on the system core                  */
    AG_THREAD_ANY_CORE = 1u << 1,
};

typedef struct ag_task_api {
    uint32_t size;

    ag_thread_t (*create)(void (*fn)(void *), void *arg, const char *name,
                          size_t stack, int priority, uint32_t flags);
    void (*exit)(void);
    ag_err_t (*join)(ag_thread_t t, uint32_t timeout_ms);
    void (*yield)(void);
    void (*sleep_ms)(uint32_t ms);
    ag_thread_t (*self)(void);

    ag_mutex_t (*mutex_create)(void);
    void (*mutex_delete)(ag_mutex_t m);
    bool (*mutex_lock)(ag_mutex_t m, uint32_t timeout_ms);
    void (*mutex_unlock)(ag_mutex_t m);

    ag_sem_t (*sem_create)(uint32_t initial, uint32_t max);
    void (*sem_delete)(ag_sem_t s);
    bool (*sem_take)(ag_sem_t s, uint32_t timeout_ms);
    void (*sem_give)(ag_sem_t s);

    ag_queue_t (*queue_create)(uint32_t items, size_t item_size);
    void (*queue_delete)(ag_queue_t q);
    bool (*queue_send)(ag_queue_t q, const void *item, uint32_t timeout_ms);
    bool (*queue_recv)(ag_queue_t q, void *item, uint32_t timeout_ms);

    /* Disable preemption on the current core; keep it short. */
    void (*critical_enter)(void);
    void (*critical_exit)(void);
} ag_task_api_t;

/* ------------------------------------------------------------------------ */
/* proc - processes                                                         */
/* ------------------------------------------------------------------------ */

typedef enum {
    AG_PS_LOADING = 0,
    AG_PS_RUNNING,
    AG_PS_BACKGROUND,
    AG_PS_STOPPED,
    AG_PS_ZOMBIE,
} ag_proc_state_t;

typedef struct {
    ag_pid_t        pid;
    char            name[32];
    ag_proc_state_t state;
    bool            foreground;
    size_t          mem_used;
    uint32_t        cpu_permille; /* rough CPU share, 0..1000              */
    ag_time_t       started;
    /* 0=low, 1=normal, 2=high — see ag_proc_prio_t in argon/proc.h */
    uint8_t         priority;
} ag_procinfo_t;

enum ag_spawn_flags {
    AG_SPAWN_FOREGROUND = 0,
    AG_SPAWN_BACKGROUND = 1u << 0,
    AG_SPAWN_RESIDENT = 1u << 1, /* TSR: stays loaded after ag_main returns */
    AG_SPAWN_NO_CONSOLE = 1u << 2,
    /* Caller will ag_session_bind_to(); do not auto-bind (avoids wiping parked builtins). */
    AG_SPAWN_NO_SESSION = 1u << 3,
};

typedef struct ag_proc_api {
    uint32_t size;

    /* Load and run; returns the child's exit code (blocking). */
    int32_t (*exec)(const char *path, int argc, const char **argv);
    /* Load and run asynchronously; returns pid. */
    ag_pid_t (*spawn)(const char *path, int argc, const char **argv,
                      uint32_t flags);
    ag_err_t (*wait)(ag_pid_t pid, int32_t *exit_code, uint32_t timeout_ms);
    ag_err_t (*kill)(ag_pid_t pid);
    ag_pid_t (*self)(void);
    ag_err_t (*enumerate)(uint32_t index, ag_procinfo_t *out);
    ag_err_t (*foreground)(ag_pid_t pid);

    const char *(*getenv)(const char *key);
    ag_err_t (*setenv)(const char *key, const char *value);

    /*
     * True once, when the system has asked this process to stop - Ctrl+C, or
     * another process being polite about it.  Reading it clears it.
     *
     * An application that checks this between pieces of work is one that can be
     * asked to stop and tidy up after itself.  One that does not is one that has
     * to be killed, which works but throws away whatever it was in the middle
     * of.  Long loops should check it; short programs need not bother.
     */
    bool (*interrupted)(void);

    /*
     * Arms a deadline for this process: if it does not call sys->heartbeat()
     * within `ms`, the supervisor treats it as hung and stops it.  0 disarms.
     *
     * Opt-in, and deliberately so.  A deadline the system imposed would be wrong
     * for every application that legitimately waits a long time - for a key, for
     * a card, for a reply - and being killed for waiting is worse than not being
     * watched.  An application that arms this is saying it knows how long its own
     * work takes, which is a promise only it can make.
     */
    void (*watchdog)(uint32_t ms);

    /*
     * ABI 0.20: true while this process's session slot has keyboard/display
     * focus.  Outside focus, apps should not flush/swap or run heavy redraw.
     */
    bool (*focused)(void);
} ag_proc_api_t;

/* ------------------------------------------------------------------------ */
/* cfg - system / board configuration access                                */
/* ------------------------------------------------------------------------ */

typedef struct ag_cfg_api {
    uint32_t size;

    /* Keys use "section.item" notation, e.g. "display.rotation". */
    ag_err_t (*get_str)(const char *key, char *buf, size_t len);
    int32_t (*get_int)(const char *key, int32_t def);
    bool (*get_bool)(const char *key, bool def);
    ag_err_t (*set_str)(const char *key, const char *value);
    ag_err_t (*commit)(void);
} ag_cfg_api_t;

/* ------------------------------------------------------------------------ */
/* net - TCP sockets (OpenEth in QEMU today; Wi-Fi later on hardware)       */
/* ------------------------------------------------------------------------ */

/*
 * Handles are opaque and distinct from filesystem handles.  Close them with
 * net->close, not fs->close.  IPv4 addresses and ports are host byte order.
 * timeout_ms of UINT32_MAX means "wait forever"; 0 means "return at once"
 * (typically -AG_EAGAIN when nothing is ready).
 */
typedef struct ag_net_api {
    uint32_t size;

    bool (*ready)(void);
    ag_err_t (*wait_ready)(uint32_t timeout_ms);
    ag_err_t (*ifaddr)(uint32_t *addr_out); /* host-order IPv4 */

    ag_handle_t (*tcp_listen)(uint16_t port);
    ag_handle_t (*tcp_accept)(ag_handle_t listen, uint32_t timeout_ms);
    ag_handle_t (*tcp_connect)(uint32_t addr, uint16_t port,
                               uint32_t timeout_ms);

    int32_t (*send)(ag_handle_t sock, const void *buf, size_t len);
    int32_t (*recv)(ag_handle_t sock, void *buf, size_t len);
    ag_err_t (*close)(ag_handle_t sock);

    /* ABI 0.13: O_NONBLOCK.  send/recv then return -AG_EAGAIN instead of stalling. */
    ag_err_t (*set_nonblock)(ag_handle_t sock, bool on);
} ag_net_api_t;

/* ------------------------------------------------------------------------ */
/* audio - PCM output (built-in pcmnull discard; I2S/virt via .SYS)         */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint32_t rate;     /* Hz, typically 22050 */
    uint8_t  channels; /* 1 or 2 */
    uint8_t  bits;     /* 16 */
} ag_audio_fmt_t;

/* Cumulative stream accounting for virtual/HW PCM sinks (GETSTATS). */
typedef struct {
    uint64_t bytes_in;           /* accepted from write() */
    uint64_t bytes_sent;         /* delivered to host/HW */
    uint64_t bytes_drop_noclient;/* no peer / not open */
    uint64_t bytes_drop_overflow;/* ring/TCP backpressure */
    uint32_t eagain_events;      /* send returned EAGAIN */
    uint32_t overflow_events;    /* ring overflow discards */
    uint32_t ring_used;          /* bytes pending in driver ring */
    uint32_t ring_cap;           /* ring capacity */
} ag_audio_stats_t;

typedef struct ag_audio_api {
    uint32_t size;

    /* Always 1 after devices init (built-in /dev/pcmnull). */
    int (*present)(void);
    /* 1 only when a real hardware backend is open (never for pcmnull). */
    int (*is_hw)(void);

    /* Exclusive open of the built-in null sink; fmt NULL → board rate stereo s16. */
    ag_err_t (*open)(const ag_audio_fmt_t *fmt);
    void (*close)(void);

    /*
     * Interleaved signed PCM.  `frames` is sample-frames (stereo L+R = 1).
     * Returns frames accepted, or a negative -AG_Exxx.  May drop on overrun
     * rather than block the emulator for long.
     */
    int32_t (*write)(const int16_t *pcm, int32_t frames);
    /* Best-effort free space in frames. */
    int32_t (*space)(void);
} ag_audio_api_t;

/* ------------------------------------------------------------------------ */
/* Root table                                                               */
/* ------------------------------------------------------------------------ */

typedef struct ag_api {
    uint32_t size;
    uint16_t abi_major;
    uint16_t abi_minor;

    const ag_sys_api_t  *sys;
    const ag_mem_api_t  *mem;
    const ag_fs_api_t   *fs;
    const ag_con_api_t  *con;
    const ag_inp_api_t  *inp;
    const ag_gfx_api_t  *gfx;
    const ag_dev_api_t  *dev;
    const ag_io_api_t   *io;
    const ag_time_api_t *time;
    const ag_task_api_t *task;
    const ag_proc_api_t *proc;
    const ag_cfg_api_t  *cfg;

    /* NULL when the profile has no networking. */
    const ag_net_api_t *net;

    /* ABI 0.14+: PCM out (built-in pcmnull; virt/I2S via .SYS). */
    const ag_audio_api_t *audio;
} ag_api_t;

/* ------------------------------------------------------------------------ */
/* Executable image header                                                  */
/* ------------------------------------------------------------------------ */

#define AG_AXE_MAGIC 0x31455841u /* 'AXE1' little endian */

enum ag_axe_flags {
    AG_AXE_HOT_TEXT = 1u << 0,   /* .text must live in internal SRAM        */
    AG_AXE_NEEDS_GFX = 1u << 1,  /* refuses to start without a display      */
    AG_AXE_NEEDS_NET = 1u << 2,
    AG_AXE_DRIVER = 1u << 3,     /* .SYS module, entry is ag_driver_init    */
    AG_AXE_RESIDENT = 1u << 4,
    /*
     * Set by the build tool, not by the application: the two parts of the image
     * must be placed adjacent, data immediately after code, and share one bias.
     * Needed where code reaches its data PC-relatively - RISC-V does, with the
     * medany code model - so the distance between the parts cannot change.
     */
    AG_AXE_CONTIGUOUS = 1u << 5,
    AG_AXE_NEEDS_AUDIO = 1u << 6, /* refuses to start without api->audio    */
};

/*
 * Emitted by the SDK into every application image under the symbol
 * `__ag_app_header`.  The loader reads it before applying relocations.
 */
typedef struct {
    uint32_t magic;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;
    uint32_t stack_size;   /* 0 = kernel default                           */
    uint32_t heap_size;    /* 0 = kernel default                           */
    char     name[32];
    char     version[16];
    char     author[32];
    uint32_t reserved[8];  /* future: signature offset, resource offset     */
} ag_app_header_t;

/*
 * Every process gets this pointer installed before ag_main() is called.
 * Applications should use the inline wrappers in <argon/argon.h> instead of
 * touching it directly.
 */
extern const ag_api_t *g_ag_api;

static inline const ag_api_t *ag_api(void) { return g_ag_api; }

/* Application entry point, provided by the application. */
int ag_main(int argc, char **argv);

/* Driver module entry point, provided by .SYS modules. */
typedef ag_err_t (*ag_driver_init_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_ABI_H */
