/*
 * ArgonOS - built-in shell.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/shell.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <argon/board.h>
#include <argon/cmdline.h>
#include <argon/codepage.h>
#include <argon/console.h>
#include <argon/device.h>
#include <argon/ioclaim.h>
#include <argon/kernel.h>
#include <argon/lineedit.h>
#include <argon/loader.h>
#include <argon/log.h>
#include <argon/module.h>
#include <argon/path.h>
#include <argon/probe.h>
#include <argon/proc.h>
#include <argon/vfs.h>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "boot/platform.h"
#include "core/sysconfig.h"
#include "proc/supervisor.h"
#include "shell/cmd_fs.h"

typedef struct {
    const char *name;
    const char *usage;
    const char *help;
    int (*fn)(int argc, char **argv);
} ag_command_t;

static char          s_cwd[AG_PATH_MAX] = "/";
static ag_lineedit_t s_line;
static int           s_last_status;

/* ---------------------------------------------------------------------- */
/* Presentation                                                           */
/* ---------------------------------------------------------------------- */

static const struct {
    const char *mount;
    char        letter;
} k_drives[] = {
    {"/sd", 'A'},
    {"/sys", 'C'},
    {"/tmp", 'T'},
    {"/dev", 'D'},
};

void ag_shell_dos_path(const char *posix_path, char *out, size_t n)
{
    if (out == NULL || n == 0) {
        return;
    }
    if (posix_path == NULL) {
        posix_path = "/";
    }

    for (size_t i = 0; i < sizeof(k_drives) / sizeof(k_drives[0]); i++) {
        const char  *mount = k_drives[i].mount;
        const size_t len = strlen(mount);

        if (strncmp(posix_path, mount, len) != 0) {
            continue;
        }
        if (posix_path[len] != '\0' && posix_path[len] != '/') {
            continue;
        }

        size_t w = 0;
        if (w + 4 < n) {
            out[w++] = k_drives[i].letter;
            out[w++] = ':';
            out[w++] = '\\';
        }
        const char *tail = posix_path + len + (posix_path[len] == '/' ? 1 : 0);
        for (const char *p = tail; *p != '\0' && w + 1 < n; p++) {
            out[w++] = (*p == '/') ? '\\' : *p;
        }
        out[w] = '\0';
        return;
    }

    snprintf(out, n, "%s", posix_path);
}

static void build_prompt(char *out, size_t n)
{
    ag_shell_dos_path(s_cwd, out, n - 2);
    const size_t len = strlen(out);
    if (len + 2 <= n) {
        out[len] = '>';
        out[len + 1] = '\0';
    }
}

ag_err_t ag_shell_set_cwd(const char *path)
{
    if (path == NULL || path[0] != '/') {
        return -AG_EINVAL;
    }
    if (strlen(path) >= sizeof(s_cwd)) {
        return -AG_ERANGE;
    }
    strcpy(s_cwd, path);
    return AG_OK;
}

/*
 * Where the shell starts.  Preferring removable media matches the habit of
 * booting off the floppy, and falls back through internal storage to the RAM
 * disk so that there is always somewhere to be.
 */
static void pick_initial_cwd(void)
{
    static const char *const candidates[] = {"/sd", "/sys", "/tmp"};

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        ag_stat_t st;
        if (ag_vfs_stat(candidates[i], NULL, &st) == AG_OK &&
            (st.attr & AG_A_DIR)) {
            ag_shell_set_cwd(candidates[i]);
            return;
        }
    }
    ag_shell_set_cwd("/");
}

/* ---------------------------------------------------------------------- */
/* Line rendering                                                         */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint16_t row;
    uint16_t col0;
} prompt_pos_t;

static void redraw_line(const ag_lineedit_t *le, const prompt_pos_t *pos)
{
    ag_console_lock();
    ag_screen_t *sc = ag_console_screen();

    const uint16_t avail =
        (sc->cols > pos->col0 + 1) ? (uint16_t)(sc->cols - pos->col0) : 1;

    /*
     * A byte is a character is a column, because the line editor holds code page
     * bytes - the same bytes the screen holds.  A line longer than the screen
     * scrolls horizontally rather than wrapping; wrapping would move the prompt
     * row and lose track of where to redraw.
     */
    const uint16_t offset = (le->cursor >= avail)
                                ? (uint16_t)(le->cursor - avail + 1)
                                : 0;
    const uint16_t shown =
        ((uint16_t)(le->len - offset) < avail) ? (uint16_t)(le->len - offset)
                                              : avail;

    ag_screen_gotoxy(sc, pos->col0, pos->row);
    ag_screen_write(sc, le->buf + offset, shown);
    for (uint16_t x = shown; x < avail; x++) {
        ag_screen_putc_raw(sc, ' ');
    }

    ag_screen_gotoxy(sc, (uint16_t)(pos->col0 + le->cursor - offset), pos->row);
    ag_console_unlock();
}

/* ---------------------------------------------------------------------- */
/* Commands                                                               */
/* ---------------------------------------------------------------------- */

/* Defined after the table it walks. */
static int cmd_help(int argc, char **argv);

static int cmd_ver(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const ag_sysinfo_t  *si = ag_sysinfo();
    const ag_platform_t *pl = ag_platform();

    ag_console_printf("%s %s (%s)\n", si->os_name, si->os_version, si->build);
    ag_console_printf("%s rev %u, %u cores at %u MHz, profile %s\n", si->chip,
                      (unsigned)pl->chip_revision, (unsigned)si->cpu_cores,
                      (unsigned)(si->cpu_hz / 1000000u), si->profile);
    /* The board name comes from the board layer, which is where it is decided. */
    ag_console_printf("board %s, ABI %u.%u, application core %u\n",
                      ag_board()->name, si->abi_major, si->abi_minor,
                      si->app_core);
    if (ag_sysconfig_sources()[0] != '\0') {
        ag_console_printf("configured by %s\n", ag_sysconfig_sources());
    }
    return 0;
}

static int cmd_mem(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const size_t int_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    ag_console_printf("                 total        free     largest\n");
    ag_console_printf("  internal  %8u KB  %8u KB  %8u KB\n",
                      (unsigned)(int_total / 1024), (unsigned)(int_free / 1024),
                      (unsigned)(int_block / 1024));
    if (psram_total > 0) {
        ag_console_printf("  extended  %8u KB  %8u KB\n",
                          (unsigned)(psram_total / 1024),
                          (unsigned)(psram_free / 1024));
    } else {
        ag_console_puts("  extended         none\n");
    }
    ag_console_printf("\n  %u KB used by the system\n",
                      (unsigned)((int_total - int_free) / 1024));

    /*
     * The arena is the honest answer to "how much executable memory is there":
     * MALLOC_CAP_EXEC reports zero on this chip because ESP-IDF gives none of
     * that memory to the heap, which is why the arena is reserved at link time.
     */
    ag_console_printf("  %u KB reserved for application code (%s)\n",
                      (unsigned)(ag_loader_arena_size() / 1024),
                      ag_loader_arena_busy() ? "in use" : "free");

    /* Silent when it is zero, which is the only acceptable value. */
    const uint32_t dropped = ag_console_dropped_events();
    if (dropped != 0) {
        ag_console_printf("  WARNING: %u console input events lost\n",
                          (unsigned)dropped);
    }
    return 0;
}

static int cmd_cls(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ag_console_lock();
    ag_screen_cls(ag_console_screen());
    ag_console_unlock();
    return 0;
}

static int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        ag_console_puts(argv[i]);
        if (i + 1 < argc) {
            ag_console_puts(" ");
        }
    }
    ag_console_puts("\n");
    return 0;
}

static int cmd_boot(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const char *const k_stage_names[AG_STAGE_COUNT] = {
        "platform", "memory", "log",     "board",  "console",    "storage",
        "config",   "devices", "media",  "modules", "supervisor", "shell",
    };

    const ag_boot_report_t *r = ag_boot_report();

    ag_console_printf("boot took %u us%s\n\n", (unsigned)r->boot_us,
                      r->degraded ? "  (degraded)" : "");
    for (int i = 0; i < AG_STAGE_COUNT; i++) {
        if (r->stage_result[i] == -AG_ENOSYS) {
            ag_console_printf("  %-11s not implemented yet\n",
                              k_stage_names[i]);
        } else if (r->stage_result[i] == AG_OK) {
            ag_console_printf("  %-11s ok         %6u us\n", k_stage_names[i],
                              (unsigned)r->stage_us[i]);
        } else {
            ag_console_printf("  %-11s FAILED     err %d\n", k_stage_names[i],
                              (int)r->stage_result[i]);
        }
    }
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const uint64_t us = (uint64_t)esp_timer_get_time();
    const uint32_t total_s = (uint32_t)(us / 1000000u);

    ag_console_printf("up %u:%02u:%02u.%03u\n", (unsigned)(total_s / 3600u),
                      (unsigned)((total_s / 60u) % 60u),
                      (unsigned)(total_s % 60u),
                      (unsigned)((us / 1000u) % 1000u));
    return 0;
}

static int cmd_color(int argc, char **argv)
{
    if (argc != 3) {
        ag_console_puts("usage: color <fg> <bg>, values 0-15\n");
        return 1;
    }

    const int fg = atoi(argv[1]);
    const int bg = atoi(argv[2]);
    if (fg < 0 || fg > 15 || bg < 0 || bg > 15) {
        ag_console_puts("colour values are 0 to 15\n");
        return 1;
    }

    ag_console_lock();
    ag_screen_set_attr(ag_console_screen(),
                       AG_ATTR((uint8_t)fg, (uint8_t)bg));
    ag_console_unlock();
    return 0;
}

static void print_load_error(const char *path, ag_err_t err)
{
    const char *why;

    switch (-err) {
    case AG_ENOENT:  why = "file not found"; break;
    case AG_EFORMAT: why = "not an ArgonOS application"; break;
    case AG_ENOTSUP: why = "built for a different processor"; break;
    case AG_EABI:    why = "built for a different version of this system"; break;
    case AG_ENOMEM:  why = "not enough memory to load it"; break;
    case AG_EBUSY:   why = "too many applications are loaded"; break;
    case AG_ERANGE:  why = "too many arguments"; break;
    case AG_EINVAL:  why = "is a driver; use 'drv load'"; break;
    case AG_EEXIST:  why = "already loaded"; break;
    case AG_ENFILE:  why = "too many modules are loaded"; break;
    default:         why = "could not be loaded"; break;
    }
    ag_console_printf("%s: %s\n", path, why);
}

static int cmd_run(int argc, char **argv)
{
    bool background = false;
    int  first = 1;

    if (argc > 1 && ag_path_icmp(argv[1], "/b") == 0) {
        background = true;
        first = 2;
    }
    if (argc <= first) {
        ag_console_puts("usage: run [/b] <file> [arguments]\n");
        ag_console_puts("  /b  leave it running in the background\n");
        return 1;
    }

    /*
     * The application sees the path it was started with as argv[0], which is
     * what a program expects and what lets it find its own files.
     */
    if (background) {
        ag_pid_t       pid = 0;
        const ag_err_t err = ag_proc_spawn(argv[first], argc - first,
                                           &argv[first], AG_SPAWN_BACKGROUND,
                                           &pid);
        if (err != AG_OK) {
            print_load_error(argv[first], err);
            return 1;
        }
        ag_console_printf("started pid %u\n", (unsigned)pid);
        return 0;
    }

    int32_t        status = 0;
    const ag_err_t err = ag_proc_exec(argv[first], argc - first, &argv[first], 0,
                                      &status);
    if (err == -AG_EKILLED) {
        ag_console_printf("%s: stopped\n", argv[first]);
        return 1;
    }
    if (err != AG_OK) {
        print_load_error(argv[first], err);
        return 1;
    }

    /* Whatever the application did to the screen, the prompt starts clean. */
    ag_console_lock();
    ag_screen_set_attr(ag_console_screen(), AG_ATTR_DEFAULT);
    ag_screen_set_cursor(ag_console_screen(), true);
    ag_console_unlock();

    return (int)status;
}

static int cmd_log(int argc, char **argv)
{
    uint32_t want = UINT32_MAX;

    for (int i = 1; i < argc; i++) {
        if (ag_path_icmp(argv[i], "clear") == 0) {
            ag_log_clear();
            ag_console_puts("journal cleared\n");
            return 0;
        }
        if (ag_path_icmp(argv[i], "-n") == 0 && i + 1 < argc) {
            const int n = atoi(argv[++i]);
            if (n > 0) {
                want = (uint32_t)n;
            }
        }
    }

    const ag_journal_t *j = ag_log_journal();
    const uint32_t      held = ag_journal_count(j);
    const uint32_t      skip = (want < held) ? (held - want) : 0;

    ag_journal_iter_t it;
    ag_journal_begin(j, &it);

    char     line[AG_JOURNAL_LINE_MAX];
    uint32_t index = 0;
    while (ag_journal_next(j, &it, line, sizeof(line))) {
        if (index++ < skip) {
            continue;
        }
        ag_console_printf("%s\n", line);
    }

    const uint32_t lost = ag_journal_lost(j);
    ag_console_printf("\n%u lines held", (unsigned)held);
    if (lost > 0) {
        ag_console_printf(", %u older lines already overwritten",
                          (unsigned)lost);
    }
    ag_console_puts("\n");
    return 0;
}

/*
 * The screen holds bytes; this says what they mean.  Changing the page does not
 * rewrite what is already on the screen - the same as changing the font on a PC -
 * so the sensible time to do it is before printing the text it is meant for.
 */
static int cmd_chcp(int argc, char **argv)
{
    if (argc < 2) {
        const ag_cp_t now = ag_cp_active();
        ag_console_printf("active code page: %u  (%s)\n",
                          (unsigned)ag_cp_number(now), ag_cp_title(now));
        for (int i = 0; i < AG_CP_COUNT; i++) {
            if ((ag_cp_t)i != now) {
                ag_console_printf("  %u  %s\n",
                                  (unsigned)ag_cp_number((ag_cp_t)i),
                                  ag_cp_title((ag_cp_t)i));
            }
        }
        return 0;
    }

    ag_cp_t chosen;
    if (!ag_cp_from_number((uint16_t)atoi(argv[1]), &chosen)) {
        ag_console_printf("%s: no such code page\n", argv[1]);
        return 1;
    }

    ag_cp_set_active(chosen);

    /* Every row has to be sent again: the same bytes now mean other characters. */
    ag_console_lock();
    ag_screen_mark_all_dirty(ag_console_screen());
    ag_console_unlock();

    ag_console_printf("active code page: %u\n", (unsigned)ag_cp_number(chosen));
    return 0;
}

static int cmd_errorlevel(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ag_console_printf("%d\n", s_last_status);
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ag_console_puts("restarting...\n");
    ag_console_sync();
    esp_restart();
    return 0;
}

static int cmd_ps(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ag_console_puts("  pid  name              state        memory     \n");

    uint32_t shown = 0;
    for (uint32_t i = 0;; i++) {
        ag_procinfo_t info;
        if (ag_proc_info(i, &info) != AG_OK) {
            break;
        }
        ag_console_printf("  %-4u %-17s %-12s %5u KB%s\n", (unsigned)info.pid,
                          info.name, ag_proc_state_name(info.state),
                          (unsigned)(info.mem_used / 1024u),
                          info.foreground ? "  (foreground)" : "");
        shown++;
    }

    if (shown == 0) {
        ag_console_puts("  no applications loaded\n");
    }
    return 0;
}

/* Bytes, kilobytes or megabytes, whichever makes the number readable. */
static void human_size(uint64_t bytes, char *out, size_t len)
{
    if (bytes == 0) {
        snprintf(out, len, "%s", "-");
    } else if (bytes < 1024u) {
        snprintf(out, len, "%u B", (unsigned)bytes);
    } else if (bytes < 1024u * 1024u) {
        snprintf(out, len, "%u KB", (unsigned)(bytes / 1024u));
    } else {
        snprintf(out, len, "%u MB", (unsigned)(bytes / (1024u * 1024u)));
    }
}

static void dev_flag_text(uint32_t flags, char *out, size_t len)
{
    out[0] = '\0';
    const struct {
        uint32_t    bit;
        const char *text;
    } names[] = {
        {AG_DEVF_EXCLUSIVE, "exclusive"},
        {AG_DEVF_READONLY, "read-only"},
        {AG_DEVF_HOTPLUG, "removable"},
        {AG_DEVF_DMA, "dma"},
        {AG_DEVF_BUSY, "open"},
    };

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if ((flags & names[i].bit) == 0) {
            continue;
        }
        if (out[0] != '\0') {
            strncat(out, " ", len - strlen(out) - 1);
        }
        strncat(out, names[i].text, len - strlen(out) - 1);
    }
}

/*
 * What is registered, and what one device is.  The listing is the answer to
 * "what does this board have", which on a system whose whole point is being
 * brought up on an unknown board is the first question anybody asks.
 */
static int cmd_dev(int argc, char **argv)
{
    if (argc > 1) {
        ag_device_t *dev = ag_dev_find(argv[1]);
        if (dev == NULL) {
            ag_console_printf("%s: no such device\n", argv[1]);
            return 1;
        }

        char size[16];
        char flags[64];
        human_size(ag_dev_size(dev), size, sizeof(size));
        dev_flag_text(dev->flags | (dev->open_count ? AG_DEVF_BUSY : 0), flags,
                      sizeof(flags));

        ag_console_printf("name    %s\n", dev->name);
        ag_console_printf("class   %s\n", ag_dev_class_name(dev->cls));
        ag_console_printf("driver  %s\n", dev->driver);
        ag_console_printf("size    %s\n", size);
        ag_console_printf("open    %u\n", (unsigned)dev->open_count);
        ag_console_printf("flags   %s\n", (flags[0] != '\0') ? flags : "-");
        ag_console_printf("path    d:\\%s\n", dev->name);

        ag_geometry_t geo;
        if (ag_dev_ioctl(dev, AG_IOC_GEOMETRY, &geo, sizeof(geo)) == AG_OK) {
            ag_console_printf("sectors %u of %u bytes\n",
                              (unsigned)geo.sectors, (unsigned)geo.sector_size);
        }
        return 0;
    }

    ag_console_puts("name      class     driver      size       flags\n");

    uint32_t shown = 0;
    for (uint32_t i = 0;; i++) {
        ag_devinfo_t info;
        if (ag_dev_info(i, AG_DEV_ANY, &info) != AG_OK) {
            break;
        }

        char size[16];
        char flags[64];
        human_size(ag_dev_size(ag_dev_find(info.name)), size, sizeof(size));
        dev_flag_text(info.flags, flags, sizeof(flags));

        ag_console_printf("%-9s %-9s %-11s %-10s %s\n", info.name,
                          ag_dev_class_name(info.cls), info.driver, size,
                          flags);
        shown++;
    }

    if (shown == 0) {
        ag_console_puts("no devices\n");
    }
    return 0;
}

/*
 * Loadable .SYS modules.  `dev` shows what they registered; this shows the
 * modules themselves, and is how one is brought in or taken out by hand.
 */
static int cmd_drv(int argc, char **argv)
{
    if (argc >= 2 && ag_path_icmp(argv[1], "load") == 0) {
        if (argc != 3) {
            ag_console_puts("usage: drv load <file>\n");
            return 1;
        }
        const ag_err_t err = ag_module_load(argv[2], s_cwd);
        if (err != AG_OK) {
            print_load_error(argv[2], err);
            return 1;
        }
        ag_console_printf("loaded %s\n", argv[2]);
        return 0;
    }

    if (argc >= 2 && ag_path_icmp(argv[1], "unload") == 0) {
        if (argc != 3) {
            ag_console_puts("usage: drv unload <name>\n");
            return 1;
        }
        const ag_err_t err = ag_module_unload(argv[2]);
        if (err == -AG_ENOENT) {
            ag_console_printf("%s: not loaded\n", argv[2]);
            return 1;
        }
        if (err != AG_OK) {
            ag_console_printf("%s: %d\n", argv[2], (int)err);
            return 1;
        }
        ag_console_printf("unloaded %s\n", argv[2]);
        return 0;
    }

    if (argc >= 2 && ag_path_icmp(argv[1], "probe") == 0) {
        if (argc != 2) {
            ag_console_puts("usage: drv probe\n");
            ag_console_puts(
                "  load modules listed under [modules] probe= in SYSTEM.CFG\n");
            ag_console_puts(
                "  for chips that answer on I2C (see docs/sdk/05-drivers.md)\n");
            return 1;
        }
        uint32_t       loaded = 0;
        uint32_t       missed = 0;
        const ag_err_t err = ag_probe_run(&loaded, &missed);
        if (err != AG_OK) {
            ag_console_printf("probe: %d\n", (int)err);
            return 1;
        }
        ag_console_printf("probe: %u loaded, %u absent\n", (unsigned)loaded,
                          (unsigned)missed);
        return 0;
    }

    if (argc > 1) {
        ag_console_puts("usage: drv [load|unload|probe] ...\n");
        return 1;
    }

    ag_console_puts("name      version  code       data       path\n");

    uint32_t shown = 0;
    for (uint32_t i = 0;; i++) {
        ag_modinfo_t info;
        if (ag_module_info(i, &info) != AG_OK) {
            break;
        }

        char code[16];
        char data[16];
        human_size(info.code_bytes, code, sizeof(code));
        human_size(info.data_bytes, data, sizeof(data));

        ag_console_printf("%-9s %-8s %-10s %-10s %s\n", info.name,
                          info.version, code, data, info.path);
        shown++;
    }

    if (shown == 0) {
        ag_console_puts("no modules loaded\n");
    }
    return 0;
}

/* ---------------------------------------------------------------------- */

static const char *pin_state_name(ag_pin_state_t state)
{
    switch (state) {
    case AG_PIN_RESERVED: return "system";
    case AG_PIN_HELD:     return "held";
    case AG_PIN_FREE:
    default:              return "free";
    }
}

static void io_show_pin(int pin)
{
    ag_pin_info_t info;
    if (ag_io_pin_info(pin, &info) != AG_OK) {
        ag_console_printf("no pin %d on this chip\n", pin);
        return;
    }

    const ag_io_api_t *io = ag_loader_api()->io;
    ag_console_printf("%4d  %-8s %-6s %-12s %5d%s\n", pin,
                      pin_state_name(info.state),
                      (info.state == AG_PIN_HELD && info.owner != AG_PID_KERNEL)
                          ? "app"
                          : (info.state == AG_PIN_FREE ? "-" : "system"),
                      (info.why[0] != '\0') ? info.why : "-",
                      (int)io->gpio_read(pin), info.isr ? "  isr" : "");
}

/*
 * The pin map, and the one tool that makes an unknown board tractable: a scan
 * of an I2C bus.  Both belong in the shell rather than in an application,
 * because the moment you need them is before anything has been copied onto the
 * board - see docs/user/02-board-setup.md.
 */
static int cmd_io(int argc, char **argv)
{
    const ag_io_api_t *io = ag_loader_api()->io;
    if (io == NULL) {
        ag_console_puts("this build has no direct hardware access\n");
        return 1;
    }

    if (argc >= 3 && ag_path_icmp(argv[1], "i2c") == 0) {
        const int bus = atoi(argv[2]);
        ag_console_printf("scanning i2c%d...\n", bus);

        uint32_t found = 0;
        for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
            const ag_err_t err = io->i2c_probe(bus, addr);
            if (err == AG_OK) {
                ag_console_printf("  0x%02x answers\n", (unsigned)addr);
                found++;
            } else if (err != -AG_ENOENT) {
                /* Not "nothing at this address" but "no such bus" or "the bus
                 * is not working": say so once and stop, rather than printing
                 * a hundred and twelve identical lines. */
                ag_console_printf("  i2c%d: %s\n", bus,
                                  ag_loader_api()->sys->strerror(err));
                return 1;
            }
        }
        ag_console_printf("%u device(s)\n", (unsigned)found);
        return 0;
    }

    if (argc >= 2) {
        const int pin = atoi(argv[1]);

        if (argc >= 3) {
            ag_err_t err = AG_OK;
            if (ag_path_icmp(argv[2], "in") == 0) {
                err = io->gpio_config(pin, AG_GPIO_IN);
            } else if (ag_path_icmp(argv[2], "up") == 0) {
                err = io->gpio_config(pin, AG_GPIO_IN_PULLUP);
            } else if (ag_path_icmp(argv[2], "down") == 0) {
                err = io->gpio_config(pin, AG_GPIO_IN_PULLDOWN);
            } else if (ag_path_icmp(argv[2], "out") == 0) {
                err = io->gpio_config(pin, AG_GPIO_OUT);
            } else if (ag_path_icmp(argv[2], "0") == 0 ||
                       ag_path_icmp(argv[2], "1") == 0) {
                io->gpio_write(pin, argv[2][0] - '0');
            } else {
                ag_console_puts("usage: io <pin> [in|up|down|out|0|1]\n");
                return 1;
            }
            if (err != AG_OK) {
                ag_console_printf("pin %d: %s\n", pin,
                                  ag_loader_api()->sys->strerror(err));
                return 1;
            }
        }

        ag_console_puts(" pin  state    owner  what         level\n");
        io_show_pin(pin);
        return 0;
    }

    ag_console_puts(" pin  state    owner  what         level\n");

    uint32_t   shown = 0;
    const int  pins = ag_io_pin_count();
    for (int pin = 0; pin < pins; pin++) {
        ag_pin_info_t info;
        if (ag_io_pin_info(pin, &info) != AG_OK ||
            info.state == AG_PIN_FREE) {
            continue;
        }
        io_show_pin(pin);
        shown++;
    }

    /* The free ones are not listed - there are forty of them and they all say
     * the same thing - but the count is what tells you there is room. */
    ag_console_printf("%u of %d pins free\n", (unsigned)(pins - shown), pins);
    return 0;
}

/*
 * The file manager, built into the image.  It is the same code as apps/fm builds
 * into a .AXE, called here directly instead of being loaded - so a board that
 * boots has one without anything having to be copied onto it.
 */
int ag_fm_main(int argc, char **argv);

static int cmd_fm(int argc, char **argv)
{
    /* argv[0] is the command; the manager takes the two panels' directories. */
    return ag_fm_main(argc, argv);
}

int ag_edit_main(int argc, char **argv);

static int cmd_edit(int argc, char **argv)
{
    return ag_edit_main(argc, argv);
}

static int cmd_kill(int argc, char **argv)
{
    if (argc != 2) {
        ag_console_puts("usage: kill <pid>\n");
        return 1;
    }

    const ag_pid_t pid = (ag_pid_t)atoi(argv[1]);
    if (pid <= 0) {
        ag_console_puts("pid must be a number above zero\n");
        return 1;
    }

    const ag_err_t err = ag_proc_kill(pid, "killed from the shell");
    switch (-err) {
    case AG_OK:
        ag_console_printf("pid %u stopped\n", (unsigned)pid);
        return 0;
    case AG_ENOENT:
        ag_console_printf("no process with pid %u\n", (unsigned)pid);
        return 1;
    case AG_EBUSY:
        /* The journal has the detail; the prompt gets the decision. */
        ag_console_puts("it is inside the system and cannot be stopped "
                        "safely; see log\n");
        return 1;
    default:
        ag_console_printf("could not stop pid %u: %d\n", (unsigned)pid,
                          (int)err);
        return 1;
    }
}

static int cmd_fg(int argc, char **argv)
{
    if (argc != 2) {
        ag_console_printf("usage: fg <pid>\n");
        return 1;
    }

    const ag_pid_t pid = (ag_pid_t)atoi(argv[1]);
    ag_err_t       err = ag_proc_set_foreground(pid);
    if (err != AG_OK) {
        ag_console_printf("no process with pid %u\n", (unsigned)pid);
        return 1;
    }

    /*
     * Bringing it to the front means the shell steps back: it waits, rather
     * than going on reading the same keyboard the process now owns.
     */
    int32_t status = 0;
    err = ag_proc_wait(pid, &status, UINT32_MAX);

    ag_console_lock();
    ag_screen_set_attr(ag_console_screen(), AG_ATTR_DEFAULT);
    ag_screen_set_cursor(ag_console_screen(), true);
    ag_console_unlock();

    if (err == -AG_EKILLED) {
        ag_console_printf("pid %u stopped\n", (unsigned)pid);
        return 1;
    }
    if (err != AG_OK) {
        ag_console_printf("pid %u: %d\n", (unsigned)pid, (int)err);
        return 1;
    }
    return (int)status;
}

static const ag_command_t k_commands[] = {
    {"help", "", "list these commands", cmd_help},
    {"ver", "", "version and hardware", cmd_ver},
    {"mem", "", "memory usage", cmd_mem},
    {"boot", "", "boot stage report", cmd_boot},
    {"log", "[-n N|clear]", "system journal", cmd_log},
    {"uptime", "", "time since reset", cmd_uptime},
    {"cls", "", "clear the screen", cmd_cls},
    {"echo", "<text>", "print text", cmd_echo},
    {"color", "<fg> <bg>", "set text colours", cmd_color},
    {"chcp", "[437|866|1251]", "screen code page", cmd_chcp},
    {"fm", "[left] [right]", "file manager, two panels", cmd_fm},
    {"edit", "[file]", "create or edit a text file", cmd_edit},
    {"dev", "[name]", "list devices, or describe one", cmd_dev},
    {"drv", "[load|unload|probe]", "modules: list, load, unload, I2C probe",
     cmd_drv},
    {"io", "[pin [mode]] | i2c <bus>", "pins and buses", cmd_io},
    {"ps", "", "list running applications", cmd_ps},
    {"kill", "<pid>", "stop an application", cmd_kill},
    {"fg", "<pid>", "bring an application to the foreground and wait", cmd_fg},
    {"dir", "[path]", "list a directory", ag_cmd_dir},
    {"cd", "[path]", "change directory", ag_cmd_cd},
    {"type", "<file>", "print a file", ag_cmd_type},
    {"copy", "<src> <dst>", "copy a file", ag_cmd_copy},
    {"del", "<pattern>", "delete files", ag_cmd_del},
    {"md", "<path>", "make a directory", ag_cmd_mkdir},
    {"rd", "<path>", "remove a directory", ag_cmd_rmdir},
    {"ren", "<old> <new>", "rename a file", ag_cmd_rename},
    {"mount", "", "list mounted drives", ag_cmd_mount},
    {"format", "<drive> [/y]", "make a fresh filesystem", ag_cmd_format},
    {"hexdump", "<file>", "dump a file as bytes", ag_cmd_hexdump},
    {"recv", "<file>", "receive a file as hex", ag_cmd_recv},
    {"run", "[/b] <file> [args]", "run an application", cmd_run},
    {"errorlevel", "", "exit code of the last command", cmd_errorlevel},
    {"reboot", "", "restart the board", cmd_reboot},
    {NULL, NULL, NULL, NULL},
};

static int cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ag_console_puts("ArgonOS built-in commands:\n\n");
    for (int i = 0; k_commands[i].name != NULL; i++) {
        ag_console_printf("  %-11s %-12s %s\n", k_commands[i].name,
                          k_commands[i].usage, k_commands[i].help);
    }
    ag_console_puts(
        "\nLine editing: arrows, Home, End, Ctrl+A/E/U/K/W; history with up "
        "and down.\n"
        "Output can be sent to a file:  dir > files.txt   or   ver >> log.txt\n");
    return 0;
}

/* ---------------------------------------------------------------------- */

const char *ag_shell_cwd(void) { return s_cwd; }

bool ag_shell_interrupted(void) { return ag_supervisor_interrupted(); }

/* Output redirection: the sink the console writes through for "dir > file". */
static int32_t file_sink(void *ctx, const char *data, size_t len)
{
    const ag_handle_t h = (ag_handle_t)(intptr_t)ctx;
    return ag_vfs_write(h, data, len);
}

/*
 * Finds "> file" or ">> file" at the end of the argument list, opens the
 * target and shortens argc so the command never sees it.  Returns the new
 * argc, or a negative error.
 */
static int take_redirection(int argc, char **argv, ag_handle_t *out)
{
    *out = AG_INVALID_HANDLE;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '>') {
            continue;
        }

        const bool  append = argv[i][1] == '>';
        const char *target = argv[i] + (append ? 2 : 1);

        if (*target == '\0') {
            if (i + 1 >= argc) {
                ag_console_puts("syntax error: expected a file after >\n");
                return -AG_EINVAL;
            }
            target = argv[i + 1];
        }

        const uint32_t flags = AG_O_WRONLY | AG_O_CREATE |
                               (append ? AG_O_APPEND : AG_O_TRUNC);
        const ag_handle_t h = ag_vfs_open(target, s_cwd, flags);
        if (h < 0) {
            ag_console_printf("cannot write %s\n", target);
            return h;
        }

        *out = h;
        return i; /* everything from here on was redirection */
    }
    return argc;
}

int ag_shell_execute(const char *line)
{
    char  work[AG_LINE_MAX];
    char *argv[AG_ARGV_MAX];

    if (line == NULL) {
        return 0;
    }
    strncpy(work, line, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    int argc = ag_cmdline_split(work, argv, AG_ARGV_MAX);
    if (argc == 0) {
        return 0;
    }

    ag_handle_t redirect = AG_INVALID_HANDLE;
    const int   trimmed = take_redirection(argc, argv, &redirect);
    if (trimmed < 0) {
        return 1;
    }
    argc = trimmed;

    int status = 127;
    bool found = false;

    /* This command's own answer to "was I interrupted", not the last one's. */
    ag_supervisor_clear_interrupt();

    if (redirect >= 0) {
        ag_console_redirect(file_sink, (void *)(intptr_t)redirect);
    }

    /*
     * A bare drive specification changes drive, the way it did in DOS.  Unlike
     * DOS we do not remember a separate directory per drive: "A:" goes to the
     * root of A, which is what someone typing it after a reboot expects.
     */
    if (argv[0][0] != '\0' && argv[0][1] == ':' && argv[0][2] == '\0') {
        char verb[3] = "cd";
        char *cd_argv[2] = {verb, argv[0]};
        status = ag_cmd_cd(2, cd_argv);
        found = true;
    }

    for (int i = 0; !found && k_commands[i].name != NULL; i++) {
        if (ag_path_icmp(argv[0], k_commands[i].name) == 0) {
            status = k_commands[i].fn(argc, argv);
            found = true;
        }
    }

    if (redirect >= 0) {
        ag_console_redirect(NULL, NULL);
        ag_vfs_close(redirect);
    }

    /*
     * The Ctrl+C that stopped the command is still in the queue, along with
     * anything else typed while it was scrolling.  Those keys were aimed at the
     * command, not at the next prompt - a terminal drops them for the same
     * reason, and leaving them would mean the prompt comes back with a line
     * somebody typed at something else.
     */
    if (ag_supervisor_interrupted()) {
        ag_console_flush_input();
    }

    if (!found) {
        /* The message every DOS user knows. */
        ag_console_printf("Bad command or file name: %s\n", argv[0]);
    }
    return status;
}

static void show_prompt(prompt_pos_t *pos)
{
    /* Large enough for the longest path the VFS will accept, plus "A:\>". */
    char prompt[AG_PATH_MAX + 8];
    build_prompt(prompt, sizeof(prompt));

    ag_console_lock();
    ag_screen_t *sc = ag_console_screen();
    /* Start the prompt on a fresh line so output never runs into it. */
    if (sc->cur_x != 0) {
        ag_screen_puts(sc, "\n");
    }
    ag_screen_puts(sc, prompt);
    pos->row = sc->cur_y;
    pos->col0 = sc->cur_x;
    ag_console_unlock();
}

void ag_shell_run(void)
{
    ag_lineedit_init(&s_line);
    pick_initial_cwd();

    ag_console_puts("\nType 'help' for a list of commands.\n");

    for (;;) {
        prompt_pos_t pos;
        show_prompt(&pos);
        ag_lineedit_reset(&s_line);
        redraw_line(&s_line, &pos);

        bool done = false;
        while (!done) {
            ag_event_t ev;
            if (!ag_console_read_event(&ev, UINT32_MAX)) {
                continue;
            }

            switch (ag_lineedit_key(&s_line, &ev)) {
            case AG_LINE_CHANGED:
                redraw_line(&s_line, &pos);
                break;

            case AG_LINE_DONE:
                ag_console_puts("\n");
                if (s_line.len > 0) {
                    ag_lineedit_remember(&s_line, s_line.buf);
                    s_last_status = ag_shell_execute(s_line.buf);
                }
                done = true;
                break;

            case AG_LINE_CANCEL:
                ag_console_puts("^C\n");
                done = true;
                break;

            case AG_LINE_EOF:
            case AG_LINE_COMPLETE:
            case AG_LINE_IDLE:
            default:
                break;
            }
        }
    }
}
