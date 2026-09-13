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
 *      the bound source) and poly_uv / poly_fill_tex (bilinear UV on quads,
 *      affine triangles otherwise).
 * 0.27 appended text_info / text_row / text_cursor to ag_display_ops_t: the
 *      console as characters, for a panel a framebuffer will not fit on.
 * 0.28 defined ag_input_ops_t: an AG_DEV_INPUT driver the kernel polls for
 *      events, for hardware that has to be asked rather than interrupting.
 * 0.29 appended io->spi_config: the clock for one chip on a shared bus.
 * 0.44 appended units / span_w / span_h to ag_input_ops_t: an input driver
 *      that measures in pixels rather than console cells says so, and the
 *      kernel scales its span to the surface instead of multiplying a cell.
 *      A touchscreen under a graphical shell loses eight pixels in nine
 *      otherwise, and has to agree with the kernel about whose grid it is.
 * 0.30 appended blit_rect to ag_display_ops_t: a rectangle of pixels the
 *      kernel owns, for a panel whose glass is larger than any framebuffer
 *      the machine driving it can afford.
 * 0.31 appended gfx->present: an application's own pixels on the panel, for a
 *      machine where the system framebuffer is absent or the wrong shape.
 * 0.32 appended fs->map / fs->unmap: a file's bytes at an address, read only,
 *      staged into flash - for data too big to hold and too fixed to need to.
 * 0.33 appended net->resolve: a name into an address.  Without it every
 *      connect() in an application needs a dotted quad, which is not how
 *      anything on a network is written down.
 * 0.34 added api->ble (NULL unless the build has the BLE peripheral): a small
 *      BLE-MIDI surface - advertise as a MIDI device and send notes - so a
 *      graphical app can be a MIDI controller a phone plays.
 * 0.35 made api->power stop being NULL: what the machine is set to, and a
 *      transition an application is told about before it happens.  With
 *      AG_IOC_DISPLAY_BACKLIGHT beside it, for the panel driver that owns the
 *      pin the light is on.
 * 0.36 power: a transition now says who asked for it.  An automatic one is
 *      advisory and harmless; one a person typed is an order, and a process
 *      that does not answer that it is fit for the new mode is ended.
 *      power->declare replaces power->hold, and the answer that used to veto
 *      (HOLD) is now the admission that gets a process killed (UNFIT).
 * 0.37 power: AG_POWER_CRUISE, a step down that costs a third of the speed and
 *      nothing else, taken by the system whenever no process has said it needs
 *      the full clock.  It renumbers ag_power_mode_t, which is why it is worth
 *      a line of its own: ECO and DOZE moved up by one.
 * 0.38 radio, given to applications past the one thing BLE-MIDI already gave:
 *      api->ble grows the general central and peripheral it always wrapped in
 *      the shell - observe the air, connect to a device and read/write any
 *      characteristic, or be a device a phone reads and writes (NULL entries
 *      unless the build has CONFIG_ARGON_BLE_CENTRAL / _PERIPHERAL).  And a new
 *      api->wifimon appears - promiscuous capture, channel, filter, and raw
 *      802.11 injection - NULL unless CONFIG_ARGON_NET_WIFI_MON, which is off by
 *      default: the frame-forging radio is a build-time choice, and the ABI slot
 *      follows it.  Wi-Fi station/AP control and ESP-NOW stay shell-only on
 *      purpose (see docs/04-roadmap.md): the link is the system's to raise, and
 *      an application already has TCP, DNS and its own address over it.
 * 0.39 api->wifi: the rest of the radio, past capture and injection - the same
 *      station, access point and ESP-NOW the shell drives, now an application's
 *      to drive too.  0.38 held these back "on purpose"; a graphical Wi-Fi tool
 *      is the application that changes the calculus, because it is the thing a
 *      person points at the air with, and it cannot be one if raising the link
 *      is somebody else's job.  Station (scan/connect/disconnect/status) is
 *      present whenever the build has the radio (CONFIG_ARGON_NET_WIFI); the
 *      access-point entries are NULL without CONFIG_ARGON_NET_WIFI_AP and the
 *      ESP-NOW entries NULL without CONFIG_ARGON_NET_ESPNOW - probe with AG_HAS.
 *      api->wifi itself is NULL on a board with no radio at all (QEMU).
 * 0.40 ble->adv_raw: raw, non-connectable BLE advertising - a caller-built
 *      payload under a spoofed random address, rebroadcast on every call.  It is
 *      to BLE advertising what wifimon->tx_raw is to 802.11: the injection half
 *      of the radio, for a tool that forges the advertisements a phone shows as
 *      pairing pop-ups.  NULL without CONFIG_ARGON_BLE_PERIPHERAL.
 * 0.41 api->cam: the DVP capture engine (LCD_CAM + DMA) as a primitive a
 *      loadable sensor driver drives, so the sensor's register tables live in a
 *      .SYS rather than in the image.  The firmware carries only the thin
 *      transport; the driver configures it with the sensor's pins/format and
 *      pulls frames.  NULL without CONFIG_ARGON_ENABLE_CAMERA (off by default -
 *      no camera in QEMU, and it is a board's peripheral, not the chip's).
 * 0.42 pointer events carry pixels, always, and ag_coninfo_t gained cell_w /
 *      cell_h so a text application can get back to a column and a row.
 *      Before this the same field meant cells from a terminal or a touchscreen
 *      and pixels from a virtual mouse, and an application could not tell -
 *      so graphical ones (grain, amp, doom) mishandled every tap.  The console
 *      is still a grid of cells, because that is what a text console is; what
 *      changed is that a pointer is a position on a surface and is now reported
 *      as one.  A terminal has no pixels of its own, so the kernel scales its
 *      column and row by the cell size on the way in.
 * 0.43 ag_net_ops_t: the class vtable for an AG_DEV_NET device, and the first
 *      time dev->ops() means anything for a class other than a display.  A .SYS
 *      that speaks to an external radio coprocessor (Wi-Fi, GSM/LTE...) over a
 *      wire publishes it, and the kernel routes its whole socket path through
 *      it when the operator says `net use <dev>` - so a board can carry no
 *      built-in radio at all and still reach the network, the radio being a
 *      chip on a bus like any other .SYS.  One provider is active at a time; the
 *      built-in stack is the default.  Append-only: nothing an existing image
 *      calls has moved.
 * 0.47 the two lines above, merged.  0.43's ag_net_ops_t came off the radio
 *      line and 0.44-0.46 - input units, another slot's screen and its
 *      cursor, a prompt in a slot of its own - off the desktop line, and
 *      each line had bumped this number for itself while the other was
 *      being written.  An image built from this carries all of them, so it
 *      needs a number larger than either, and that is all 0.47 means.
 *
 *      The numbers were handed out in parallel and some are spoken for
 *      twice: worktree-cluster calls its own additions 0.47 and
 *      worktree-usb-kvm calls its 0.45.  Whichever of those lands next has
 *      to RENUMBER rather than assume its number is free - and the check is
 *      one line: `git show <branch>:sdk/include/argon/abi.h | grep MINOR`.
 * 0.48 sys->module_task: a task that belongs to a loadable driver rather than
 *      to a process, so a driver can do work that takes milliseconds without
 *      charging it to whoever called in.  Until this, every driver was a
 *      collection of callbacks on the caller's thread, and a screen at the end
 *      of a wire made the game that drew a frame wait for the frame to go out.
 *      The module owns the task and unload waits for it.
 *
 *      This is the renumbering the paragraph above asks for: the work is
 *      commit 2ec139a on `worktree-fallout-cxx`, where it was written as 0.43
 *      and board-verified in that shape, and 0.43 was spoken for here by
 *      ag_net_ops_t before it could land.  Nothing about it changed but the
 *      number.
 */
#define AG_ABI_MAJOR 0u
#define AG_ABI_MINOR 48u

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

    /*
     * ABI 0.45: give a session slot a shell of its own, and say which slot
     * got it.
     *
     * For a desktop that wants a prompt in a window.  `slot` below zero
     * means any free one, which is what a window asking for a prompt
     * actually wants - it has no business choosing a slot number, and
     * nothing else in the ABI lets it enumerate them.  `cwd` may be NULL
     * for the root.
     *
     * Returns the slot, or a negative error: -AG_ENOSPC when every slot
     * already has something in it.
     */
    int32_t (*prompt_in_slot)(int slot, const char *cwd);

    /*
     * ABI 0.48: during ag_driver_init only — a task of the module's own.
     *
     * task->create is the application's, and it refuses a caller with no
     * process: a thread belongs to a process and dies with it, and a driver has
     * no process to belong to.  So every driver in this system has been a
     * collection of callbacks running on whoever called in - which is right for
     * a register write and wrong for anything that takes milliseconds.  A
     * screen at the end of a serial wire is the case that forced this: three
     * hundred milliseconds of a frame going out, charged to the game that drew
     * it, when the wire could just as well be fed from a task of its own.  A
     * screen at the end of a network is the same case again, with an accept()
     * and an HTTP request in it.
     *
     * The module owns it.  Unload calls the module_on_unload hook first - which
     * is where the driver tells its task to stop - and then waits for the task
     * to return before the image is unmapped, because unmapping code a task is
     * still inside is a fault that names the wrong culprit.  A task that will
     * not return refuses the unload rather than taking the machine down.
     *
     * The task must therefore watch a flag and return.  Do not call exit.
     *
     * `flags` is ag_thread_flags, the same three an application's thread gets,
     * and the choice is not cosmetic: the foreground application is pinned to
     * the app core, so a driver doing real work there is a second runnable task
     * on the one core already busy.  A driver that takes milliseconds asks for
     * AG_THREAD_SYS_CORE.  `priority` at 5 or below cannot starve an
     * application; above that it can, and nothing in this build says so.
     */
    bool (*module_task)(void (*fn)(void *), void *arg, const char *name,
                        uint32_t stack, int priority, uint32_t flags);
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

    /*
     * ABI 0.32: a file's bytes at an address, read only.
     *
     * For data that is too big to hold in memory and never changes: a cartridge,
     * a font, a table of impulse responses.  The bytes are staged into flash and
     * mapped where the processor's own cache fetches them, so the cost in memory
     * is nothing - which on a board with sixty kilobytes free is the difference
     * between running a half-megabyte cartridge and not.
     *
     * Read only.  Writing through the pointer faults; it does not change
     * anything and it does not touch the file.
     *
     * Not free and not lazy: map() copies the whole file into flash before it
     * returns, so it costs as long as writing that much flash and wears it a
     * little.  Worth it for something read a great many times, not for a
     * configuration file.  A second call after unmap copies again.
     *
     * -AG_ENFILE when too many mappings are live, -AG_ENOSPC when the flash area
     * cannot hold it, -AG_ENOSYS on a machine with nowhere to stage it.
     */
    ag_err_t (*map)(const char *path, const void **out, uint64_t *out_len);
    ag_err_t (*unmap)(const void *ptr);
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

/*
 * One cell of a text screen: the byte and its CGA attribute.
 *
 * Declared here rather than beside the display vtable that first needed it,
 * because the console reads its own screen back into these too (peek_row).
 */
typedef struct {
    uint8_t ch;
    uint8_t attr;
} ag_textcell_t;

/* Attribute byte, CGA-compatible layout: bg << 4 | fg. */
#define AG_ATTR(fg, bg) ((uint8_t)(((bg) << 4) | ((fg) & 0x0f)))

typedef struct {
    uint16_t cols;
    uint16_t rows;
    uint16_t cur_x;
    uint16_t cur_y;
    uint8_t  attr;
    bool     has_local_display;
    /*
     * ABI 0.42: how big one cell is, in the pixels a pointer event is measured
     * in.  Zero when there is no surface to measure against - a serial terminal
     * has no pixels at all and will never say how big its font is.
     *
     * Here because pointer events carry pixels (see AG_EV_POINTER_* below) and a
     * text application needs to get back to a column and a row.  It is a fact
     * about this console, published once, rather than a second coordinate
     * system: the console *is* a grid of cells - that is what ag_poke
     * addresses - while a pointer is a position on a surface.  Conflating the
     * two is what made touch land in the wrong place in graphical applications
     * for as long as it did.
     */
    uint16_t cell_w;
    uint16_t cell_h;
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

    /*
     * Read the screen back, one row at a time (ABI 0.43).
     *
     * The console draws itself, on whatever it has: a framebuffer, a panel's
     * text cells, a serial terminal.  An application that wants to show the
     * console inside a window of its own - a desktop shell with a console
     * window in it - cannot use any of those; it needs the cells.
     *
     * A row rather than the whole screen, because the caller decides what to
     * keep: eighty cells is a hundred and sixty bytes, where a screen is two
     * kilobytes that a caller redrawing one changed line would throw away.
     *
     * Returns the number of cells written (never more than `max` or the
     * screen's width), or a negative error.  Refused for a process that is not
     * the console's - the same rule ag_poke follows, and for the same reason:
     * the console belongs to the foreground.
     */
    int32_t (*peek_row)(uint16_t row, ag_textcell_t *cells, uint16_t max);

    /*
     * The same, from the screen of a session slot that is not this one.
     *
     * For a window showing somebody else's prompt.  A slot with a shell of
     * its own (`prompt 2`) has a screen of its own, and that screen is
     * nowhere on the glass while a desktop is in front - reading it is the
     * only way to draw it.
     *
     * Slots are numbered from zero here, as they are everywhere inside the
     * system; the shell writes them from one because that is what the
     * Alt+digit on the keyboard says.
     */
    /* ABI 0.45. */
    int32_t (*peek_row_slot)(int slot, uint16_t row, ag_textcell_t *cells,
                             uint16_t max);

    /*
     * Where that screen's cursor is.
     *
     * A window drawing somebody else's prompt has no way to ask: coninfo
     * answers about the CALLER's console, so a window that drew a caret from
     * it would be drawing its own cursor on another screen's text - and one
     * that drew none, as this did first, shows a prompt with nothing
     * blinking in it, which does not look like a prompt at all.
     *
     * Zero on success; the coordinates are cells.
     */
    /* ABI 0.46. */
    int32_t (*cursor_of_slot)(int slot, uint16_t *x, uint16_t *y);

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
            /*
             * Where, in the pixels of the surface an application draws into
             * (ABI 0.42).  Every source is scaled to that on the way in - a
             * mouse counts in pixels already, a touchscreen and a terminal
             * count in cells and the kernel multiplies.  A text application
             * divides by con->cell_w / cell_h to get its column and row.
             */
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

    /*
     * Hand an event to the prompt living in a session slot.
     *
     * There is one keyboard and it belongs to whoever is in front, so a
     * prompt behind a window never reads a character on its own: the
     * window in front reads them and passes them along.  That is the
     * whole of how a shell can live in a window while a desktop holds
     * the screen.
     *
     * False when that slot has no prompt of its own, or its queue is
     * full.
     */
    /* ABI 0.45. */
    bool (*post_to_slot)(int slot, const ag_event_t *ev);
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

/*
 * A rectangle of somebody else's pixels (ABI 0.30).
 *
 * The memory belongs to the caller and outlives the call; the driver reads from
 * it and returns.  Format is RGB565 in the machine's own byte order, which on
 * every part this runs on is little endian - a panel that wants the other order
 * swaps as it sends, because it is streaming the bytes anyway.
 *
 * `px` is the first pixel *of the rectangle*, not of the surface.
 *
 * That is worth being exact about, because the obvious alternative - the corner
 * of the whole picture, with x and y as an offset into it - quietly requires the
 * caller to hold the whole picture in memory.  A renderer that produces sixteen
 * rows at a time, which is what an emulator's scanline loop produces and what
 * fits on a machine like this, has no such corner to point at.  With the
 * rectangle's own origin it hands over the band it just filled and says where
 * on the screen it goes.
 *
 * Both the whole surface and the changed part are described, because a driver
 * cannot place the one without knowing the other: a screen 320 pixels wide
 * showing a surface of 160 has a choice to make about scale and margins, and it
 * has to make the same choice for every rectangle or the picture tears itself
 * apart.  So surf_w/surf_h are the picture, x/y/w/h are the part, and px is
 * where that part's pixels actually are.
 */
typedef struct {
    const void *px;     /* first pixel of the rectangle, RGB565              */
    uint32_t    stride; /* bytes between rows of px                          */
    uint16_t    surf_w; /* the whole picture, for placement                  */
    uint16_t    surf_h;
    uint16_t    x, y, w, h; /* where the rectangle is in it, and how big     */
} ag_blit_t;

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
     * convex polygon from the bound blit_src_rect: a quad is one bilinear map
     * (no triangle-fan seam); n!=4 is a fan of affine triangles.  Vertices
     * without poly_uv get a bounding-box map onto that rect.
     */
    void (*poly_uv)(int16_t u, int16_t v);
    void (*poly_fill_tex)(void);

    /*
     * ABI 0.31: the application's own pixels, straight onto the panel.
     *
     * acquire() hands out the system's framebuffer - one surface, shared by
     * every application, sized once at boot for all of them.  That is the right
     * arrangement until the surface is a fifth of all the memory there is: an
     * emulator with a 160x144 screen, sixteen kilobytes of video memory and a
     * cartridge to hold cannot pay for the system's surface as well as its own,
     * and the system's is the one it cannot use.
     *
     * So it may bring its own.  The caller owns the memory, says how big the
     * whole picture is and which part of it changed; the panel driver places
     * it, at whole-number scale, centred (see blit_rect in ag_display_ops_t).
     * Nothing is copied on the way - the pixels go from the caller's buffer to
     * the panel - so the buffer must not be a stack local that has gone by the
     * time this returns, and it does not return until they are sent.
     *
     * The display still has to be acquired: this says who owns the pixels, not
     * who owns the screen.  Where the system has no surface at all
     * ([display] driver = panel), acquire() still works and reports fb = NULL
     * with the panel's own size, and this is then the only way to draw.
     *
     * -AG_ENODEV when nothing can show it, -AG_EPERM when the display belongs
     * to somebody else.
     */
    ag_err_t (*present)(const ag_blit_t *b);
} ag_gfx_api_t;

/*
 * Class vtable for AG_DEV_INPUT devices that have to be asked (ABI 0.28).
 *
 * A touchscreen on a slow bus does not interrupt with an event; it holds a
 * voltage, and somebody has to read it.  So the kernel asks, from the console
 * tick it already runs, rather than each such driver keeping a task to sleep
 * and wake on a clock.  (When this was written a .SYS had no way to own a task
 * at all; since 0.48 it has - sys->module_task - and that is the right answer
 * for a driver whose work takes milliseconds.  A device that only needs looking
 * at ten times a second is still better served here.)
 *
 * poll() fills at most `max` events and returns how many, or a negative error.
 * Zero is the normal answer and must be cheap: it is called ten times a second
 * forever.  The events go into the same queue the terminal decoder feeds, so
 * nothing above can tell a finger from a mouse on the other end of a cable.
 *
 * Coordinates are console cells by default, like the terminal's mouse
 * reports, because what is on that kind of screen is the console - and a
 * driver written before ABI 0.44 says nothing about units, so cells is what
 * silence means.
 *
 * There is now something to point at, which is why the other convention
 * exists (0.44): the CYD runs a graphical shell on a panel with no system
 * framebuffer, and a touch controller that reports cells throws away eight
 * pixels in nine before anybody sees it.  Worse, the round trip has to agree
 * with the kernel about which grid - the console's, not the panel's - and
 * when it did not, the pointer sat below the stylus and the bottom of the
 * glass could not be pressed at all.  A driver that measures in pixels says
 * so and hands over pixels, and no grid is involved.
 *
 *   .units  AG_PTR_CELLS (0, and what an older driver means by saying
 *           nothing) or AG_PTR_PIXELS.
 *   .span_w Pixels across and down of whatever the driver measured against -
 *   .span_h its own glass.  The kernel scales that to the surface, so a touch
 *           panel and a screen of different sizes still agree.  Zero means
 *           "the same as the surface", which is the usual case.
 */
typedef struct ag_input_ops {
    uint32_t size;
    int32_t (*poll)(ag_handle_t h, ag_event_t *out, uint32_t max);
    /* ABI 0.44 */
    uint16_t units;
    uint16_t span_w;
    uint16_t span_h;
} ag_input_ops_t;

enum ag_ptr_units {
    AG_PTR_CELLS = 0,
    AG_PTR_PIXELS = 1,
};

/*
 * One cell of the text console: the byte on the screen and its colours, high
 * nibble background, low nibble foreground, as they have been since CGA.  What
 * the byte means is the code page's business (con->codepage).
 */
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

    /*
     * ABI 0.27: the console as characters rather than as pixels.
     *
     * A panel on a chip with no PSRAM does not get a framebuffer: 320x240 in
     * RGB565 is 150 KB, and the largest block of memory this class of board
     * has free is around a hundred.  The whole of acquire/flush/swap above
     * assumes a surface that does not exist there.  So the kernel sends the
     * console the other way - as the cells that changed - and the driver
     * turns them into pixels a row at a time, with a buffer the size of one
     * row of text and no more.
     *
     * The kernel calls these from the console tick, on the console task, with
     * `h` set to zero: it holds the device, not an open handle.  A driver that
     * has a framebuffer leaves all three NULL and gets the pixel path instead.
     *
     * text_info answers how many cells fit; the console is resized to that at
     * boot when the board asks for it.  text_row is called for rows that
     * changed, with `count` cells starting at column zero.  text_cursor moves
     * the caret and is called only when it has moved or blinked - it carries
     * the cell underneath, because a caret drawn as a blank would rub out the
     * character it is standing on and the driver has nowhere to look it up.
     */
    ag_err_t (*text_info)(ag_handle_t h, uint16_t *cols, uint16_t *rows);
    void (*text_row)(ag_handle_t h, uint16_t row, const ag_textcell_t *cells,
                     uint16_t count);
    void (*text_cursor)(ag_handle_t h, uint16_t col, uint16_t row,
                        ag_textcell_t under, bool visible);

    /*
     * ABI 0.30: pixels, for the same kind of panel as text_row above.
     *
     * acquire/flush/swap hand a driver a framebuffer it owns.  This is the
     * other way round and for the other kind of machine: the kernel owns the
     * surface - as large as its heap will bear, which here is a quarter of the
     * glass - and the driver is told which part of it changed.  The driver
     * decides how that surface lands on the panel; scaling it by a whole
     * number and centring what is left is what the one in tree does.
     *
     * Called on the task that flushed, with `h` zero and the device registry
     * held.  A driver with a framebuffer of its own leaves this NULL.
     *
     * Must not print, and neither must text_row or text_cursor above.  The
     * registry is held here, while the console task takes the console first
     * and the registry second - so a driver that reaches for the console from
     * inside one of these closes the ring and stops the machine.  A driver
     * with something to say says it from ag_driver_init.
     */
    void (*blit_rect)(ag_handle_t h, const ag_blit_t *b);
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
    /*
     * ABI 0.35: arg is a uint8_t, 0..100 - how bright the panel should be.
     * Zero means off, and off means as dark as this board can be made: on a
     * hand-held with a lit screen the backlight is the largest single load
     * there is, larger than the processor at full speed, so this is the one
     * call in the power path whose effect a person can see.
     *
     * A driver whose panel has no controllable light answers -AG_ENOTSUP,
     * which is not an error - it is the answer.
     */
    AG_IOC_DISPLAY_BACKLIGHT = AG_IOC(AG_DEV_DISPLAY, 1),

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

    /*
     * ABI 0.29: the clock for one chip select, overriding the bus speed from
     * BOARD.CFG.  A bus is a set of wires and the chips on it rarely agree
     * about speed: this board's panel takes 40 MHz and the touch controller
     * sharing those same three wires stops answering above about two.  Set it
     * once, before the first transfer to that chip.
     */
    ag_err_t (*spi_config)(int bus, int cs, uint32_t khz);
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
    uint64_t (*cycles)(void); /* CPU cycles of the calling core, 32-bit wrap  */

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

    /*
     * ABI 0.33: a name into a host-order IPv4 address.  A dotted quad is
     * answered without asking anything, so this is also the way to read one.
     *
     * Blocks for as long as the resolver takes - seconds, when a server is
     * slow - and there is no timeout to pass, because the stack that owns the
     * resolver does not offer one.  -AG_ENOENT for a name that does not
     * resolve, -AG_EAGAIN before the interface has an address of its own.
     */
    ag_err_t (*resolve)(const char *host, uint32_t *addr_out);
} ag_net_api_t;

/*
 * Class vtable for an AG_DEV_NET device (ABI 0.43), read by the kernel's socket
 * layer when `net use <dev>` binds this device as the active network provider.
 * A .SYS talking to an external radio coprocessor publishes it as class_ops in
 * ag_dev_add; it is not reached through dev->open/ops() by an application.
 *
 * It mirrors, function for function, what the built-in stack does - the same
 * shape as components/argon_port/include/argon/port/net.h, because the kernel
 * clients above it (net.c, netio.c, httpc.c, ftpc.c, ssh.c, ...) are written
 * against exactly that shape and must not be able to tell which provider is
 * underneath.  `dev` is the device the kernel bound; recover the driver's state
 * with dev->get_priv(dev).
 *
 * SOCKET IDS.  listen/accept/connect hand back the coprocessor's own small
 * integers; the kernel wraps them into ag_handle_t exactly as it wraps a port
 * fd.  A negative return is an -AG_E* code.  Because only one provider is active
 * at a time the id space belongs to whoever is bound, so no tagging is needed.
 *
 * THE recv_now / wait_readable SPLIT is the contract that matters: recv_now must
 * return whatever has already arrived and never block (-AG_EAGAIN when nothing
 * has), and wait_readable must answer within its timeout without a blocking
 * read underneath.  A driver over a wire keeps a per-socket receive buffer fed
 * by the coprocessor's unsolicited pushes and answers both out of it; recv()
 * (which the ABI allows to block) may loop over the same buffer.
 */
typedef struct ag_net_ops {
    uint32_t size;

    ag_err_t (*start)(ag_device_t *dev);
    bool (*ready)(ag_device_t *dev);
    ag_err_t (*ifaddr)(ag_device_t *dev, uint32_t *addr_out); /* host-order */
    ag_err_t (*resolve)(ag_device_t *dev, const char *host, uint32_t *addr_out);

    int (*listen)(ag_device_t *dev, uint16_t port);
    int (*accept)(ag_device_t *dev, int listen_fd, uint32_t timeout_ms);
    int (*connect)(ag_device_t *dev, uint32_t addr, uint16_t port,
                   uint32_t timeout_ms);

    int32_t (*send)(ag_device_t *dev, int fd, const void *buf, size_t len);
    int32_t (*recv)(ag_device_t *dev, int fd, void *buf, size_t len);
    void (*net_close)(ag_device_t *dev, int fd);
    ag_err_t (*nonblock)(ag_device_t *dev, int fd, bool on);

    int (*wait_readable)(ag_device_t *dev, int fd, uint32_t timeout_ms);
    int32_t (*recv_now)(ag_device_t *dev, int fd, void *buf, size_t len);
} ag_net_ops_t;

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
/* ble - Bluetooth Low Energy for applications (ABI 0.34)                   */
/* ------------------------------------------------------------------------ */

/*
 * BLE-MIDI was the first radio an application got (0.34), because a MIDI
 * controller was the first application that wanted one.  0.38 gives it the rest,
 * the same general central and peripheral the shell has driven and the board has
 * proven end to end: observe what is in range, connect to a device and read or
 * write any characteristic, or be a device a phone reads and writes.  The port
 * carries all of it (argon/port/ble.h); this is the appended, feature-probed
 * surface of it - entries are NULL where the build left a role out.
 */

/* How much text a name, a UUID, or one characteristic value can be. */
#define AG_BLE_NAME_MAX  31
#define AG_BLE_UUIDS_MAX 6  /* 16-bit service UUIDs kept per advertised device */
#define AG_BLE_UUID_STR  37 /* a UUID as text, 128-bit form + terminator       */
#define AG_BLE_VAL_MAX   256 /* most of one characteristic value, bytes         */

/* Characteristic properties, the bits GATT advertises about what you may do. */
#define AG_BLE_PROP_READ   0x02
#define AG_BLE_PROP_WNORSP 0x04 /* write without a response                    */
#define AG_BLE_PROP_WRITE  0x08
#define AG_BLE_PROP_NOTIFY 0x10
#define AG_BLE_PROP_INDIC  0x20

/* One discovered service: a range of handles and what it is. */
typedef struct {
    char     uuid[AG_BLE_UUID_STR];
    uint16_t start; /* first handle of the service                             */
    uint16_t end;   /* last handle of the service                              */
} ag_ble_svc_t;

/* One discovered characteristic.  `handle` is the value handle - the one
 * read()/write() take, not the declaration. */
typedef struct {
    char     uuid[AG_BLE_UUID_STR];
    uint16_t handle;
    uint8_t  props; /* AG_BLE_PROP_* bitmask                                   */
} ag_ble_chr_t;

/*
 * One device the observer saw, as much of it as the advertisement carried.  A
 * field is zero when the advertisement did not have it: no name is an empty
 * string, no appearance is 0, no manufacturer is company 0xffff.  addr_type is
 * the port's own numbering; pass it back to connect() unchanged.
 */
typedef struct {
    uint8_t  addr[6];
    int      addr_type;
    int8_t   rssi;
    bool     connectable;
    char     name[AG_BLE_NAME_MAX + 1];
    uint16_t appearance;
    uint8_t  flags;
    uint8_t  n_uuids;
    uint16_t uuids[AG_BLE_UUIDS_MAX];
    uint16_t company;
} ag_ble_dev_t;

/* The peripheral's standing, from adv_status(). */
typedef struct {
    bool     advertising;
    bool     connected; /* a client is connected right now                     */
    uint32_t writes;    /* how many writes have arrived since adv_start        */
    uint32_t read_len;  /* length of the value clients read                    */
} ag_ble_adv_status_t;

typedef struct ag_ble_api {
    uint32_t size;

    /*
     * Advertise as a BLE-MIDI device under `name` (kept short - it shares a
     * 31-byte advertisement with a 128-bit service UUID).  The radio is started
     * if it was not.  Idempotent; call again to change the name.
     */
    ag_err_t (*midi_advertise)(const char *name);

    /*
     * One MIDI channel-voice message: `status` (0x90|channel for note-on,
     * 0x80|channel for note-off), then two data bytes (note, velocity).  A
     * note-on with velocity 0 is the customary note-off.  -AG_ENODEV until a
     * client has connected and subscribed - there is nowhere to send until
     * then, and midi_ready() says when that is.
     */
    ag_err_t (*midi_send)(uint8_t status, uint8_t data1, uint8_t data2);

    /* True once a client is connected and listening for notes. */
    bool (*midi_ready)(void);

    /* Stop advertising and drop any client. */
    ag_err_t (*adv_stop)(void);

    /*
     * ABI 0.38 - the peripheral, past MIDI: the board as a plain device a phone
     * or PC connects to.  NULL unless the build has CONFIG_ARGON_BLE_PERIPHERAL.
     * One custom service: a value clients read (adv_set_read sets it) and one
     * they write (adv_last_write returns the most recent).  adv_start advertises
     * connectably and forever - a client that leaves does not stop it - and
     * starts the radio if it was off.
     */
    ag_err_t (*adv_start)(const char *name);
    void     (*adv_set_read)(const void *data, uint32_t len);
    int32_t  (*adv_last_write)(uint8_t *out, uint32_t max);
    ag_err_t (*adv_status)(ag_ble_adv_status_t *out);

    /*
     * ABI 0.38 - the central: observe, then connect and talk.  NULL unless the
     * build has CONFIG_ARGON_BLE_CENTRAL.  There is one radio, so one of these
     * at a time: scan is -AG_EBUSY while connected, connect is -AG_EBUSY while
     * scanning, and both are -AG_ENODEV with the radio off (scan/connect start
     * it).  scan blocks for `seconds` (0 = a sensible default) - a scan is a
     * listening window with nothing to return until it closes.
     *
     * connect blocks until the link is up or the attempt failed.  discover walks
     * every service and characteristic into the tables services()/chars() read;
     * it blocks a second or two on a device with many attributes.  read returns
     * the bytes placed in `out` (truncated at the ATT MTU), write with
     * with_response waits for the peer's acknowledgement - without, it returns
     * once queued and a failure is silent, which is what "without a response"
     * means.  A peer that drops the link is not hidden: connected() goes false
     * and the next read/write is -AG_ENODEV.  Nothing here reconnects - the
     * session is the application's to own.
     */
    ag_err_t (*scan)(ag_ble_dev_t *out, uint32_t max, uint32_t *found,
                     uint32_t seconds);
    ag_err_t (*connect)(const uint8_t addr[6], int addr_type,
                        uint32_t timeout_ms);
    ag_err_t (*disconnect)(void);
    bool     (*connected)(void);
    ag_err_t (*discover)(uint32_t timeout_ms);
    uint32_t (*services)(ag_ble_svc_t *out, uint32_t max);
    uint32_t (*chars)(ag_ble_chr_t *out, uint32_t max);
    int32_t  (*read)(uint16_t handle, uint8_t *out, uint32_t max,
                     uint32_t timeout_ms);
    ag_err_t (*write)(uint16_t handle, const void *data, uint32_t len,
                      bool with_response, uint32_t timeout_ms);

    /*
     * ABI 0.40 - raw advertising injection (NULL without the peripheral build).
     * addr is a six-byte address to spoof (forced to static-random), or NULL to
     * keep the board's own; data/len is one legacy advertisement (<= 31 bytes),
     * broadcast non-connectably.  Call it in a loop with a fresh address and
     * payload to put a crowd of fake devices in the air.  The peripheral's
     * adv_start and this share one advertising instance - use one at a time.
     */
    ag_err_t (*adv_raw)(const uint8_t addr[6], const void *data, uint32_t len);
} ag_ble_api_t;

/* ------------------------------------------------------------------------ */
/* power - how hard the machine is being driven, and who gets told          */
/* ------------------------------------------------------------------------ */

/*
 * ArgonOS does not save power behind an application's back, and it does not let
 * an application stop it either.  Which of those two matters more depends on
 * who asked, so a transition carries that with it.
 *
 *   AG_POWER_AUTO - the system noticed nobody was using it.  Advisory: every
 *                   application is told, nothing is required of any of them,
 *                   and nothing is ended.  It also refuses to do the part that
 *                   would break something - see AG_POWER_FIT_FULL_ONLY below -
 *                   because saving power was never worth breaking work for.
 *
 *   AG_POWER_USER - a person typed it.  That is an order, not a proposal: an
 *                   application has the grace period to answer that it is fit
 *                   for the new mode, and one that does not is **ended**.
 *
 * The second rule sounds harsh and is the kind one.  The alternative is an
 * application that goes on running at a third of the clock it was written for:
 * an audio path that clicks, a controller that misses its deadline, a log with
 * nothing in it to say why.  A process that is stopped, with a line in the
 * journal naming it and its own reason, is a fault somebody can act on.
 *
 * So an application that means to survive `power eco` answers - which is one
 * call in a loop it already has - or says once, with declare(), that any mode
 * suits it.  An application that cannot work slowly says that instead, and is
 * ended rather than left to misbehave.
 */

/*
 * The ladder, and how much of the machine each rung gives up.  Ordered: a
 * larger value is less machine, and the system compares them that way.
 *
 * The first step down is different in kind from the rest, and the difference is
 * what makes it automatic.  On this family the processor runs from the PLL at
 * 240, 160 or 80 MHz, and the peripheral bus stays at 80 MHz through all three:
 * dropping to 160 changes how fast arithmetic happens and nothing else - no
 * bus, no baud rate, no divider anywhere.  It costs a third of the speed, and
 * what it can break is only work that was already close to the edge.
 *
 * So AG_POWER_CRUISE is taken on silence: unless some process has said it needs
 * the full clock, the system assumes two thirds of it will do.  The lower rungs
 * keep the opposite rule - there, silence is not consent and a person's command
 * ends what has not answered - because a third of the clock is a different
 * proposition, and because those rungs are only reached deliberately or after
 * minutes of nobody touching the machine.
 *
 * A system that would rather be asked than assume can invert the first rule
 * with [power] cruise_when = declared in SYSTEM.CFG, and then only processes
 * that declared AG_POWER_FIT_ANY let the machine cruise.
 */
typedef enum {
    AG_POWER_FULL = 0, /* the clock at its maximum, the screen lit         */
    AG_POWER_CRUISE,   /* a step down nobody objected to; 160 MHz here     */
    AG_POWER_ECO,      /* the clock pinned low                             */
    AG_POWER_DOZE,     /* the clock pinned low and the screen dark         */
} ag_power_mode_t;

typedef enum {
    AG_POWER_AUTO = 0, /* the idle timer: advisory, kills nothing          */
    AG_POWER_USER,     /* a person: mandatory, ends what cannot comply     */
} ag_power_cause_t;

typedef enum {
    AG_POWER_ANSWER_NONE = 0, /* has not answered this transition           */
    AG_POWER_OK,              /* fit: carrying on                           */
    AG_POWER_PARKED,          /* fit: stopped whatever needed the clock     */
    AG_POWER_UNFIT,           /* not designed for it; end me instead        */
} ag_power_answer_t;

/*
 * Said once instead of answered every time.  It survives until the process
 * does, and it is what the two kinds of application that never poll should
 * use: a tool that plainly does not care, and a realtime path that plainly
 * does.
 */
typedef enum {
    /* The default: I answer each transition myself.  On a transition a person
     * asked for, no answer means not fit, and the process is ended. */
    AG_POWER_FIT_ASK = 0,
    /* Any mode suits me; stop asking.  Never ended for a mode change. */
    AG_POWER_FIT_ANY,
    /* I need the full clock.  An automatic transition then leaves the clock
     * alone; one a person typed ends this process, and says whose reason it
     * was. */
    AG_POWER_FIT_FULL_ONLY,
} ag_power_fitness_t;

/*
 * Long enough for a sentence a person can act on ("22 kHz tract, 87% of a
 * core"), short enough that one row per process fits in the internal memory
 * this class of machine has left over.  Anything longer is truncated rather
 * than refused.
 */
#define AG_POWER_WHY_MAX 32

typedef struct {
    uint8_t mode;    /* ag_power_mode_t - what the machine is now          */
    uint8_t pending; /* what it is about to be; == mode when nothing is    */
    uint8_t cause;   /* ag_power_cause_t of the pending transition         */
    bool    screen_on;
    bool    pending_screen_on;

    /*
     * cpu_mhz is read from the machine each time and is the only field here
     * that is not a setting: where scaling is available it moves on its own
     * between the two numbers below.  min == max means the clock is pinned.
     */
    uint32_t cpu_mhz;
    uint32_t cpu_min_mhz;
    uint32_t cpu_max_mhz;

    /*
     * What the band will be if the pending transition goes through - the
     * numbers, not just the name of the mode, because the number is what an
     * application needs in order to answer.  A synthesiser that measured its
     * own load knows whether it fits in eighty megahertz; "eco" tells it
     * nothing.  Equal to the fields above when nothing is pending.
     */
    uint32_t pending_cpu_min_mhz;
    uint32_t pending_cpu_max_mhz;

    /*
     * Milliseconds left to answer, zero when nothing is pending.  An
     * application polling once a frame has tens of frames to decide in.  On an
     * AG_POWER_USER transition this is also how long it has to live if it does
     * not answer.
     */
    uint32_t grace_ms;
} ag_power_status_t;

typedef struct ag_power_api {
    uint32_t size;

    /* Cheap; meant to be called once round an application's own loop. */
    ag_err_t (*status)(ag_power_status_t *out);

    /*
     * Answer the pending transition.  `why` is for AG_POWER_UNFIT and is what
     * the person at the console is shown; NULL otherwise.  -AG_ENOENT when
     * nothing is pending, which is the normal answer to a late poll.
     */
    ag_err_t (*answer)(ag_power_answer_t a, const char *why);

    /*
     * A standing answer, for an application that would rather say once than
     * answer every time.  `why` is shown by `power` and in the journal when a
     * declaration of AG_POWER_FIT_FULL_ONLY costs this process its life.
     *
     * AG_POWER_FIT_FULL_ONLY takes effect before this call returns: if the
     * system was cruising, the clock is back at its maximum by the time the
     * application does anything else.  That is the point of declaring it in the
     * first line of main rather than answering later - the first buffer is as
     * real as the thousandth.
     *
     * It goes when the process does, like every other resource, so a program
     * that crashes cannot leave the machine pinned at full speed for ever.
     */
    ag_err_t (*declare)(ag_power_fitness_t fitness, const char *why);
} ag_power_api_t;

/* ------------------------------------------------------------------------ */
/* wifimon - promiscuous capture and raw 802.11 injection (ABI 0.38)        */
/* ------------------------------------------------------------------------ */

/*
 * The radio on no network at all: turned to a channel, handing up every frame
 * it carries, and putting frames of the application's own making into the air.
 * This is the app-facing side of the same primitives the shell's `mon` uses -
 * capture and inject, nothing that knows what a beacon or a deauth is.  What a
 * frame means, and whether one ought to be sent, is the application's to decide,
 * the way the shell decides it for a person.
 *
 * api->wifimon is NULL unless the build set CONFIG_ARGON_NET_WIFI_MON, which is
 * off by default: a board that forges frames is a build-time choice, so the ABI
 * slot is one too.  An application probes `if (ag_api()->wifimon)` and adapts.
 */

/* Which kinds of frame are handed up, a mask for filter(); they combine. */
#define AG_WIFIMON_MGMT 0x1u /* beacons, probes, auth, deauth, assoc         */
#define AG_WIFIMON_CTRL 0x2u /* RTS/CTS/ACK and the rest of the fabric       */
#define AG_WIFIMON_DATA 0x4u /* the frames that actually carry something      */
#define AG_WIFIMON_MISC 0x8u /* everything the radio could not classify       */
#define AG_WIFIMON_ALL  0xfu

#define AG_WIFIMON_TX_MAX 1500u /* the largest raw frame this layer injects    */
#define AG_WIFIMON_SNAP   128u  /* how much of a frame recv() can hand back    */

/* Index into the counters[] array from ag_wifimon_api_t.counters(). */
enum {
    AG_WIFIMON_C_TOTAL = 0,
    AG_WIFIMON_C_MGMT,
    AG_WIFIMON_C_CTRL,
    AG_WIFIMON_C_DATA,
    AG_WIFIMON_C_MISC,
    AG_WIFIMON_C_N
};

/*
 * What recv() reports about the frame it hands back, beside the bytes: how
 * strong it was, which channel it came in on, and its real length on the air -
 * which may be larger than the prefix recv() could copy (AG_WIFIMON_SNAP).
 */
typedef struct {
    int8_t   rssi;
    uint8_t  channel;
    uint32_t length;
} ag_wifimon_frame_t;

typedef struct ag_wifimon_api {
    uint32_t size;

    /* Enter/leave promiscuous mode.  start() puts the radio on no network and
     * needs it powered; a joined station leaves its network here. */
    ag_err_t (*start)(void);
    ag_err_t (*stop)(void);

    /* The one channel the receiver hears; 1..14.  Sweep by setting each. */
    ag_err_t (*channel)(uint8_t primary);
    uint8_t  (*channel_get)(void);

    /* Which frame types reach recv() at all; a mask of AG_WIFIMON_* bits. */
    ag_err_t (*filter)(uint32_t mask);

    /*
     * Pop one captured frame into `buf` (up to `max`, at most AG_WIFIMON_SNAP
     * bytes are kept per frame).  Returns the byte count, or -AG_EAGAIN when
     * nothing arrived within timeout_ms (0 polls).  `meta`, when not NULL, gets
     * the rssi/channel and the frame's real length.
     */
    int32_t (*recv)(void *buf, uint32_t max, ag_wifimon_frame_t *meta,
                    uint32_t timeout_ms);

    /*
     * Put one raw frame on the current channel: a complete 802.11 frame without
     * the trailing FCS, which the radio appends.  Returns when the frame is
     * handed to the radio, not when anything received it.
     */
    ag_err_t (*tx_raw)(const void *frame, uint32_t len);

    /* Running frame counts by type (AG_WIFIMON_C_* index it) and how many
     * captured frames were dropped because the ring was full. */
    void     (*counters)(uint32_t out[AG_WIFIMON_C_N]);
    uint32_t (*dropped)(void);
} ag_wifimon_api_t;

/* ------------------------------------------------------------------------ */
/* wifi - station, access point and ESP-NOW for applications (ABI 0.39)      */
/* ------------------------------------------------------------------------ */

/*
 * The radio as a network, not as raw air.  wifimon (above) is the receiver on
 * one channel and the frame forge; this is the rest of what the shell's `wifi`
 * and `espnow` do - find the networks in reach and join one, offer a network of
 * the board's own, or throw datagrams straight at another board.  It is the same
 * port underneath (argon/port/wifi.h, argon/port/espnow.h); this is the
 * feature-probed, GPL-free face of it an application links against.
 *
 * One radio, so the usual exclusions hold and the port enforces them: a scan is
 * -AG_EBUSY while an association is in flight, the access point is forced onto a
 * joined station's channel, and ESP-NOW rides whatever channel the radio is on.
 *
 * Bringing the radio up costs a ~36 KB contiguous slice of internal RAM on a
 * board with about sixty free, so an application that means to use the radio is
 * usually launched after `wifi on` has already raised it - start() here is the
 * same bring-up and will fail -AG_ENOMEM from inside a large resident app, the
 * way it does for wifimon.  scan/connect/ap_start/espnow_start all raise the
 * radio if it is down, so start() is only needed to raise it without doing
 * anything else yet.
 */

#define AG_WIFI_SSID_MAX 32 /* an SSID is at most 32 bytes, not NUL-counted   */
#define AG_WIFI_PASS_MAX 63 /* a WPA key is 8..63 characters                  */

/* Security of a network, ag_wifi_ap_t.auth.  Ordered as the port orders it. */
enum {
    AG_WIFI_SEC_OPEN = 0,
    AG_WIFI_SEC_WEP,
    AG_WIFI_SEC_WPA,
    AG_WIFI_SEC_WPA2,
    AG_WIFI_SEC_WPA3,
    AG_WIFI_SEC_ENTERPRISE,
    AG_WIFI_SEC_OTHER,
};

/* Station link state, ag_wifi_status_t.state. */
enum {
    AG_WIFI_ST_OFF = 0,   /* radio not started                               */
    AG_WIFI_ST_IDLE,      /* on, joined to nothing                           */
    AG_WIFI_ST_JOINING,   /* association or DHCP in progress                 */
    AG_WIFI_ST_JOINED,    /* associated; an address may still be coming      */
};

/* ESP-NOW limits, mirroring the port. */
#define AG_WIFI_ESPNOW_MAX 250 /* one datagram's payload, bytes              */
#define AG_WIFI_ESPNOW_KEY 16  /* an encryption key, exactly this many bytes */

/* One network a scan found. */
typedef struct {
    char    ssid[AG_WIFI_SSID_MAX + 1];
    uint8_t bssid[6];
    int8_t  rssi;    /* dBm, negative                                        */
    uint8_t channel;
    uint8_t auth;    /* AG_WIFI_SEC_*                                         */
} ag_wifi_ap_t;

/* Where the station half stands.  The key is never reported. */
typedef struct {
    uint8_t  state;      /* AG_WIFI_ST_*                                     */
    char     ssid[AG_WIFI_SSID_MAX + 1]; /* joined or being joined          */
    uint8_t  bssid[6];   /* the access point actually joined, or zeros       */
    bool     pinned;     /* this access point was asked for by name          */
    int8_t   rssi;
    uint8_t  channel;
    uint32_t attempts;   /* association attempts since the last join         */
    int32_t  last_reason;/* the port's own disconnect reason code            */
} ag_wifi_status_t;

/* What the access-point half is offering, when one is up. */
typedef struct {
    bool     on;
    char     ssid[AG_WIFI_SSID_MAX + 1];
    uint8_t  channel;
    bool     hidden;
    bool     secured;    /* WPA2 with a key, not open                        */
    uint32_t clients;    /* stations associated right now                    */
    uint32_t ip;         /* the board's own address on it, host-order IPv4   */
} ag_wifi_ap_status_t;

typedef struct ag_wifi_api {
    uint32_t size;

    /* ---- station ---- */

    /* Power the radio on, joined to nothing.  Idempotent; -AG_ENOMEM when the
     * ~36 KB the driver needs is not free (see the note above). */
    ag_err_t (*start)(void);
    /* Power the radio off and give its memory back. */
    ag_err_t (*stop)(void);

    /*
     * Block a second or two and fill `out` with up to `max` networks; `found`
     * gets the number seen, which may exceed `max`.  Raises the radio if it is
     * down.  -AG_EBUSY while an association attempt is in flight.
     */
    ag_err_t (*scan)(ag_wifi_ap_t *out, uint32_t max, uint32_t *found);

    /*
     * Join a network.  Returns as soon as the attempt is made, not when it has
     * succeeded - poll status() (or net->ready() for an address).  bssid NULL
     * joins any access point of that name; six bytes pin one.  An empty pass
     * for the network already set means the key already held, not no key.
     */
    ag_err_t (*connect)(const char *ssid, const char *pass,
                        const uint8_t bssid[6]);
    ag_err_t (*disconnect)(void);
    ag_err_t (*status)(ag_wifi_status_t *out);

    /* ---- access point (NULL unless CONFIG_ARGON_NET_WIFI_AP) ---- */

    /*
     * Offer a network of the board's own.  Raises the radio if it is down.  An
     * empty pass is an open network; a key is 8..63 characters and shorter is
     * -AG_EINVAL.  channel 0 picks one; while also joined to a network the
     * point is forced onto that network's channel (ap_status reports which).
     */
    ag_err_t (*ap_start)(const char *ssid, const char *pass, uint8_t channel,
                         bool hidden);
    ag_err_t (*ap_stop)(void);
    ag_err_t (*ap_status)(ag_wifi_ap_status_t *out);

    /* ---- ESP-NOW (NULL unless CONFIG_ARGON_NET_ESPNOW) ---- */

    /*
     * Board-to-board datagrams, no network between them.  start() needs the
     * radio up (it raises it) and adds the broadcast peer.  self() is the
     * board's own address the other end must peer_add.  A frame is at most
     * AG_WIFI_ESPNOW_MAX bytes; longer is -AG_EINVAL.  A destination must be a
     * peer first (peer_add, or the broadcast peer).  peer_add channel 0 means
     * the channel the radio is on; key NULL is an open peer, else exactly
     * AG_WIFI_ESPNOW_KEY bytes.  recv() pops one waiting datagram into `buf`
     * (its sender into `mac`), returning the byte count or -AG_EAGAIN when
     * none arrived within timeout_ms (0 polls); dropped() is how many the ring
     * had to discard.
     */
    ag_err_t (*espnow_start)(void);
    ag_err_t (*espnow_stop)(void);
    ag_err_t (*espnow_self)(uint8_t out[6]);
    ag_err_t (*espnow_peer_add)(const uint8_t mac[6], uint8_t channel,
                                const uint8_t *key);
    ag_err_t (*espnow_peer_del)(const uint8_t mac[6]);
    ag_err_t (*espnow_send)(const uint8_t mac[6], const void *data,
                            uint32_t len);
    int32_t  (*espnow_recv)(uint8_t mac[6], void *buf, uint32_t max,
                            uint32_t timeout_ms);
    uint32_t (*espnow_dropped)(void);
} ag_wifi_api_t;

/* ------------------------------------------------------------------------ */
/* cam - a DVP image sensor's frames (ABI 0.41)                             */
/* ------------------------------------------------------------------------ */
/*
 * The thin half of a camera: the chip's LCD_CAM peripheral and its DMA, which
 * cannot be reached from a .SYS through io and so live in the image.  The other
 * half - which sensor is on the wires, its register tables, its SCCB init - is
 * a loadable driver that configures this transport and reads frames from it.
 * That split keeps the sensor zoo out of the firmware: a new sensor is a new
 * .SYS, not a rebuild.
 */

/* Pixel formats the transport delivers.  RGB565 is what a sensor without a
 * JPEG engine (the GC2145 on the S3 CAM board) gives; the encoder, if any, is
 * the application's, not the firmware's. */
typedef enum {
    AG_CAM_FMT_RGB565 = 0,
} ag_cam_fmt_t;

/* A sensor's DVP wiring, as the driver knows it.  Pin < 0 means "none". */
typedef struct {
    int16_t  xclk;      /* clock the chip drives out to the sensor           */
    int16_t  pclk;      /* pixel clock the sensor drives back                */
    int16_t  vsync;
    int16_t  href;      /* also called DE, data enable                       */
    int16_t  data[8];   /* D0..D7                                            */
    uint32_t xclk_hz;   /* XCLK frequency, e.g. 20000000                     */
} ag_cam_pins_t;

typedef struct ag_cam_api {
    uint32_t size;

    /*
     * Bring the transport up for a sensor already (or about to be) set to this
     * format and size.  Allocates the frame buffer in PSRAM, generates XCLK,
     * starts DMA.  The driver calls this once it has the sensor talking.
     */
    ag_err_t (*configure)(const ag_cam_pins_t *pins, ag_cam_fmt_t fmt,
                          uint32_t width, uint32_t height);

    /*
     * One captured frame.  Blocks up to timeout_ms for the next one; the
     * returned pointer is the PSRAM frame buffer and is valid until the next
     * capture().  NULL on timeout or error; *len is the bytes received.
     */
    const uint8_t *(*capture)(size_t *len, uint32_t timeout_ms);

    /* Stop DMA, free the buffer and the peripheral. */
    void (*stop)(void);
} ag_cam_api_t;

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

    /* ABI 0.34+: BLE for applications - NULL unless the build has the BLE
     * peripheral (CONFIG_ARGON_BLE_PERIPHERAL).  Small on purpose: enough for a
     * MIDI controller, not a general GATT toolkit. */
    const ag_ble_api_t *ble;

    /* ABI 0.35+: the clock, the screen, and being told before they change. */
    const ag_power_api_t *power;

    /* ABI 0.38+: promiscuous capture and raw 802.11 injection - NULL unless
     * the build set CONFIG_ARGON_NET_WIFI_MON (off by default). */
    const ag_wifimon_api_t *wifimon;

    /* ABI 0.39+: station, access point and ESP-NOW - NULL on a board with no
     * radio (CONFIG_ARGON_NET_WIFI off, e.g. QEMU).  Within it the AP and
     * ESP-NOW entries are NULL unless their own build options are set. */
    const ag_wifi_api_t *wifi;

    /* ABI 0.41+: the DVP camera transport - NULL without
     * CONFIG_ARGON_ENABLE_CAMERA. */
    const ag_cam_api_t *cam;
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
    /*
     * Run from flash (XIP) even when the code would fit the IRAM arena.
     *
     * The loader's default is to place code in the arena when it fits and fall
     * back to flash only when it does not - the arena is faster, so fitting is
     * the good case.  An application sets this to invert that on purpose: it
     * would rather leave the arena's internal SRAM free for something that
     * cannot come from flash.  The board this matters on has one radio and
     * sixty kilobytes of internal RAM: a Wi-Fi bring-up needs a ~36 KB
     * contiguous slice of it, and code the loader parked in the arena would
     * otherwise split the block the radio needs.  Sending that code to flash
     * instead keeps the internal RAM whole.
     *
     * Honoured only for a non-contiguous image (xtensa): flash cannot host a
     * contiguous image's data, so a contiguous one is placed in the arena
     * regardless.  On a part where the code will not fit the arena anyway (the
     * 8 KB arena on the original ESP32) the loader already chooses flash, and
     * the flag then only makes that choice intentional rather than incidental.
     */
    AG_AXE_WANT_XIP = 1u << 7,
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
