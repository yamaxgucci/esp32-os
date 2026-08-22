/*
 * ArgonOS - built-in shell.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/shell.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <argon/audio.h>
#include <argon/board.h>
#include <argon/cmdline.h>
#include <argon/codepage.h>
#include <argon/console.h>
#include <argon/session.h>
#include <argon/display.h>
#include <argon/device.h>
#include <argon/ioclaim.h>
#include <argon/kernel.h>
#include <argon/lineedit.h>
#include <argon/loader.h>
#include <argon/log.h>
#include <argon/module.h>
#include <argon/netmsg.h>
#include <argon/path.h>
#include <argon/probe.h>
#include <argon/power.h>
#include <argon/proc.h>
#include <argon/recovery.h>
#include <argon/shell_path.h>
#include <argon/vfs.h>

#include <argon/btinput.h>
#include <argon/net.h>
#include <argon/port/bt.h>
#include <argon/port/ble.h>
#include <argon/port/io.h>
#include <argon/port/mem.h>
#include <argon/port/net.h>
#include <argon/port/wifi.h>
#include <argon/port/sys.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

#include "boot/platform.h"
#include "core/sysconfig.h"
#include "proc/supervisor.h"
#include "shell/cmd_fs.h"
#include "net/espnow.h"
#include "net/wifimon.h"
#include "shell/cmd_net.h"
#include "shell/cmd_power.h"
#include "shell/cmd_unzip.h"

typedef struct {
    const char *name;
    const char *usage;
    const char *help;
    int (*fn)(int argc, char **argv);
} ag_command_t;

int ag_edit_main(int argc, char **argv);

static char          s_cwd[AG_PATH_MAX] = "/";
static ag_lineedit_t s_line;
static int           s_last_status;
static int           s_script_depth;

#define AG_SHELL_SCRIPT_MAX_DEPTH 8

/* Defined below; used by run/call before the body. */
int ag_shell_run_script(const char *path);

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
    {"/host", 'H'},
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
    char path[AG_PATH_MAX];
    ag_shell_dos_path(s_cwd, path, sizeof(path));
    if (ag_session_is_system()) {
        snprintf(out, n, "(sys) %s>", path);
    } else {
        snprintf(out, n, "(%d) %s>", ag_session_display_number(ag_session_focused()),
                 path);
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
    (void)ag_session_set_cwd(ag_session_focused(), path);
    return AG_OK;
}

static void sync_cwd_from_session(void)
{
    const char *cwd = ag_session_cwd(ag_session_focused());
    if (cwd != NULL && cwd[0] == '/') {
        strncpy(s_cwd, cwd, sizeof(s_cwd) - 1);
        s_cwd[sizeof(s_cwd) - 1] = '\0';
    }
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
            for (int s = 0; s < AG_SESSION_SLOTS; s++) {
                (void)ag_session_set_cwd(s, s_cwd);
            }
            return;
        }
    }
    ag_shell_set_cwd("/");
    for (int s = 0; s < AG_SESSION_SLOTS; s++) {
        (void)ag_session_set_cwd(s, s_cwd);
    }
}

/* ---------------------------------------------------------------------- */
/* Line rendering                                                         */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint16_t row;
    uint16_t col0;
} prompt_pos_t;

static char         s_prompt[AG_PATH_MAX + 16];
static prompt_pos_t s_prompt_pos;

/* Caller holds the console lock. */
static void redraw_line_locked(const ag_lineedit_t *le, const prompt_pos_t *pos)
{
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
}

static void redraw_line(const ag_lineedit_t *le, const prompt_pos_t *pos)
{
    ag_console_lock();
    redraw_line_locked(le, pos);
    ag_console_unlock();
}

/*
 * Log echo calls this with the console lock held after printing a background
 * message over the edit row.  Re-paint the prompt and whatever has been typed.
 */
static void live_restore(void *ctx)
{
    (void)ctx;
    ag_screen_t *sc = ag_console_screen();
    ag_screen_puts(sc, s_prompt);
    s_prompt_pos.row = sc->cur_y;
    s_prompt_pos.col0 = sc->cur_x;
    redraw_line_locked(&s_line, &s_prompt_pos);
}

/* ---------------------------------------------------------------------- */
/* Commands                                                               */
/* ---------------------------------------------------------------------- */

/* Defined after the table it walks. */
static int cmd_help(int argc, char **argv);

static bool narrow_screen(void);

static int cmd_ver(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const ag_sysinfo_t  *si = ag_sysinfo();
    const ag_platform_t *pl = ag_platform();

    ag_console_printf("%s %s (%s)\n", si->os_name, si->os_version, si->build);
    if (narrow_screen()) {
        ag_console_printf("%s rev %u, %ux%u MHz, %s\n", si->chip,
                          (unsigned)pl->chip_revision, (unsigned)si->cpu_cores,
                          (unsigned)(si->cpu_hz / 1000000u), si->profile);
        ag_console_printf("%s, ABI %u.%u, app core %u\n", ag_board()->name,
                          si->abi_major, si->abi_minor, si->app_core);
    } else {
        ag_console_printf("%s rev %u, %u cores at %u MHz, profile %s\n",
                          si->chip, (unsigned)pl->chip_revision,
                          (unsigned)si->cpu_cores,
                          (unsigned)(si->cpu_hz / 1000000u), si->profile);
        /* The board name comes from the board layer: that is where it is
         * decided. */
        ag_console_printf("board %s, ABI %u.%u, application core %u\n",
                          ag_board()->name, si->abi_major, si->abi_minor,
                          si->app_core);
    }
    if (ag_sysconfig_sources()[0] != '\0') {
        ag_console_printf("configured by %s\n", ag_sysconfig_sources());
    }
    return 0;
}

/*
 * Is this a narrow screen?
 *
 * The console is eighty columns unless the board's own display cannot hold
 * eighty: 320 pixels across at 8 pixels a cell is forty.  A table laid out for
 * eighty does not become a narrower table there - it becomes every line split
 * in two, with the tail of each one starting the next, which is harder to read
 * than no table at all.  So the commands that print lists ask.
 */
static bool narrow_screen(void)
{
    const ag_screen_t *s = ag_console_screen();
    return s == NULL || s->cols < 60;
}

static int cmd_mem(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const size_t int_free = ag_port_mem_free(AG_MEM_FAST);
    const size_t int_total = ag_port_mem_total(AG_MEM_FAST);
    const size_t int_block = ag_port_mem_largest(AG_MEM_FAST);
    const size_t psram_free = ag_port_mem_free(AG_MEM_SLOW);
    const size_t psram_total = ag_port_mem_total(AG_MEM_SLOW);

    /*
     * And the same question asked the way an image asks it.
     *
     * "Internal" counts everything the chip has of its own, including memory
     * that can only be reached a word at a time.  An application's data, an
     * emulator's video memory, a filename - all of that is bytes, and on the
     * original ESP32 the two numbers are not the same at all: a board can show
     * seventy kilobytes free and refuse a twenty-five kilobyte image, which is
     * exactly what happened here and cost an afternoon of looking at the wrong
     * figure.
     */
    const size_t byte_free = ag_port_mem_free(AG_MEM_FAST | AG_MEM_BYTE);
    const size_t byte_total = ag_port_mem_total(AG_MEM_FAST | AG_MEM_BYTE);
    const size_t byte_block = ag_port_mem_largest(AG_MEM_FAST | AG_MEM_BYTE);

    if (narrow_screen()) {
        ag_console_printf("           total    free largest\n");
        ag_console_printf("internal %5uK  %5uK  %5uK\n",
                          (unsigned)(int_total / 1024),
                          (unsigned)(int_free / 1024),
                          (unsigned)(int_block / 1024));
        ag_console_printf("bytes    %5uK  %5uK  %5uK\n",
                          (unsigned)(byte_total / 1024),
                          (unsigned)(byte_free / 1024),
                          (unsigned)(byte_block / 1024));
        if (psram_total > 0) {
            ag_console_printf("extended %5uK  %5uK\n",
                              (unsigned)(psram_total / 1024),
                              (unsigned)(psram_free / 1024));
        } else {
            ag_console_puts("extended  none\n");
        }
    } else {
        ag_console_printf("                 total        free     largest\n");
        ag_console_printf("  internal  %8u KB  %8u KB  %8u KB\n",
                          (unsigned)(int_total / 1024),
                          (unsigned)(int_free / 1024),
                          (unsigned)(int_block / 1024));
        ag_console_printf("  bytes     %8u KB  %8u KB  %8u KB\n",
                          (unsigned)(byte_total / 1024),
                          (unsigned)(byte_free / 1024),
                          (unsigned)(byte_block / 1024));
        if (psram_total > 0) {
            ag_console_printf("  extended  %8u KB  %8u KB\n",
                              (unsigned)(psram_total / 1024),
                              (unsigned)(psram_free / 1024));
        } else {
            ag_console_puts("  extended         none\n");
        }
    }
    ag_console_printf("\n  %u KB used by the system\n",
                      (unsigned)((int_total - int_free) / 1024));

    /*
     * The arena is the honest answer to "how much executable memory is there":
     * asking the port for AG_MEM_EXEC reports zero on this chip, because the
     * layer below gives none of that memory to the heap - which is exactly why
     * the arena is reserved at link time.
     */
    ag_console_printf(narrow_screen()
                          ? "  %uK code arena (%s)\n"
                          : "  %u KB reserved for application code (%s)\n",
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
    if (argc >= 2 && ag_path_icmp(argv[1], "recovery") == 0) {
        const ag_err_t err = ag_boot_recovery_set_marker(true);
        if (err != AG_OK) {
            ag_console_printf("boot recovery: %d\n", (int)err);
            return 1;
        }
        ag_console_puts("next boot will skip modules (marker "
                        AG_BOOT_SAFE_PATH ")\n");
        return 0;
    }
    if (argc >= 2 && ag_path_icmp(argv[1], "normal") == 0) {
        const ag_err_t err = ag_boot_recovery_set_marker(false);
        if (err != AG_OK) {
            ag_console_printf("boot normal: %d\n", (int)err);
            return 1;
        }
        ag_console_puts("recovery marker cleared; modules load on next boot\n");
        return 0;
    }

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

    ag_console_printf("\nrecovery: %s",
                      ag_boot_in_recovery() ? "ACTIVE" : "off");
    if (ag_boot_in_recovery()) {
        ag_console_printf(" (%s)", ag_boot_recovery_reason());
    }
    ag_console_printf(", unclean streak %u, marker %s\n",
                      ag_boot_recovery_attempts(),
                      ag_boot_recovery_marker_present() ? "yes" : "no");
    ag_console_puts("  boot recovery  — set marker for next boot\n");
    ag_console_puts("  boot normal    — clear marker\n");
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const uint64_t us = (uint64_t)ag_port_us();
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
    case AG_ENOMEM:
        why = "not enough memory (task stack / heap / code arena)";
        break;
    case AG_EBUSY:   why = "too many applications are loaded"; break;
    case AG_ERANGE:  why = "too many arguments"; break;
    case AG_EINVAL:  why = "is a driver; use 'drv load'"; break;
    case AG_EEXIST:  why = "already loaded"; break;
    case AG_ENFILE:  why = "too many modules are loaded"; break;
    default:         why = "could not be loaded"; break;
    }
    ag_console_printf("%s: %s\n", path, why);
}

static const char *shell_path_env(void)
{
    const ag_cfg_t *cfg = ag_sysconfig();
    if (cfg == NULL) {
        return NULL;
    }
    return ag_cfg_get(cfg, "shell.path", NULL);
}

/*
 * Spawn into the focused user slot.  `path` is the resolved image; argv[0]
 * should be that path (or the name the user typed — loader uses `path`).
 */
static int spawn_in_slot(const char *path, int argc, char **argv,
                         bool detach_only)
{
    if (ag_session_is_system()) {
        ag_console_puts("run: use a user slot (Alt+1..4)\n");
        return 1;
    }

    /*
     * Pin the target slot before spawn: Alt+N can race on the console task and
     * would otherwise bind the new app into whichever slot is focused at the
     * moment bind runs.
     */
    const int target_slot = ag_session_focused();

    ag_pid_t       pid = 0;
    const ag_err_t err = ag_proc_spawn(
        path, argc, argv,
        (uint32_t)AG_SPAWN_BACKGROUND | (uint32_t)AG_SPAWN_NO_SESSION, &pid);
    if (err != AG_OK) {
        print_load_error(path, err);
        return 1;
    }

    char name[32] = "app";
    for (uint32_t i = 0;; i++) {
        ag_procinfo_t info;
        if (ag_proc_info(i, &info) != AG_OK) {
            break;
        }
        if (info.pid == pid) {
            memcpy(name, info.name, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            break;
        }
    }

    if (ag_session_bind_to(pid, name, target_slot) != AG_OK) {
        /* Target slot already has an app — keep whatever free slot spawn used. */
        (void)ag_session_bind(pid, name);
    }

    const int slot = ag_session_slot_of(pid);
    ag_console_printf("started pid %u in slot %d (loading)\n", (unsigned)pid,
                      slot >= 0 ? ag_session_display_number(slot) : -1);

    /*
     * Focus the new app only if the user stayed on the launch slot.  Alt+N
     * during a long load must not yank them back from another slot.
     */
    if (!detach_only && slot >= 0 && ag_session_focused() == target_slot) {
        (void)ag_session_focus(slot);
    }
    return 0;
}

static int launch_resolved(const char *path, int argc, char **argv,
                           bool detach_only)
{
    if (ag_shell_is_script(path)) {
        if (argc > 1) {
            ag_console_puts("script: arguments are ignored\n");
        }
        return ag_shell_run_script(path);
    }
    return spawn_in_slot(path, argc, argv, detach_only);
}

static int cmd_run(int argc, char **argv)
{
    bool detach_only = false;
    int  first = 1;

    if (argc > 1 && ag_path_icmp(argv[1], "/b") == 0) {
        detach_only = true;
        first = 2;
    }
    if (argc <= first) {
        ag_console_puts("usage: run [/b] <file> [arguments]\n");
        ag_console_puts("  starts in the current user slot (or a free one)\n");
        ag_console_puts("  /b  same, without switching focus to the app\n");
        ag_console_puts("  .bat/.cmd run as shell scripts (line by line)\n");
        ag_console_puts("  Alt+1..4 / fg <slot|pid>; Ctrl+\\ = system shell\n");
        return 1;
    }

    char resolved[AG_PATH_MAX];
    const ag_err_t rerr =
        ag_shell_resolve_cmd(argv[first], s_cwd, shell_path_env(), resolved,
                             sizeof(resolved));
    if (rerr != AG_OK) {
        print_load_error(argv[first], rerr);
        return 1;
    }
    argv[first] = resolved;
    return launch_resolved(resolved, argc - first, &argv[first], detach_only);
}

static int cmd_call(int argc, char **argv)
{
    if (argc != 2) {
        ag_console_puts("usage: call <script.bat|.cmd>\n");
        ag_console_puts("  runs a shell command script (same as typing its name)\n");
        return 1;
    }
    char resolved[AG_PATH_MAX];
    const ag_err_t err =
        ag_shell_resolve_cmd(argv[1], s_cwd, shell_path_env(), resolved,
                             sizeof(resolved));
    if (err != AG_OK) {
        ag_console_printf("%s: not found\n", argv[1]);
        return 1;
    }
    if (!ag_shell_is_script(resolved)) {
        ag_console_puts("call: not a .bat/.cmd script\n");
        return 1;
    }
    return ag_shell_run_script(resolved);
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
    ag_port_restart();
    return 0;
}

static int cmd_ps(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ag_console_puts(
        "  pid  name              state        prio    memory\n");

    uint32_t shown = 0;
    for (uint32_t i = 0;; i++) {
        ag_procinfo_t info;
        if (ag_proc_info(i, &info) != AG_OK) {
            break;
        }
        ag_console_printf("  %-4u %-17s %-12s %-7s %5u KB%s\n",
                          (unsigned)info.pid, info.name,
                          ag_proc_state_name(info.state),
                          ag_proc_prio_name((ag_proc_prio_t)info.priority),
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

    const bool narrow = narrow_screen();

    ag_console_puts(narrow ? "name     class    driver\n"
                           : "name      class     driver      size       flags\n");

    uint32_t shown = 0;
    for (uint32_t i = 0;; i++) {
        ag_devinfo_t info;
        if (ag_dev_info(i, AG_DEV_ANY, &info) != AG_OK) {
            break;
        }

        if (narrow) {
            /* Size and flags are one `dev <name>` away, and that fits. */
            ag_console_printf("%-8s %-8s %s\n", info.name,
                              ag_dev_class_name(info.cls), info.driver);
            shown++;
            continue;
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

#define DRV_CFG_PATH     "/sys/SYSTEM.CFG"
/* Lowercase: LittleFS is case-sensitive; c:\drv\… resolves to /sys/drv/… */
#define DRV_DIR_PATH     "/sys/drv"
#define DRV_CFG_MAX      4096
#define DRV_COPY_CHUNK   4096

static int cfg_has_device_line(const char *text, const char *dos_path)
{
    const char *p = text;
    int         in_modules = 0;

    while (*p) {
        const char *line = p;
        const char *eol = strchr(p, '\n');
        size_t      len = eol ? (size_t)(eol - p) : strlen(p);
        char        buf[192];
        char       *s;
        char       *eq;

        if (len >= sizeof(buf)) {
            len = sizeof(buf) - 1u;
        }
        memcpy(buf, line, len);
        buf[len] = '\0';
        if (len > 0 && buf[len - 1u] == '\r') {
            buf[len - 1u] = '\0';
        }

        s = buf;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (*s == ';' || *s == '#' || *s == '\0') {
            /* skip */
        } else if (*s == '[') {
            char *end = strchr(s, ']');
            if (end != NULL) {
                *end = '\0';
                in_modules = (ag_path_icmp(s + 1, "modules") == 0);
            } else {
                in_modules = 0;
            }
        } else if (in_modules) {
            eq = strchr(s, '=');
            if (eq != NULL) {
                char *key = s;
                /*
                 * Where the value starts, taken before anything moves.  It
                 * used to be computed as eq + 1 *after* the loop below had
                 * walked eq backwards over the spaces in front of the '=', so
                 * for the one spelling this file is actually written in -
                 * `device = path`, with spaces - it pointed at the byte that
                 * had just been set to nul.  Every value read as empty, no
                 * line ever matched, and `drv install` of a driver that was
                 * already installed added it again: SYSTEM.CFG grew a second
                 * copy of the same module and loaded it twice on every boot.
                 */
                char *val = eq + 1;

                *eq = '\0';
                while (eq > key && (eq[-1] == ' ' || eq[-1] == '\t')) {
                    eq--;
                    *eq = '\0';
                }
                while (*val == ' ' || *val == '\t') {
                    val++;
                }
                if (ag_path_icmp(key, "device") == 0 &&
                    ag_path_icmp(val, dos_path) == 0) {
                    return 1;
                }
            }
        }

        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
    return 0;
}

#if AG_PORT_HAS_WIFI || AG_PORT_HAS_BT

/* Case-insensitive compare of a fixed length; the section name in the file may
 * be in any case and there is no such helper in argon/path.h. */
static int ag_path_icmpn_local(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return (int)((unsigned char)ca) - (int)((unsigned char)cb);
        }
    }
    return 0;
}

/* One hex digit, or -1.  Bluetooth addresses arrive as text. */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/*
 * Replace one section of SYSTEM.CFG, or remove it when `body` is NULL.
 *
 * Rewritten rather than appended to, unlike [modules]: a second `device =`
 * line is another module to load, and a second `ssid =` line is a
 * contradiction about the same thing.  Sections other than the named one are
 * copied through untouched, including the ones this system does not know
 * about - a config file is not only ours to keep.
 */
/*
 * Rewrites one section of SYSTEM.CFG, keeping the rest as it stands.
 *
 * The two buffers come from the heap, and that is a fix rather than a taste.
 * As stack arrays they were four kilobytes each in a task with twelve, and the
 * shell reaches this function several frames deep: `wifi connect` remembers the
 * network here, and remembering it overflowed the stack of the task the shell
 * runs in.
 *
 * What that looked like from outside was a board that restarts when a Wi-Fi
 * password is typed - so the password was the suspect, then the memory the radio
 * takes, and neither had anything to do with it.  What said otherwise was one
 * line the chip prints and nobody had been reading:
 *
 *     ***ERROR*** A stack overflow in task main has been detected.
 *
 * Rewriting a configuration file happens when somebody types a command, so a
 * heap allocation and a free cost nothing that matters, and eight kilobytes of
 * stack in a twelve kilobyte task was never going to be safe for long.
 */
static ag_err_t cfg_replace_section(const char *section, const char *body)
{
    size_t      used = 0;
    size_t      len = 0;

    char *const text = (char *)ag_port_alloc(DRV_CFG_MAX * 2u,
                                             AG_MEM_FAST | AG_MEM_BYTE);
    if (text == NULL) {
        return -AG_ENOMEM;
    }
    char *const out = text + DRV_CFG_MAX;

    ag_handle_t h = ag_vfs_open(DRV_CFG_PATH, NULL, AG_O_RDONLY);

    if (h >= 0) {
        const int32_t n = ag_vfs_read(h, text, DRV_CFG_MAX - 1u);
        ag_vfs_close(h);
        if (n < 0) {
            ag_port_free(text);
            return (ag_err_t)n;
        }
        used = (size_t)n;
    }
    text[used] = '\0';

    const size_t seclen = strlen(section);
    bool         drop = false;
    const char  *p = text;
    while (p < text + used) {
        const char  *eol = strchr(p, '\n');
        const size_t line = (eol != NULL) ? (size_t)(eol - p) + 1u
                                          : (size_t)(text + used - p);
        const char  *s = p;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (*s == '[') {
            const char *close = strchr(s, ']');
            drop = (close != NULL) && ((size_t)(close - s) == seclen + 1u) &&
                   (ag_path_icmpn_local(s + 1, section, seclen) == 0);
        }
        if (!drop) {
            if (len + line >= DRV_CFG_MAX) {
                ag_port_free(text);
                return -AG_ENOSPC;
            }
            memcpy(out + len, p, line);
            len += line;
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }

    if (body != NULL && body[0] != '\0') {
        if (len > 0 && out[len - 1u] != '\n') {
            out[len++] = '\n';
        }
        const int n = snprintf(out + len, DRV_CFG_MAX - len, "[%s]\n%s",
                               section, body);
        if (n < 0 || (size_t)n >= DRV_CFG_MAX - len) {
            ag_port_free(text);
            return -AG_ENOSPC;
        }
        len += (size_t)n;
    }

    h = ag_vfs_open(DRV_CFG_PATH, NULL, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        ag_port_free(text);
        return (ag_err_t)h;
    }
    const int32_t w = ag_vfs_write(h, out, len);
    (void)ag_vfs_close(h);
    ag_port_free(text);
    return (w < 0) ? (ag_err_t)w : AG_OK;
}

#endif /* AG_PORT_HAS_WIFI || AG_PORT_HAS_BT */

#if AG_PORT_HAS_WIFI
static ag_err_t cfg_ensure_wifi(const char *ssid, const char *pass,
                                const uint8_t *bssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return cfg_replace_section("wifi", NULL);
    }

    /*
     * The access point is written down when one was named, because a board that
     * has to be told again after every power cut has not been told.  Absent
     * means what it has always meant: join the network, whichever box answers.
     */
    char pinned[32] = ""; /* "bssid = aa:bb:cc:dd:ee:ff\n" and a terminator */
    if (bssid != NULL) {
        char text[18];
        if (ag_mac_str(bssid, text, sizeof(text)) > 0) {
            snprintf(pinned, sizeof(pinned), "bssid = %s\n", text);
        }
    }

    char body[200];
    snprintf(body, sizeof(body), "ssid = %s\npass = %s\n%s", ssid,
             (pass != NULL) ? pass : "", pinned);
    return cfg_replace_section("wifi", body);
}
#endif

#if AG_PORT_WIFI_HAS_AP
/*
 * The point's own section, [ap], kept apart from [wifi] on purpose: the two are
 * written independently - a board can be told to run a point without touching
 * the network it joins, and the reverse - and cfg_replace_section rewrites a
 * whole section at a time.  One section for both would mean each write had to
 * carry the other's keys or lose them.
 */
static ag_err_t cfg_ensure_ap(const char *ssid, const char *pass,
                              unsigned channel, bool hidden)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return cfg_replace_section("ap", NULL);
    }
    char body[200];
    snprintf(body, sizeof(body),
             "ssid = %s\npass = %s\nchannel = %u\nhidden = %d\n", ssid,
             (pass != NULL) ? pass : "", channel, hidden ? 1 : 0);
    return cfg_replace_section("ap", body);
}
#endif

#if AG_PORT_HAS_BT
static ag_err_t cfg_ensure_bt(const char *addr, int addr_type)
{
    if (addr == NULL || addr[0] == '\0') {
        return cfg_replace_section("bt", NULL);
    }
    char body[96];
    snprintf(body, sizeof(body), "keyboard = %s\ntype = %d\n", addr,
             addr_type);
    return cfg_replace_section("bt", body);
}
#endif

static ag_err_t cfg_ensure_device(const char *dos_path)
{
    char        text[DRV_CFG_MAX];
    ag_handle_t h;
    int32_t     n;
    size_t      used = 0;
    char        add[192];
    size_t      add_len;

    h = ag_vfs_open(DRV_CFG_PATH, NULL, AG_O_RDONLY);
    if (h >= 0) {
        n = ag_vfs_read(h, text, sizeof(text) - 1u);
        ag_vfs_close(h);
        if (n < 0) {
            return (ag_err_t)n;
        }
        used = (size_t)n;
        text[used] = '\0';
        if (cfg_has_device_line(text, dos_path)) {
            return AG_OK;
        }
    } else if (h != -AG_ENOENT) {
        return (ag_err_t)h;
    } else {
        text[0] = '\0';
        used = 0;
    }

    /* Extra [modules] block is fine: ag_cfg_next walks every device=. */
    add_len = (size_t)snprintf(add, sizeof(add), "%s[modules]\ndevice = %s\n",
                               (used > 0 && text[used - 1u] != '\n') ? "\n" : "",
                               dos_path);
    if (add_len >= sizeof(add) || used + add_len + 1u > sizeof(text)) {
        return -AG_ENOSPC;
    }
    memcpy(text + used, add, add_len + 1u);
    used += add_len;

    h = ag_vfs_open(DRV_CFG_PATH, NULL, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        return (ag_err_t)h;
    }
    n = ag_vfs_write(h, text, used);
    (void)ag_vfs_close(h);
    if (n < 0) {
        return (ag_err_t)n;
    }
    if ((size_t)n != used) {
        return -AG_EIO;
    }
    return AG_OK;
}

static ag_err_t cfg_remove_device(const char *dos_path)
{
    char        text[DRV_CFG_MAX];
    char        out[DRV_CFG_MAX];
    ag_handle_t h;
    int32_t     n;
    size_t      used;
    size_t      out_used = 0;
    const char *p;
    int         in_modules = 0;
    int         changed = 0;

    h = ag_vfs_open(DRV_CFG_PATH, NULL, AG_O_RDONLY);
    if (h < 0) {
        return (h == -AG_ENOENT) ? AG_OK : (ag_err_t)h;
    }
    n = ag_vfs_read(h, text, sizeof(text) - 1u);
    ag_vfs_close(h);
    if (n < 0) {
        return (ag_err_t)n;
    }
    used = (size_t)n;
    text[used] = '\0';

    p = text;
    while (*p) {
        const char *line = p;
        const char *eol = strchr(p, '\n');
        size_t      len = eol ? (size_t)(eol - p) : strlen(p);
        char        buf[192];
        char        raw[192];
        char       *s;
        char       *eq;
        int         drop = 0;
        size_t      copy_len;

        if (len >= sizeof(buf)) {
            len = sizeof(buf) - 1u;
        }
        memcpy(raw, line, len);
        raw[len] = '\0';
        memcpy(buf, raw, len + 1u);
        if (len > 0 && buf[len - 1u] == '\r') {
            buf[len - 1u] = '\0';
        }

        s = buf;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end != NULL) {
                *end = '\0';
                in_modules = (ag_path_icmp(s + 1, "modules") == 0);
            } else {
                in_modules = 0;
            }
        } else if (in_modules && *s != ';' && *s != '#' && *s != '\0') {
            eq = strchr(s, '=');
            if (eq != NULL) {
                char *key = s;
                /*
                 * Where the value starts, taken before anything moves.  It
                 * used to be computed as eq + 1 *after* the loop below had
                 * walked eq backwards over the spaces in front of the '=', so
                 * for the one spelling this file is actually written in -
                 * `device = path`, with spaces - it pointed at the byte that
                 * had just been set to nul.  Every value read as empty, no
                 * line ever matched, and `drv install` of a driver that was
                 * already installed added it again: SYSTEM.CFG grew a second
                 * copy of the same module and loaded it twice on every boot.
                 */
                char *val = eq + 1;

                *eq = '\0';
                while (eq > key && (eq[-1] == ' ' || eq[-1] == '\t')) {
                    eq--;
                    *eq = '\0';
                }
                while (*val == ' ' || *val == '\t') {
                    val++;
                }
                if (ag_path_icmp(key, "device") == 0 &&
                    ag_path_icmp(val, dos_path) == 0) {
                    drop = 1;
                    changed = 1;
                }
            }
        }

        copy_len = eol ? (size_t)(eol - line + 1) : strlen(line);
        if (!drop) {
            if (out_used + copy_len + 1u > sizeof(out)) {
                return -AG_ENOSPC;
            }
            memcpy(out + out_used, line, copy_len);
            out_used += copy_len;
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }

    if (!changed) {
        return AG_OK;
    }

    h = ag_vfs_open(DRV_CFG_PATH, NULL, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        return (ag_err_t)h;
    }
    n = ag_vfs_write(h, out, out_used);
    (void)ag_vfs_close(h);
    if (n < 0) {
        return (ag_err_t)n;
    }
    return AG_OK;
}

static ag_err_t drv_copy_file(const char *src_abs, const char *dst_abs)
{
    static uint8_t chunk[DRV_COPY_CHUNK];
    ag_handle_t    in;
    ag_handle_t    out;
    int32_t        n;

    in = ag_vfs_open(src_abs, NULL, AG_O_RDONLY);
    if (in < 0) {
        return (ag_err_t)in;
    }
    out = ag_vfs_open(dst_abs, NULL, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (out < 0) {
        ag_vfs_close(in);
        return (ag_err_t)out;
    }
    while ((n = ag_vfs_read(in, chunk, sizeof(chunk))) > 0) {
        size_t left = (size_t)n;
        size_t off = 0;
        while (left > 0) {
            const int32_t w = ag_vfs_write(out, chunk + off, left);
            if (w < 0) {
                ag_vfs_close(in);
                ag_vfs_close(out);
                return (ag_err_t)w;
            }
            if (w == 0) {
                ag_vfs_close(in);
                ag_vfs_close(out);
                return -AG_ENOSPC;
            }
            off += (size_t)w;
            left -= (size_t)w;
        }
    }
    ag_vfs_close(in);
    ag_vfs_close(out);
    return (n < 0) ? (ag_err_t)n : AG_OK;
}

static int drv_install_one(const char *spec)
{
    char        src_res[AG_PATH_MAX];
    char        drv_dir[AG_PATH_MAX];
    char        dst_abs[AG_PATH_MAX];
    char        dos_path[64];
    const char *base;
    ag_err_t    err;
    ag_stat_t   st;

    err = ag_path_resolve(spec, s_cwd, src_res, sizeof(src_res));
    if (err != AG_OK) {
        ag_console_printf("%s: %d\n", spec, (int)err);
        return 1;
    }
    base = ag_path_basename(src_res);
    if (base == NULL || base[0] == '\0') {
        ag_console_puts("drv install: bad name\n");
        return 1;
    }
    if (ag_vfs_stat(src_res, NULL, &st) != AG_OK) {
        ag_console_printf("%s: file not found\n", spec);
        if (strncmp(src_res, "/sd", 3) == 0 &&
            ag_vfs_stat("/sd", NULL, &st) != AG_OK) {
            ag_console_puts("A: is not mounted (argon run -Sd)\n");
        }
        return 1;
    }

    /* Empty leftover only.  A populated C:\DRV cannot be rmdir'd. */
    (void)ag_vfs_rmdir("/sys/DRV", NULL);
    if (ag_vfs_realpath(DRV_DIR_PATH, NULL, drv_dir, sizeof(drv_dir)) !=
        AG_OK) {
        err = ag_vfs_mkdir(DRV_DIR_PATH, NULL);
        if (err != AG_OK && err != -AG_EEXIST) {
            ag_console_printf("mkdir %s: %d\n", DRV_DIR_PATH, (int)err);
            return 1;
        }
        snprintf(drv_dir, sizeof(drv_dir), "%s", DRV_DIR_PATH);
    }
    err = ag_path_join(drv_dir, base, dst_abs, sizeof(dst_abs));
    if (err != AG_OK) {
        ag_console_printf("drv install: %d\n", (int)err);
        return 1;
    }

    err = drv_copy_file(src_res, dst_abs);
    if (err != AG_OK) {
        if (err == -AG_ENOENT) {
            ag_console_printf("copy: cannot create %s\n", dst_abs);
        } else {
            ag_console_printf("copy: %d\n", (int)err);
        }
        return 1;
    }

    snprintf(dos_path, sizeof(dos_path), "c:\\drv\\%s", base);
    err = cfg_ensure_device(dos_path);
    if (err != AG_OK) {
        ag_console_printf("SYSTEM.CFG: %d\n", (int)err);
        return 1;
    }

    err = ag_module_load(dst_abs, NULL);
    if (err != AG_OK) {
        print_load_error(dst_abs, err);
        return 1;
    }
    ag_console_printf("installed %s (loaded; autoload on boot)\n", dos_path);
    return 0;
}

static int cmd_drv_install(int argc, char **argv)
{
    int failed = 0;
    int i;

    if (argc < 3) {
        ag_console_puts("usage: drv install <file.sys> [file.sys ...]\n");
        return 1;
    }
    for (i = 2; i < argc; i++) {
        if (drv_install_one(argv[i]) != 0) {
            failed++;
        }
    }
    return failed ? 1 : 0;
}

static int cmd_drv_uninstall(int argc, char **argv)
{
    char        dst_abs[AG_PATH_MAX];
    char        dos_path[64];
    const char *name;
    ag_err_t    err;
    int         i;

    if (argc != 3) {
        ag_console_puts("usage: drv uninstall <name|file.sys>\n");
        return 1;
    }
    name = ag_path_basename(argv[2]);
    if (name == NULL || name[0] == '\0') {
        name = argv[2];
    }

    /* Unload by module header name when possible; also try basename. */
    err = ag_module_unload(argv[2]);
    if (err == -AG_ENOENT) {
        for (i = 0;; i++) {
            ag_modinfo_t info;
            if (ag_module_info((uint32_t)i, &info) != AG_OK) {
                break;
            }
            if (ag_path_icmp(info.name, argv[2]) == 0 ||
                ag_path_icmp(ag_path_basename(info.path), name) == 0) {
                err = ag_module_unload(info.name);
                break;
            }
        }
    }
    if (err != AG_OK && err != -AG_ENOENT) {
        ag_console_printf("unload: %d\n", (int)err);
        return 1;
    }

    snprintf(dos_path, sizeof(dos_path), "c:\\drv\\%s", name);
    if (ag_path_ext(name) == NULL) {
        /* Header name like PCMVIRT → file pcmvirt.sys is unknown; try .sys */
        snprintf(dos_path, sizeof(dos_path), "c:\\drv\\%s.sys", name);
    }
    (void)cfg_remove_device(dos_path);

    /* Also remove exact basename form if user passed file.sys */
    if (ag_path_ext(name) != NULL) {
        snprintf(dos_path, sizeof(dos_path), "c:\\drv\\%s", name);
        (void)cfg_remove_device(dos_path);
        err = ag_path_join(DRV_DIR_PATH, name, dst_abs, sizeof(dst_abs));
        if (err == AG_OK) {
            (void)ag_vfs_unlink(dst_abs, NULL);
        }
    } else {
        char lower[40];
        size_t k;
        for (k = 0; name[k] && k + 1u < sizeof(lower); k++) {
            char c = name[k];
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            lower[k] = c;
        }
        lower[k] = '\0';
        snprintf(dos_path, sizeof(dos_path), "c:\\drv\\%s.sys", lower);
        (void)cfg_remove_device(dos_path);
        err = ag_path_join(DRV_DIR_PATH, lower, dst_abs, sizeof(dst_abs));
        if (err == AG_OK) {
            size_t n = strlen(dst_abs);
            if (n + 4u < sizeof(dst_abs)) {
                memcpy(dst_abs + n, ".sys", 5);
                (void)ag_vfs_unlink(dst_abs, NULL);
            }
        }
        /* Uppercase .SYS variant from install basename */
        snprintf(dst_abs, sizeof(dst_abs), "%s/%s.SYS", DRV_DIR_PATH, name);
        (void)ag_vfs_unlink(dst_abs, NULL);
        snprintf(dos_path, sizeof(dos_path), "c:\\drv\\%s.SYS", name);
        (void)cfg_remove_device(dos_path);
    }

    ag_console_printf("uninstalled %s\n", argv[2]);
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

    if (argc >= 2 && ag_path_icmp(argv[1], "install") == 0) {
        return cmd_drv_install(argc, argv);
    }

    if (argc >= 2 && ag_path_icmp(argv[1], "uninstall") == 0) {
        return cmd_drv_uninstall(argc, argv);
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
        ag_console_puts(
            "usage: drv [load|unload|install|uninstall|probe] ...\n");
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
#if AG_PORT_HAS_WIFI

static const char *wifi_auth_name(ag_wifi_auth_t a)
{
    switch (a) {
    case AG_WIFI_OPEN:       return "open";
    case AG_WIFI_WEP:        return "WEP";
    case AG_WIFI_WPA:        return "WPA";
    case AG_WIFI_WPA2:       return "WPA2";
    case AG_WIFI_WPA3:       return "WPA3";
    case AG_WIFI_ENTERPRISE: return "802.1X";
    default:                 return "?";
    }
}

static const char *wifi_state_name(ag_wifi_state_t s)
{
    switch (s) {
    case AG_WIFI_OFF:     return "off";
    case AG_WIFI_IDLE:    return "on, not joined";
    case AG_WIFI_JOINING: return "joining";
    default:              return "joined";
    }
}

static int wifi_status(void)
{
    ag_port_wifi_status_t st;
    const ag_err_t        err = ag_port_wifi_status(&st);
    if (err != AG_OK) {
        ag_console_printf("wifi: %s\n", ag_loader_api()->sys->strerror(err));
        return 1;
    }

    ag_console_printf("radio %s\n", wifi_state_name(st.state));
    if (st.ssid[0] != '\0') {
        ag_console_printf("ssid %s", st.ssid);
        if (st.state == AG_WIFI_JOINED) {
            ag_console_printf(", %d dBm, channel %u", (int)st.rssi,
                              (unsigned)st.channel);
        }
        ag_console_puts("\n");
    }

    /*
     * Which access point, printed whenever there is one to print.  With two
     * boxes answering to one name - and that is the usual arrangement in a flat
     * - "joined" says nothing about whether the link is any good, and this is
     * the line that does.
     */
    static const uint8_t k_no_ap[6] = {0};
    if (memcmp(st.bssid, k_no_ap, sizeof(k_no_ap)) != 0) {
        char text[18];
        (void)ag_mac_str(st.bssid, text, sizeof(text));
        ag_console_printf("access point %s%s\n", text,
                          st.pinned ? " (pinned)" : "");
    } else if (st.pinned) {
        ag_console_puts("pinned to an access point that has not answered\n");
    }
    if (st.state != AG_WIFI_JOINED && st.last_reason != 0) {
        ag_console_printf("last failure: %s\n",
                          ag_port_wifi_reason(st.last_reason));
    }
    if (st.attempts > 1u) {
        ag_console_printf("%u attempts\n", (unsigned)st.attempts);
    }

    uint32_t addr = 0;
    if (ag_net_ready() && ag_port_net_ifaddr(&addr) == AG_OK) {
        ag_console_printf("address %u.%u.%u.%u\n", (unsigned)(addr >> 24),
                          (unsigned)((addr >> 16) & 0xffu),
                          (unsigned)((addr >> 8) & 0xffu),
                          (unsigned)(addr & 0xffu));
    } else if (st.state == AG_WIFI_JOINED) {
        ag_console_puts("no address yet (DHCP)\n");
    }

#if AG_PORT_WIFI_HAS_AP
    /*
     * The other direction, when the board is offering a network as well as (or
     * instead of) using one.  Its address is fixed and does not come from DHCP,
     * so the line above says nothing about it - this is the one that does.
     */
    ag_port_wifi_ap_status_t ap;
    if (ag_port_wifi_ap_status(&ap) == AG_OK && ap.on) {
        ag_console_printf("access point \"%s\"%s%s, channel %u\n", ap.ssid,
                          ap.secured ? "" : " (open)",
                          ap.hidden ? " (hidden)" : "", (unsigned)ap.channel);
        ag_console_printf("  at %u.%u.%u.%u, %u client%s\n",
                          (unsigned)(ap.ip >> 24),
                          (unsigned)((ap.ip >> 16) & 0xffu),
                          (unsigned)((ap.ip >> 8) & 0xffu),
                          (unsigned)(ap.ip & 0xffu), (unsigned)ap.clients,
                          (ap.clients == 1u) ? "" : "s");
    }
#endif
    return 0;
}

/*
 * What the last scan found, so that connecting can name a number.
 *
 * An SSID can be thirty-two characters and often is; typing one to join a
 * network one has just been shown is work the machine should be doing.  So the
 * names are kept and `wifi connect #3` means the third line of the last scan.
 *
 * On the heap, asked for the first time anything scans.  Sixteen names is half a
 * kilobyte, and half a kilobyte of static data is more than the S3 firmware has
 * left in its data segment - which is the fourth time on this branch that
 * reserving memory for a possibility turned out to be the expensive way to do
 * it.  A machine that never scans pays two pointers.
 */
#define WIFI_SCAN_MAX 16

static char   (*s_scan_names)[AG_WIFI_SSID_MAX + 1];
static uint8_t (*s_scan_bssid)[6];
static uint32_t s_scan_count;

static int wifi_scan(void)
{
    ag_port_wifi_ap_t aps[WIFI_SCAN_MAX];
    uint32_t          found = 0;

    ag_console_puts("scanning...\n");
    const ag_err_t err = ag_port_wifi_scan(aps, WIFI_SCAN_MAX, &found);
    if (err == -AG_EBUSY) {
        /*
         * The radio cannot scan and associate at once, and a board that has
         * been told a network it cannot join retries for ever - so "busy" is
         * the answer every time, which reads as a broken command.  Say which
         * of the two it is and what stops it.
         */
        ag_port_wifi_status_t st;
        if (ag_port_wifi_status(&st) == AG_OK && st.ssid[0] != '\0') {
            ag_console_printf("busy joining %s\n", st.ssid);
        } else {
            ag_console_puts("the radio is busy\n");
        }
        ag_console_puts("  `wifi` for the reason, `wifi forget` to stop and "
                        "scan\n");
        return 1;
    }
    if (err != AG_OK) {
        ag_console_printf("scan: %s\n", ag_loader_api()->sys->strerror(err));
        return 1;
    }

    const uint32_t shown = (found < WIFI_SCAN_MAX) ? found : WIFI_SCAN_MAX;

    if (s_scan_names == NULL && shown != 0u) {
        s_scan_names = ag_port_alloc(
            (size_t)WIFI_SCAN_MAX * (AG_WIFI_SSID_MAX + 1u),
            AG_MEM_FAST | AG_MEM_BYTE);
    }
    if (s_scan_bssid == NULL && shown != 0u) {
        s_scan_bssid = ag_port_alloc((size_t)WIFI_SCAN_MAX * 6u,
                                     AG_MEM_FAST | AG_MEM_BYTE);
    }
    s_scan_count = 0;

    const bool narrow = narrow_screen();
    ag_console_puts(narrow ? "  # ssid            ch  dBm auth\n"
                           : "  # ssid                             ch   dBm  auth\n");

    for (uint32_t i = 0; i < shown; i++) {
        const char *name = aps[i].ssid[0] != '\0' ? aps[i].ssid : "(hidden)";

        /*
         * Two lines with one name is not a fault and it is the usual case: an
         * access point with two radios, or two access points serving the same
         * network.  You join the network and the chip picks whichever it hears
         * best, so either number does the same thing - but showing the tail of
         * the hardware address for those lines says *why* there are two, which
         * is the part that otherwise looks like a bug.
         */
        bool dup = false;
        for (uint32_t j = 0; j < shown && !dup; j++) {
            if (j != i && ag_path_icmp(aps[j].ssid, aps[i].ssid) == 0) {
                dup = true;
            }
        }

        ag_console_printf(narrow ? "%3u %-15s %2u %4d %s"
                                 : "%3u %-32s %2u  %4d  %s",
                          (unsigned)(i + 1u), name,
                          (unsigned)aps[i].channel, (int)aps[i].rssi,
                          wifi_auth_name(aps[i].auth));
        if (dup) {
            ag_console_printf(" %02x%02x%02x", (unsigned)aps[i].bssid[3],
                              (unsigned)aps[i].bssid[4],
                              (unsigned)aps[i].bssid[5]);
        }
        ag_console_puts("\n");

        if (s_scan_names != NULL) {
            snprintf(s_scan_names[i], AG_WIFI_SSID_MAX + 1u, "%.32s", aps[i].ssid);
            s_scan_count = i + 1u;
        }
        if (s_scan_bssid != NULL) {
            memcpy(s_scan_bssid[i], aps[i].bssid, 6u);
        }
    }

    if (found > shown) {
        ag_console_printf("%u more not shown\n", (unsigned)(found - shown));
    } else if (found == 0) {
        ag_console_puts("nothing in range\n");
    } else {
        ag_console_puts("join one with: wifi connect #<number> [password]\n");
    }
    return 0;
}

/*
 * "#3", or "3", into the name the third line of the last scan carried.
 *
 * Returns the argument unchanged when it is not a number, so a name that happens
 * to start with a digit still works.
 */
/*
 * A name, and which line of the last scan it came from (0 when the caller
 * typed a name rather than a number - there is then no one access point to
 * speak of, only a network).
 */
static const char *wifi_named_line(const char *arg, uint32_t *line_out)
{
    if (line_out != NULL) {
        *line_out = 0;
    }
    const char *digits = (arg[0] == '#') ? arg + 1 : arg;
    if (digits[0] < '1' || digits[0] > '9') {
        return arg;
    }
    for (const char *p = digits; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return arg; /* "5GHz-Guest" is a name, not a number */
        }
    }

    const uint32_t n = (uint32_t)atoi(digits);
    if (s_scan_names == NULL || n == 0u || n > s_scan_count) {
        ag_console_printf("no #%u in the last scan; run `wifi scan` first\n",
                          (unsigned)n);
        return NULL;
    }
    ag_console_printf("#%u is \"%s\"\n", (unsigned)n, s_scan_names[n - 1u]);
    if (line_out != NULL) {
        *line_out = n;
    }
    return s_scan_names[n - 1u];
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        return wifi_status();
    }

    /*
     * On and off mean started and stopped, not associated and disassociated.
     * The distinction matters on this chip more than the wording suggests:
     * a stopped radio gives back about seventy kilobytes, which is the
     * difference between a board that can run an application and one that
     * cannot.  Both radios are in every image; only one usually runs.
     */
    if (ag_path_icmp(argv[1], "on") == 0) {
        const size_t before = ag_port_mem_free(AG_MEM_FAST);

        const ag_err_t err = ag_net_init();
        if (err != AG_OK) {
            ag_console_printf("wifi on: %s\n",
                              ag_loader_api()->sys->strerror(err));
            return 1;
        }

        /*
         * What it cost and what is left, every time.
         *
         * The radio takes about seventy kilobytes to start and more again to
         * associate and take a lease, and the parts of that which fail do not
         * fail politely: an allocation refused inside the Wi-Fi stack or lwIP
         * ends in abort(), which on this chip is a reset.  From the outside that
         * looks like the board restarting when a password is typed - a password
         * problem, and it is not one.
         *
         * So the numbers are printed rather than left to be guessed, and a
         * margin too thin to associate in says so before anyone tries.
         */
        const size_t after = ag_port_mem_free(AG_MEM_FAST);
        ag_console_printf("radio on: took %u KB, %u KB free\n",
                          (unsigned)((before - after) / 1024u),
                          (unsigned)(after / 1024u));
        if (after < 24u * 1024u) {
            ag_console_puts(
                "  that is thin: association and DHCP want ~16 KB more.\n"
                "  if the board resets while joining, this is why - stop what\n"
                "  else is running, or use a build with less in it.\n");
        }

        const char *ssid = ag_cfg_get(ag_sysconfig(), "wifi.ssid", NULL);
        if (ssid != NULL && ssid[0] != '\0') {
            /* Including the access point, if one was pinned: `wifi on` has to
             * mean the same thing as a boot with the same configuration. */
            uint8_t     bssid[6];
            const char *pinned = ag_cfg_get(ag_sysconfig(), "wifi.bssid", NULL);
            const bool  have_pin =
                (pinned != NULL) && ag_mac_parse(pinned, bssid);

            (void)ag_port_wifi_connect(
                ssid, ag_cfg_get(ag_sysconfig(), "wifi.pass", ""),
                have_pin ? bssid : NULL);
            ag_console_printf("joining %s%s\n", ssid,
                              have_pin ? " (pinned)" : "");
        }
        return 0;
    }

    if (ag_path_icmp(argv[1], "scan") == 0) {
        return wifi_scan();
    }

    if (ag_path_icmp(argv[1], "connect") == 0) {
        if (argc < 3) {
            ag_console_puts(
                "usage: wifi connect <#number|ssid> [password] [/ap]\n");
            ag_console_puts("  #number is a line from the last `wifi scan`\n");
            ag_console_puts("  /ap  join that access point and no other\n");
            ag_console_puts("  no password: the one in SYSTEM.CFG, if the "
                            "network is the same\n");
            ag_console_puts("  the network is remembered in SYSTEM.CFG, in "
                            "clear text\n");
            return 1;
        }

        /*
         * /ap pins the association to one access point.
         *
         * The default is the network, and that is the right default: a name
         * with two boxes behind it is one network, and letting the radio take
         * whichever is better is the whole point of that arrangement.  But
         * "better" is the radio's opinion, and on this desk it chose the box
         * two rooms away at -77 dBm over the one next door at -42 - so there
         * has to be a way to say which, and it has to be a way that survives a
         * power cut.  It only means anything with a scan line: a bare name says
         * nothing about which box answers to it.
         */
        bool want_pin = false;
        int  positional = 0;
        const char *args[2] = {NULL, NULL};
        for (int i = 2; i < argc; i++) {
            if (ag_path_icmp(argv[i], "/ap") == 0) {
                want_pin = true;
            } else if (positional < 2) {
                args[positional++] = argv[i];
            }
        }
        if (args[0] == NULL) {
            ag_console_puts("wifi connect: which network?\n");
            return 1;
        }

        uint32_t    line = 0;
        const char *ssid = wifi_named_line(args[0], &line);
        if (ssid == NULL) {
            return 1;
        }

        const uint8_t *bssid = NULL;
        if (want_pin) {
            if (line == 0u || s_scan_bssid == NULL) {
                ag_console_puts("/ap needs a #number from `wifi scan`: a name "
                                "alone does not\n  say which access point\n");
                return 1;
            }
            bssid = s_scan_bssid[line - 1u];
        }

        /*
         * No password given means the one already written down, and only when
         * the network is the same one.  That is not a shortcut for typing: it
         * is how the access point can be changed without the key being said
         * out loud again, on a screen, in a room with people in it.
         */
        const char *pass = args[1];
        if (pass == NULL) {
            const char *known = ag_cfg_get(ag_sysconfig(), "wifi.ssid", NULL);
            if (known != NULL && ag_path_icmp(known, ssid) == 0) {
                pass = ag_cfg_get(ag_sysconfig(), "wifi.pass", "");
                ag_console_puts("using the key in SYSTEM.CFG\n");
            } else {
                pass = "";
            }
        }

        const ag_err_t err = ag_port_wifi_connect(ssid, pass, bssid);
        if (err != AG_OK) {
            ag_console_printf("wifi connect: %s\n",
                              ag_loader_api()->sys->strerror(err));
            return 1;
        }
        /*
         * Written down as well as attempted, because a board that has to be
         * told its network again after every power cut is a board that has to
         * have a keyboard attached to it.  Clear text, and said out loud
         * above: the flash of a device somebody can pick up is not a secret.
         */
        const ag_err_t cerr = cfg_ensure_wifi(ssid, pass, bssid);
        if (cerr != AG_OK) {
            ag_console_printf("SYSTEM.CFG: %d (joining anyway)\n", (int)cerr);
        }
        if (bssid != NULL) {
            char text[18];
            (void)ag_mac_str(bssid, text, sizeof(text));
            ag_console_printf("joining %s at %s...\n", ssid, text);
        } else {
            ag_console_printf("joining %s...\n", ssid);
        }
        return 0;
    }

    if (ag_path_icmp(argv[1], "ap") == 0) {
#if !AG_PORT_WIFI_HAS_AP
        ag_console_puts("this build has no access point "
                        "(ARGON_NET_WIFI_AP is off)\n");
        return 1;
#else
        /* Bare `wifi ap` is a question, answered by the same status the radio
         * gives to `wifi`. */
        if (argc < 3) {
            return wifi_status();
        }

        if (ag_path_icmp(argv[2], "off") == 0) {
            const ag_err_t err = ag_port_wifi_ap_stop();
            if (err != AG_OK) {
                ag_console_printf("wifi ap off: %s\n",
                                  ag_loader_api()->sys->strerror(err));
                return 1;
            }
            (void)cfg_ensure_ap(NULL, NULL, 0, false);
            ag_console_puts("access point off\n");
            return 0;
        }

        /*
         * wifi ap <ssid> [pass] [/ch N] [/hidden].  No key is an open point,
         * and the port refuses one to seven characters rather than making a
         * short key into an open network nobody meant.
         */
        const char *ssid = NULL;
        const char *pass = NULL;
        unsigned    channel = 0;
        bool        hidden = false;
        int         positional = 0;
        for (int i = 2; i < argc; i++) {
            if (ag_path_icmp(argv[i], "/hidden") == 0) {
                hidden = true;
            } else if (ag_path_icmp(argv[i], "/ch") == 0 && i + 1 < argc) {
                channel = (unsigned)atoi(argv[++i]);
            } else if (positional == 0) {
                ssid = argv[i];
                positional++;
            } else if (positional == 1) {
                pass = argv[i];
                positional++;
            }
        }
        if (ssid == NULL) {
            ag_console_puts(
                "usage: wifi ap <ssid> [password] [/ch 1..13] [/hidden]\n");
            ag_console_puts("  no password: an open network, said so here\n");
            ag_console_puts("  wifi ap off: stop offering one\n");
            return 1;
        }
        if (channel > 13u) {
            ag_console_puts("channel is 1 to 13\n");
            return 1;
        }

        /*
         * The point needs the radio, so bring it up if it is not - the same
         * bring-up and the same accounting as `wifi on`, because starting a
         * point is one of the two things that first spends the radio's memory.
         */
        ag_port_wifi_status_t st;
        if (ag_port_wifi_status(&st) != AG_OK || st.state == AG_WIFI_OFF) {
            const size_t   before = ag_port_mem_free(AG_MEM_FAST);
            const ag_err_t nerr = ag_net_init();
            if (nerr != AG_OK) {
                ag_console_printf("wifi ap: %s\n",
                                  ag_loader_api()->sys->strerror(nerr));
                return 1;
            }
            const size_t after = ag_port_mem_free(AG_MEM_FAST);
            ag_console_printf("radio on: took %u KB, %u KB free\n",
                              (unsigned)((before - after) / 1024u),
                              (unsigned)(after / 1024u));
        }

        ag_powerctl_bus_needed();
        const ag_err_t err =
            ag_port_wifi_ap_start(ssid, pass, (uint8_t)channel, hidden);
        if (err == -AG_EINVAL && pass != NULL && pass[0] != '\0') {
            ag_console_puts(
                "a key is 8 to 63 characters; a shorter one is refused, so an\n"
                "  open point is never started by mistake\n");
            return 1;
        }
        if (err != AG_OK) {
            ag_console_printf("wifi ap: %s\n",
                              ag_loader_api()->sys->strerror(err));
            return 1;
        }

        /* Written down so a board offers the same point after a power cut
         * without a console attached - the whole point of a fixed name. */
        (void)cfg_ensure_ap(ssid, pass, channel, hidden);

        ag_port_wifi_ap_status_t ap;
        if (ag_port_wifi_ap_status(&ap) == AG_OK && ap.on) {
            ag_console_printf("access point \"%s\"%s up on channel %u\n",
                              ap.ssid, ap.secured ? "" : " (open)",
                              (unsigned)ap.channel);
            ag_console_printf(
                "  join it and open  http://%u.%u.%u.%u/  - try  httpd 80 a:\\\n",
                (unsigned)(ap.ip >> 24), (unsigned)((ap.ip >> 16) & 0xffu),
                (unsigned)((ap.ip >> 8) & 0xffu), (unsigned)(ap.ip & 0xffu));
        } else {
            ag_console_puts("access point up\n");
        }
        return 0;
#endif /* AG_PORT_WIFI_HAS_AP */
    }

    if (ag_path_icmp(argv[1], "off") == 0) {
        const size_t before = ag_port_mem_free(AG_MEM_FAST);
        (void)ag_port_wifi_stop();
        const size_t after = ag_port_mem_free(AG_MEM_FAST);
        ag_console_printf("radio off, %u KB back\n",
                          (unsigned)((after - before) / 1024u));
        return 0;
    }

    if (ag_path_icmp(argv[1], "forget") == 0) {
        (void)ag_port_wifi_disconnect();
        const ag_err_t cerr = cfg_ensure_wifi(NULL, NULL, NULL);
        if (cerr != AG_OK) {
            ag_console_printf("SYSTEM.CFG: %d\n", (int)cerr);
        }
        /*
         * Said out loud, because it is not obvious and it costs something: the
         * key goes too, and the next `wifi connect` needs it typed again.  A
         * scan does not need this command - the radio can scan perfectly well
         * once it is joined, and only refuses while an attempt is in flight.
         */
        ag_console_puts("forgotten - the network, the key and the access "
                        "point\n");
        return 0;
    }

    ag_console_puts(
        "usage: wifi [on | off | scan | connect <ssid> [pass] | forget"
#if AG_PORT_WIFI_HAS_AP
        "\n             | ap <ssid> [pass] [/ch N] [/hidden] | ap off"
#endif
        "]\n");
    return 1;
}

#endif /* AG_PORT_HAS_WIFI */

#if AG_PORT_HAS_ESPNOW

/* "bcast"/"all"/ff:ff:ff:ff:ff:ff -> broadcast; else parse aa:bb:cc:dd:ee:ff. */
static bool espnow_parse_mac(const char *s, uint8_t out[6])
{
    if (ag_path_icmp(s, "bcast") == 0 || ag_path_icmp(s, "all") == 0) {
        memset(out, 0xff, 6);
        return true;
    }
    return ag_mac_parse(s, out);
}

static void espnow_print_payload(const uint8_t *data, uint32_t len)
{
    /* Text when it is text, so a message reads as one; a dot for the bytes that
     * are not, so binary is visible without a hex dump nobody asked for. */
    for (uint32_t i = 0; i < len; i++) {
        const uint8_t c = data[i];
        ag_console_printf("%c", (c >= 0x20 && c < 0x7f) ? (char)c : '.');
    }
}

static int espnow_status(void)
{
    if (!ag_espnow_running()) {
        ag_console_puts("espnow off\n");
        return 0;
    }
    ag_console_puts("espnow on\n");

    uint8_t mac[6];
    if (ag_espnow_self(mac) == AG_OK) {
        char text[18];
        (void)ag_mac_str(mac, text, sizeof(text));
        ag_console_printf("self %s\n", text);
    }
    const uint32_t dropped = ag_espnow_dropped();
    if (dropped > 0u) {
        ag_console_printf("%u datagram%s dropped (queue was full)\n",
                          (unsigned)dropped, (dropped == 1u) ? "" : "s");
    }
    return 0;
}

static int cmd_espnow(int argc, char **argv)
{
    /*
     * Anything that may put a radio on the air first makes sure the
     * peripheral bus is where a radio needs it.  A no-op unless the
     * clock has been taken below that - which only [power] crystal = 1
     * allows - and a hang is not the way to learn about a config key.
     */
    ag_powerctl_bus_needed();

    if (argc < 2) {
        return espnow_status();
    }

    if (ag_path_icmp(argv[1], "on") == 0) {
        /* The radio first, if it is not up - same bring-up and accounting as
         * `wifi on`, because ESP-NOW rides the same transceiver. */
        if (!ag_espnow_running()) {
            ag_port_wifi_status_t st;
            if (ag_port_wifi_status(&st) != AG_OK || st.state == AG_WIFI_OFF) {
                const size_t   before = ag_port_mem_free(AG_MEM_FAST);
                const ag_err_t nerr = ag_net_init();
                if (nerr != AG_OK) {
                    ag_console_printf("espnow on: %s\n",
                                      ag_loader_api()->sys->strerror(nerr));
                    return 1;
                }
                const size_t after = ag_port_mem_free(AG_MEM_FAST);
                ag_console_printf("radio on: took %u KB, %u KB free\n",
                                  (unsigned)((before - after) / 1024u),
                                  (unsigned)(after / 1024u));
            }
        }
        const ag_err_t err = ag_espnow_start();
        if (err != AG_OK) {
            ag_console_printf("espnow on: %s\n",
                              ag_loader_api()->sys->strerror(err));
            return 1;
        }
        return espnow_status();
    }

    if (ag_path_icmp(argv[1], "off") == 0) {
        ag_espnow_stop();
        ag_console_puts("espnow off\n");
        return 0;
    }

    if (!ag_espnow_running()) {
        ag_console_puts("espnow is off - `espnow on` first\n");
        return 1;
    }

    if (ag_path_icmp(argv[1], "peer") == 0) {
        if (argc >= 4 && ag_path_icmp(argv[2], "del") == 0) {
            uint8_t mac[6];
            if (!ag_mac_parse(argv[3], mac)) {
                ag_console_puts("that is not a hardware address\n");
                return 1;
            }
            const ag_err_t err = ag_espnow_peer_del(mac);
            ag_console_puts((err == AG_OK) ? "peer removed\n"
                                           : "no such peer\n");
            return (err == AG_OK) ? 0 : 1;
        }
        if (argc < 3) {
            ag_console_puts("usage: espnow peer <mac> [channel] [key]\n");
            ag_console_puts("       espnow peer del <mac>\n");
            ag_console_puts("  channel 0 = the one the radio is on now\n");
            ag_console_puts("  key: a shared secret, up to 16 chars, both "
                            "boards the same\n");
            return 1;
        }
        uint8_t mac[6];
        if (!ag_mac_parse(argv[2], mac)) {
            ag_console_puts("that is not a hardware address\n");
            return 1;
        }
        const unsigned channel = (argc > 3) ? (unsigned)atoi(argv[3]) : 0u;
        if (channel > 13u) {
            ag_console_puts("channel is 0 (current) or 1..13\n");
            return 1;
        }
        uint8_t        key[AG_ESPNOW_KEY];
        const uint8_t *keyp = NULL;
        if (argc > 4) {
            memset(key, 0, sizeof(key));
            const size_t kl = strlen(argv[4]);
            memcpy(key, argv[4], (kl < AG_ESPNOW_KEY) ? kl : AG_ESPNOW_KEY);
            keyp = key;
        }
        const ag_err_t err = ag_espnow_peer_add(mac, (uint8_t)channel, keyp);
        if (err != AG_OK) {
            ag_console_printf("espnow peer: %s\n",
                              ag_loader_api()->sys->strerror(err));
            return 1;
        }
        ag_console_puts((keyp != NULL) ? "peer added (encrypted)\n"
                                       : "peer added\n");
        return 0;
    }

    if (ag_path_icmp(argv[1], "send") == 0) {
        if (argc < 4) {
            ag_console_puts("usage: espnow send <mac|bcast> <text...>\n");
            return 1;
        }
        uint8_t mac[6];
        if (!espnow_parse_mac(argv[2], mac)) {
            ag_console_puts("that is not a hardware address (or `bcast`)\n");
            return 1;
        }
        /* The rest of the line, joined with spaces, is the message. */
        char   msg[AG_ESPNOW_MAX];
        size_t n = 0;
        for (int i = 3; i < argc && n < AG_ESPNOW_MAX; i++) {
            if (i > 3 && n < AG_ESPNOW_MAX) {
                msg[n++] = ' ';
            }
            const size_t room = (size_t)AG_ESPNOW_MAX - n;
            const size_t al = strlen(argv[i]);
            const size_t take = (al < room) ? al : room;
            memcpy(msg + n, argv[i], take);
            n += take;
        }
        const ag_err_t err = ag_espnow_send(mac, msg, (uint32_t)n);
        if (err != AG_OK) {
            ag_console_printf("espnow send: %s\n",
                              ag_loader_api()->sys->strerror(err));
            if (err == -AG_ENOENT) {
                ag_console_puts("  add it first: espnow peer <mac> [channel]\n");
            }
            return 1;
        }
        ag_console_printf("sent %u bytes\n", (unsigned)n);
        return 0;
    }

    if (ag_path_icmp(argv[1], "listen") == 0) {
        const unsigned secs = (argc > 2) ? (unsigned)atoi(argv[2]) : 0u;
        const int64_t  deadline =
            (secs > 0u) ? (ag_port_us() + (int64_t)secs * 1000000) : 0;
        ag_console_puts("listening (Ctrl+C to stop)...\n");

        uint8_t  mac[6];
        uint8_t  buf[AG_ESPNOW_MAX];
        uint32_t len = 0;
        for (;;) {
            if (ag_shell_interrupted()) {
                ag_console_puts("^C\n");
                break;
            }
            while (ag_espnow_recv(mac, buf, sizeof(buf), &len)) {
                char text[18];
                (void)ag_mac_str(mac, text, sizeof(text));
                ag_console_printf("%s (%u): ", text, (unsigned)len);
                espnow_print_payload(buf, len);
                ag_console_puts("\n");
            }
            if (secs > 0u && ag_port_us() > deadline) {
                break;
            }
            ag_port_task_delay(ag_port_ms_to_ticks(50));
        }
        return 0;
    }

    ag_console_puts("usage: espnow [on | off | peer <mac> [ch] [key] | "
                    "peer del <mac>\n"
                    "              | send <mac|bcast> <text> | listen [secs]]\n");
    return 1;
}

#endif /* AG_PORT_HAS_ESPNOW */

#if AG_PORT_HAS_WIFIMON

/* ---------------------------------------------------------------------- */
/* mon - monitor mode and raw injection                                   */
/* ---------------------------------------------------------------------- */

#define MON_APS  32
#define MON_STAS 32

typedef struct {
    uint8_t  bssid[6];
    char     ssid[33];
    uint8_t  channel;
    int8_t   rssi;
    uint32_t count;
} mon_ap_t;

typedef struct {
    uint8_t  mac[6];
    int8_t   rssi;
    uint32_t count;
} mon_sta_t;

/* Radio up and promiscuous on, shared by every mon subcommand that captures or
 * injects.  Injection wants promiscuous too, so it goes through here as well. */
static int mon_ensure_on(void)
{
    if (!ag_wifimon_running()) {
        ag_port_wifi_status_t st;
        if (ag_port_wifi_status(&st) != AG_OK || st.state == AG_WIFI_OFF) {
            const size_t   before = ag_port_mem_free(AG_MEM_FAST);
            const ag_err_t nerr = ag_net_init();
            if (nerr != AG_OK) {
                ag_console_printf("mon: %s\n",
                                  ag_loader_api()->sys->strerror(nerr));
                return -1;
            }
            const size_t after = ag_port_mem_free(AG_MEM_FAST);
            ag_console_printf("radio on: took %u KB, %u KB free\n",
                              (unsigned)((before - after) / 1024u),
                              (unsigned)(after / 1024u));
        }
        const ag_err_t err = ag_wifimon_start();
        if (err != AG_OK) {
            ag_console_printf("mon: %s\n",
                              ag_loader_api()->sys->strerror(err));
            return -1;
        }
    }
    return 0;
}

static int mon_status(void)
{
    if (!ag_wifimon_running()) {
        ag_console_puts("monitor off\n");
        return 0;
    }
    uint32_t c[AG_WIFIMON_C_N];
    ag_wifimon_counters(c);
    ag_console_printf("monitor on, channel %u\n",
                      (unsigned)ag_wifimon_channel_get());
    ag_console_printf("frames %u  (mgmt %u, ctrl %u, data %u, misc %u)\n",
                      (unsigned)c[AG_WIFIMON_C_TOTAL],
                      (unsigned)c[AG_WIFIMON_C_MGMT],
                      (unsigned)c[AG_WIFIMON_C_CTRL],
                      (unsigned)c[AG_WIFIMON_C_DATA],
                      (unsigned)c[AG_WIFIMON_C_MISC]);
    const uint32_t d = ag_wifimon_dropped();
    if (d > 0u) {
        ag_console_printf("%u dropped (ring was full)\n", (unsigned)d);
    }
    return 0;
}

/* Find or add; returns 1 when the entry is new (so the caller prints it). */
static int mon_ap_seen(mon_ap_t *aps, int *n, const uint8_t bssid[6],
                       const char *ssid, uint8_t ch, int8_t rssi)
{
    for (int i = 0; i < *n; i++) {
        if (memcmp(aps[i].bssid, bssid, 6) == 0) {
            aps[i].rssi = rssi;
            aps[i].channel = ch;
            aps[i].count++;
            if (aps[i].ssid[0] == '\0' && ssid[0] != '\0') {
                snprintf(aps[i].ssid, sizeof(aps[i].ssid), "%s", ssid);
            }
            return 0;
        }
    }
    if (*n >= MON_APS) {
        return 0;
    }
    memcpy(aps[*n].bssid, bssid, 6);
    snprintf(aps[*n].ssid, sizeof(aps[*n].ssid), "%s", ssid);
    aps[*n].channel = ch;
    aps[*n].rssi = rssi;
    aps[*n].count = 1;
    (*n)++;
    return 1;
}

static int mon_sta_seen(mon_sta_t *stas, int *n, const uint8_t mac[6],
                        int8_t rssi)
{
    for (int i = 0; i < *n; i++) {
        if (memcmp(stas[i].mac, mac, 6) == 0) {
            stas[i].rssi = rssi;
            stas[i].count++;
            return 0;
        }
    }
    if (*n >= MON_STAS) {
        return 0;
    }
    memcpy(stas[*n].mac, mac, 6);
    stas[*n].rssi = rssi;
    stas[*n].count = 1;
    (*n)++;
    return 1;
}

/* Pull what an 802.11 frame tells us about who is on the air, and print the
 * ones we had not seen before. */
static void mon_parse(mon_ap_t *aps, int *nap, mon_sta_t *stas, int *nsta,
                      const uint8_t *f, uint32_t len, int8_t rssi, uint8_t ch)
{
    if (len < 1u) {
        return;
    }
    const unsigned ftype = (f[0] >> 2) & 0x3u;
    const unsigned sub = (f[0] >> 4) & 0xfu;

    if (ftype == 0u && len >= 24u) { /* management */
        const uint8_t *addr2 = f + 10;
        const uint8_t *bssid = f + 16;
        if (sub == 8u || sub == 5u) { /* beacon or probe response */
            char ssid[33] = "";
            if (len >= 38u && f[36] == 0u) {
                unsigned sl = f[37];
                if (sl > 32u) {
                    sl = 32u;
                }
                if (38u + sl <= len) {
                    memcpy(ssid, f + 38, sl);
                    ssid[sl] = '\0';
                }
            }
            if (mon_ap_seen(aps, nap, bssid, ssid, ch, rssi)) {
                char text[18];
                (void)ag_mac_str(bssid, text, sizeof(text));
                ag_console_printf("AP  %s ch%2u %4d dBm  \"%s\"\n", text,
                                  (unsigned)ch, (int)rssi,
                                  ssid[0] ? ssid : "(hidden)");
            }
        } else { /* probe request, auth, assoc: addr2 is a station */
            if (mon_sta_seen(stas, nsta, addr2, rssi)) {
                char text[18];
                (void)ag_mac_str(addr2, text, sizeof(text));
                ag_console_printf("sta %s      %4d dBm\n", text, (int)rssi);
            }
        }
    } else if (ftype == 2u && len >= 16u) { /* data */
        /* Only when this board's transmitter is a station (ToDS, not FromDS)
         * is addr2 a station rather than the access point. */
        const bool to_ds = (f[1] & 0x01u) != 0u;
        const bool from_ds = (f[1] & 0x02u) != 0u;
        if (to_ds && !from_ds) {
            const uint8_t *addr2 = f + 10;
            if (mon_sta_seen(stas, nsta, addr2, rssi)) {
                char text[18];
                (void)ag_mac_str(addr2, text, sizeof(text));
                ag_console_printf("sta %s      %4d dBm\n", text, (int)rssi);
            }
        }
    }
}

/* mon watch / mon hop: the live view.  hop_ms == 0 means stay on one channel. */
static int mon_watch(unsigned secs, unsigned hop_ms)
{
    if (mon_ensure_on() != 0) {
        return 1;
    }

    mon_ap_t  *aps = ag_port_alloc(sizeof(mon_ap_t) * MON_APS,
                                   AG_MEM_FAST | AG_MEM_BYTE);
    mon_sta_t *stas = ag_port_alloc(sizeof(mon_sta_t) * MON_STAS,
                                    AG_MEM_FAST | AG_MEM_BYTE);
    if (aps == NULL || stas == NULL) {
        ag_port_free(aps);
        ag_port_free(stas);
        ag_console_puts("mon: no memory for the tables\n");
        return 1;
    }
    int nap = 0;
    int nsta = 0;

    if (hop_ms > 0u) {
        ag_console_puts("hopping channels 1..13 (Ctrl+C to stop)...\n");
    } else {
        ag_console_printf("watching channel %u (Ctrl+C to stop)...\n",
                          (unsigned)ag_wifimon_channel_get());
    }

    const int64_t deadline =
        (secs > 0u) ? (ag_port_us() + (int64_t)secs * 1000000) : 0;
    uint8_t  channel = (hop_ms > 0u) ? 1u : ag_wifimon_channel_get();
    int64_t  next_hop = ag_port_us() + (int64_t)hop_ms * 1000;
    if (hop_ms > 0u) {
        (void)ag_wifimon_channel(channel);
    }

    uint8_t  buf[AG_WIFIMON_SNAP];
    int8_t   rssi = 0;
    uint8_t  ch = 0;
    uint32_t full = 0;
    uint32_t copied = 0;
    for (;;) {
        if (ag_shell_interrupted()) {
            ag_console_puts("^C\n");
            break;
        }
        while (ag_wifimon_drain(&rssi, &ch, &full, buf, sizeof(buf), &copied)) {
            mon_parse(aps, &nap, stas, &nsta, buf, copied, rssi, ch);
        }
        if (hop_ms > 0u && ag_port_us() >= next_hop) {
            channel = (channel >= 13u) ? 1u : (uint8_t)(channel + 1u);
            (void)ag_wifimon_channel(channel);
            next_hop = ag_port_us() + (int64_t)hop_ms * 1000;
        }
        if (secs > 0u && ag_port_us() > deadline) {
            break;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(30));
    }

    ag_console_printf("%d access point%s, %d station%s seen\n", nap,
                      (nap == 1) ? "" : "s", nsta, (nsta == 1) ? "" : "s");
    ag_port_free(aps);
    ag_port_free(stas);
    return 0;
}

static int mon_hex(const char *s, uint8_t *out, uint32_t cap, uint32_t *outlen)
{
    uint32_t n = 0;
    while (*s != '\0') {
        if (*s == ' ' || *s == ':' || *s == '-') {
            s++;
            continue;
        }
        const int hi = hex_digit(*s);
        const int lo = (s[1] != '\0') ? hex_digit(s[1]) : -1;
        if (hi < 0 || lo < 0) {
            return -1;
        }
        if (n >= cap) {
            return -1;
        }
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    *outlen = n;
    return 0;
}

/* Injection frame builders.  These are the frames a person means when they say
 * "deauth" or "beacon"; the port neither knows nor cares what they are, so the
 * knowledge lives here, in the place a command was typed. */
static uint32_t mon_build_deauth(uint8_t *b, const uint8_t dest[6],
                                 const uint8_t bssid[6])
{
    uint32_t n = 0;
    b[n++] = 0xc0; /* frame control: type mgmt, subtype deauthentication */
    b[n++] = 0x00;
    b[n++] = 0x00; /* duration */
    b[n++] = 0x00;
    memcpy(b + n, dest, 6);  n += 6; /* addr1: the one being kicked off   */
    memcpy(b + n, bssid, 6); n += 6; /* addr2: from the access point       */
    memcpy(b + n, bssid, 6); n += 6; /* addr3: bssid                       */
    b[n++] = 0x00; /* sequence control */
    b[n++] = 0x00;
    b[n++] = 0x07; /* reason 7: class-3 frame from a nonassociated station */
    b[n++] = 0x00;
    return n;
}

static uint32_t mon_build_beacon(uint8_t *b, const char *ssid, uint8_t channel)
{
    uint32_t     n = 0;
    const size_t sl = strlen(ssid);
    const uint8_t slen = (sl > 32u) ? 32u : (uint8_t)sl;

    b[n++] = 0x80; /* frame control: type mgmt, subtype beacon */
    b[n++] = 0x00;
    b[n++] = 0x00; /* duration */
    b[n++] = 0x00;
    memset(b + n, 0xff, 6); n += 6; /* addr1: broadcast                    */
    /* addr2/addr3: a locally-administered address derived from the name, so
     * two different names do not collide and none impersonates real hardware. */
    uint8_t bssid[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
    for (size_t i = 0; i < sl; i++) {
        bssid[3 + (i % 3u)] ^= (uint8_t)ssid[i];
    }
    memcpy(b + n, bssid, 6); n += 6;
    memcpy(b + n, bssid, 6); n += 6;
    b[n++] = 0x00; /* sequence control */
    b[n++] = 0x00;

    /* fixed parameters: timestamp (8), beacon interval (2), capabilities (2) */
    memset(b + n, 0, 8); n += 8;
    b[n++] = 0x64; b[n++] = 0x00;      /* 100 TU */
    b[n++] = 0x01; b[n++] = 0x04;      /* ESS, short-slot */

    b[n++] = 0x00; b[n++] = slen;      /* tag 0: SSID */
    memcpy(b + n, ssid, slen); n += slen;

    b[n++] = 0x01; b[n++] = 0x04;      /* tag 1: supported rates */
    b[n++] = 0x82; b[n++] = 0x84; b[n++] = 0x8b; b[n++] = 0x96;

    b[n++] = 0x03; b[n++] = 0x01; b[n++] = channel; /* tag 3: DS (channel) */
    return n;
}

static int cmd_mon(int argc, char **argv)
{
    ag_powerctl_bus_needed();

    if (argc < 2) {
        return mon_status();
    }

    if (ag_path_icmp(argv[1], "off") == 0) {
        ag_wifimon_stop();
        ag_console_puts("monitor off\n");
        return 0;
    }

    if (ag_path_icmp(argv[1], "on") == 0) {
        if (mon_ensure_on() != 0) {
            return 1;
        }
        if (argc > 2) {
            const unsigned ch = (unsigned)atoi(argv[2]);
            if (ch < 1u || ch > 14u) {
                ag_console_puts("channel is 1..14\n");
                return 1;
            }
            (void)ag_wifimon_channel((uint8_t)ch);
        }
        return mon_status();
    }

    if (ag_path_icmp(argv[1], "channel") == 0) {
        if (argc < 3 || mon_ensure_on() != 0) {
            if (argc < 3) {
                ag_console_puts("usage: mon channel <1..14>\n");
            }
            return 1;
        }
        const unsigned ch = (unsigned)atoi(argv[2]);
        if (ch < 1u || ch > 14u) {
            ag_console_puts("channel is 1..14\n");
            return 1;
        }
        const ag_err_t err = ag_wifimon_channel((uint8_t)ch);
        ag_console_printf(err == AG_OK ? "channel %u\n" : "channel: failed\n",
                          ch);
        return (err == AG_OK) ? 0 : 1;
    }

    if (ag_path_icmp(argv[1], "filter") == 0) {
        if (argc < 3) {
            ag_console_puts("usage: mon filter <all|mgmt|ctrl|data>...\n");
            return 1;
        }
        uint32_t mask = 0;
        for (int i = 2; i < argc; i++) {
            if (ag_path_icmp(argv[i], "all") == 0) {
                mask |= AG_WIFIMON_ALL;
            } else if (ag_path_icmp(argv[i], "mgmt") == 0) {
                mask |= AG_WIFIMON_MGMT;
            } else if (ag_path_icmp(argv[i], "ctrl") == 0) {
                mask |= AG_WIFIMON_CTRL;
            } else if (ag_path_icmp(argv[i], "data") == 0) {
                mask |= AG_WIFIMON_DATA;
            }
        }
        if (mon_ensure_on() != 0) {
            return 1;
        }
        return (ag_wifimon_filter(mask) == AG_OK) ? 0 : 1;
    }

    if (ag_path_icmp(argv[1], "watch") == 0) {
        const unsigned secs = (argc > 2) ? (unsigned)atoi(argv[2]) : 0u;
        return mon_watch(secs, 0u);
    }

    if (ag_path_icmp(argv[1], "hop") == 0) {
        const unsigned ms = (argc > 2) ? (unsigned)atoi(argv[2]) : 250u;
        return mon_watch(0u, (ms < 50u) ? 50u : ms);
    }

    if (ag_path_icmp(argv[1], "tx") == 0) {
        if (argc < 3) {
            ag_console_puts("usage: mon tx <hex>   (a whole 802.11 frame, no "
                            "FCS)\n");
            return 1;
        }
        uint8_t *buf = ag_port_alloc(AG_WIFIMON_TX_MAX, AG_MEM_FAST | AG_MEM_BYTE);
        if (buf == NULL) {
            ag_console_puts("mon tx: no memory\n");
            return 1;
        }
        uint32_t len = 0;
        if (mon_hex(argv[2], buf, AG_WIFIMON_TX_MAX, &len) != 0 || len == 0u) {
            ag_console_puts("that is not hex, or it is too long\n");
            ag_port_free(buf);
            return 1;
        }
        int rc = 1;
        if (mon_ensure_on() == 0) {
            const ag_err_t err = ag_wifimon_tx(buf, len);
            if (err == AG_OK) {
                ag_console_printf("injected %u bytes\n", (unsigned)len);
                rc = 0;
            } else {
                ag_console_printf("mon tx: %s\n",
                                  ag_loader_api()->sys->strerror(err));
            }
        }
        ag_port_free(buf);
        return rc;
    }

    if (ag_path_icmp(argv[1], "deauth") == 0) {
        if (argc < 3) {
            ag_console_puts("usage: mon deauth <ap-bssid> [station|bcast] "
                            "[count]\n");
            return 1;
        }
        uint8_t bssid[6];
        if (!ag_mac_parse(argv[2], bssid)) {
            ag_console_puts("that is not a hardware address\n");
            return 1;
        }
        uint8_t dest[6];
        memset(dest, 0xff, 6); /* broadcast: every client of that AP */
        if (argc > 3 && ag_path_icmp(argv[3], "bcast") != 0) {
            if (!ag_mac_parse(argv[3], dest)) {
                ag_console_puts("that is not a station address (or `bcast`)\n");
                return 1;
            }
        }
        const int argn = (argc > 4) ? atoi(argv[4]) : 5;
        const unsigned count = (argn < 1) ? 1u : (unsigned)argn;
        if (mon_ensure_on() != 0) {
            return 1;
        }
        uint8_t  frame[26];
        const uint32_t flen = mon_build_deauth(frame, dest, bssid);
        unsigned sent = 0;
        for (unsigned i = 0; i < count; i++) {
            if (ag_wifimon_tx(frame, flen) == AG_OK) {
                sent++;
            }
        }
        ag_console_printf("sent %u deauth frame%s\n", sent,
                          (sent == 1u) ? "" : "s");
        return (sent > 0u) ? 0 : 1;
    }

    if (ag_path_icmp(argv[1], "beacon") == 0) {
        if (argc < 3) {
            ag_console_puts("usage: mon beacon <ssid> [count]\n");
            return 1;
        }
        const int argn = (argc > 3) ? atoi(argv[3]) : 1;
        const unsigned count = (argn < 1) ? 1u : (unsigned)argn;
        if (mon_ensure_on() != 0) {
            return 1;
        }
        uint8_t        frame[128];
        const uint32_t flen = mon_build_beacon(frame, argv[2],
                                               ag_wifimon_channel_get());
        unsigned sent = 0;
        for (unsigned i = 0; i < count; i++) {
            if (ag_wifimon_tx(frame, flen) == AG_OK) {
                sent++;
            }
        }
        ag_console_printf("sent %u beacon%s for \"%s\"\n", sent,
                          (sent == 1u) ? "" : "s", argv[2]);
        return (sent > 0u) ? 0 : 1;
    }

    ag_console_puts(
        "usage: mon [on [ch] | off | channel <n> | filter <types>\n"
        "           | watch [secs] | hop [ms]\n"
        "           | tx <hex> | deauth <bssid> [sta|bcast] [n] | beacon "
        "<ssid> [n]]\n");
    return 1;
}

#endif /* AG_PORT_HAS_WIFIMON */

#if AG_PORT_HAS_BT

static void bt_print_addr(const uint8_t a[6])
{
    ag_console_printf("%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3],
                      a[4], a[5]);
}

static bool bt_parse_addr(const char *s, uint8_t out[6])
{
    int n = 0;
    for (; *s != '\0' && n < 6; s++) {
        int hi, lo;
        if (*s == ':' || *s == '-') {
            continue;
        }
        hi = hex_digit(*s);
        lo = (s[1] != '\0') ? hex_digit(s[1]) : -1;
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[n++] = (uint8_t)((hi << 4) | lo);
        s++;
    }
    return n == 6;
}

/* The scan list is kept so that `bt open 3` can mean the third line of it. */
static ag_port_bt_dev_t s_bt_seen[8];
static uint32_t         s_bt_seen_n;

static int cmd_bt(int argc, char **argv)
{
    ag_port_bt_status_t st;

    if (argc < 2) {
        if (ag_port_bt_status(&st) != AG_OK) {
            ag_console_puts("bt: no radio in this build\n");
            return 1;
        }
        switch (st.state) {
        case AG_BT_OFF:      ag_console_puts("radio off\n"); break;
        case AG_BT_SCANNING: ag_console_puts("scanning\n"); break;
        case AG_BT_OPENING:  ag_console_puts("connecting\n"); break;
        case AG_BT_OPEN:
            ag_console_printf("%s connected, %u reports\n",
                              st.name[0] != '\0' ? st.name : "device",
                              (unsigned)st.reports);
            ag_console_puts("  ");
            bt_print_addr(st.addr);
            ag_console_puts("\n");
            break;
        default: ag_console_puts("radio on, nothing paired\n"); break;
        }
        return 0;
    }

    if (ag_path_icmp(argv[1], "on") == 0) {
        ag_powerctl_bus_needed();
        const ag_err_t err = ag_port_bt_start();
        if (err != AG_OK) {
            ag_console_printf("bt on: %s\n",
                              ag_loader_api()->sys->strerror(err));
            return 1;
        }
        (void)ag_btinput_init();
        ag_console_puts("radio on\n");
        return 0;
    }

    if (ag_path_icmp(argv[1], "off") == 0) {
        const size_t before = ag_port_mem_free(AG_MEM_FAST);
        (void)ag_port_bt_stop();
        const size_t after = ag_port_mem_free(AG_MEM_FAST);
        ag_console_printf("radio off, %u KB back\n",
                          (unsigned)((after - before) / 1024u));
        return 0;
    }

    if (ag_path_icmp(argv[1], "scan") == 0) {
        uint32_t found = 0;
        if (ag_port_bt_status(&st) == AG_OK && st.state == AG_BT_OFF) {
            /* Starting it here rather than refusing: `bt scan` on a board with
             * the radio off is a request for the radio, not a mistake. */
            ag_powerctl_bus_needed();
            const ag_err_t serr = ag_port_bt_start();
            if (serr != AG_OK) {
                ag_console_printf("bt: %s\n",
                                  ag_loader_api()->sys->strerror(serr));
                return 1;
            }
            (void)ag_btinput_init();
        }
        ag_console_puts("scanning...\n");
        const ag_err_t err = ag_port_bt_scan(s_bt_seen, 8, &found, 5);
        if (err != AG_OK) {
            ag_console_printf("bt scan: %s\n",
                              ag_loader_api()->sys->strerror(err));
            return 1;
        }
        s_bt_seen_n = (found < 8u) ? found : 8u;
        if (s_bt_seen_n == 0) {
            ag_console_puts("nothing advertising\n");
            return 0;
        }

        /*
         * Keyboards first, then by signal.  What a person is looking for in
         * this list is the thing they are holding, and it is the strongest
         * one that says it is a keyboard.
         */
        for (uint32_t i = 1; i < s_bt_seen_n; i++) {
            const ag_port_bt_dev_t key = s_bt_seen[i];
            uint32_t               j = i;
            while (j > 0) {
                const ag_port_bt_dev_t *prev = &s_bt_seen[j - 1];
                const bool better = (key.hid && !prev->hid) ||
                                    (key.hid == prev->hid &&
                                     key.rssi > prev->rssi);
                if (!better) {
                    break;
                }
                s_bt_seen[j] = *prev;
                j--;
            }
            s_bt_seen[j] = key;
        }
        ag_console_puts("  # name              dBm what\n");
        for (uint32_t i = 0; i < s_bt_seen_n; i++) {
            /*
             * Most of what a scan hears is nameless on purpose - beacons, and
             * phones that only say who they are while their Bluetooth screen
             * is open.  The address is then the only handle there is, and a
             * line saying "(no name)" eight times is not a list.
             */
            char label[AG_BT_NAME_MAX + 1];
            if (s_bt_seen[i].name[0] != '\0') {
                /* Copied rather than printed: the compiler cannot see that
                 * the source is the same size and terminated, and a warning
                 * that cannot be proved wrong is a warning worth avoiding. */
                memcpy(label, s_bt_seen[i].name, sizeof(label) - 1u);
                label[sizeof(label) - 1u] = '\0';
            } else {
                const uint8_t *a = s_bt_seen[i].addr;
                snprintf(label, sizeof(label), "%02x:%02x:%02x:%02x:%02x:%02x",
                         a[0], a[1], a[2], a[3], a[4], a[5]);
            }
            /*
             * Address types 1 and 3 are random - which is to say private.
             * Phones, laptops and watches rotate theirs every few minutes so
             * that they cannot be followed around, and each new one looks like
             * a new device to anybody listening, including this.  That is why
             * a scan finds more things than a person can see in the room, and
             * why the same thing can be on the list twice.
             */
            const bool rnd = (s_bt_seen[i].addr_type & 1) != 0;
            ag_console_printf("  %u %-17s %4d %s\n", (unsigned)(i + 1), label,
                              (int)s_bt_seen[i].rssi,
                              s_bt_seen[i].hid ? "keyboard" : (rnd ? "private"
                                                                   : ""));
        }
        if (found > s_bt_seen_n) {
            ag_console_printf("%u more heard, not shown\n",
                              (unsigned)(found - s_bt_seen_n));
        }
        ag_console_puts("bt open <#>  to pair with one\n");
        return 0;
    }

    if (ag_path_icmp(argv[1], "open") == 0) {
        if (argc < 3) {
            ag_console_puts("usage: bt open <# from scan | address>\n");
            return 1;
        }
        uint8_t addr[6];
        int     type = 0;

        const int idx = atoi(argv[2]);
        if (idx >= 1 && (uint32_t)idx <= s_bt_seen_n) {
            memcpy(addr, s_bt_seen[idx - 1].addr, sizeof(addr));
            type = s_bt_seen[idx - 1].addr_type;
        } else if (!bt_parse_addr(argv[2], addr)) {
            ag_console_puts("bt open: not a number from the last scan, and "
                            "not an address\n");
            return 1;
        }

        const ag_err_t err = ag_port_bt_open(addr, type);
        if (err != AG_OK) {
            ag_console_printf("bt open: %s\n",
                              ag_loader_api()->sys->strerror(err));
            return 1;
        }
        ag_console_puts("connecting; `bt` says when it is up\n");
        /*
         * Remembered here rather than after the connection succeeds, because
         * what is being remembered is the intent: this is the keyboard this
         * board is meant to have, and the boot after next should look for it
         * whether or not it answered today.
         */
        char text[32];
        snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", addr[0],
                 addr[1], addr[2], addr[3], addr[4], addr[5]);
        (void)cfg_ensure_bt(text, type);
        return 0;
    }

    if (ag_path_icmp(argv[1], "close") == 0) {
        (void)ag_port_bt_close();
        ag_console_puts("closed\n");
        return 0;
    }

    if (ag_path_icmp(argv[1], "open") != 0 &&
        ag_path_icmp(argv[1], "forget") != 0) {
        ag_console_puts(
            "usage: bt [on | off | scan | open <#|addr> | close | forget]\n");
        return 1;
    }

    if (ag_path_icmp(argv[1], "forget") == 0) {
        (void)ag_port_bt_close();
        (void)cfg_ensure_bt(NULL, 0);
        ag_console_puts("forgotten\n");
        return 0;
    }

    ag_console_puts("usage: bt [scan | open <#|addr> | close | forget]\n");
    return 1;
}

#if AG_PORT_HAS_BLE_CENTRAL || AG_PORT_HAS_BLE_PERIPH

/*
 * `ble`: the board as a BLE central (scan / connect / read / write) and as a
 * peripheral (advertise a small GATT server).  `bt` is the keyboard; this is
 * everything else - what is out there, and being something others reach.
 */

/* Bring the radio up if it is off - `ble` with it off is a request for it. */
static ag_err_t ble_ensure_radio(void)
{
    ag_port_bt_status_t st;
    if (ag_port_bt_status(&st) == AG_OK && st.state == AG_BT_OFF) {
        return ag_port_bt_start();
    }
    return AG_OK;
}

#if AG_PORT_HAS_BLE_CENTRAL
/* The last scan is kept so `ble connect #3` can mean its third line, the same
 * convenience `wifi connect #3` has. */
#define BLE_LIST_MAX 16
static struct {
    uint8_t addr[6];
    int     addr_type;
    char    name[AG_BLE_NAME_MAX + 1];
} s_ble_list[BLE_LIST_MAX];
static uint32_t s_ble_list_n;

static void ble_props_str(uint8_t p, char *out, size_t n)
{
    /* R read, W write, w write-without-response, N notify, I indicate. */
    size_t k = 0;
    if ((p & AG_BLE_PROP_READ) && k + 1u < n) out[k++] = 'R';
    if ((p & AG_BLE_PROP_WRITE) && k + 1u < n) out[k++] = 'W';
    if ((p & AG_BLE_PROP_WNORSP) && k + 1u < n) out[k++] = 'w';
    if ((p & AG_BLE_PROP_NOTIFY) && k + 1u < n) out[k++] = 'N';
    if ((p & AG_BLE_PROP_INDIC) && k + 1u < n) out[k++] = 'I';
    out[k] = '\0';
}

static int ble_scan(void)
{
    const ag_err_t rerr = ble_ensure_radio();
    if (rerr != AG_OK) {
        ag_console_printf("ble: %s\n", ag_loader_api()->sys->strerror(rerr));
        return 1;
    }

    ag_port_ble_dev_t *devs = ag_port_alloc(
        sizeof(ag_port_ble_dev_t) * BLE_LIST_MAX, AG_MEM_FAST | AG_MEM_BYTE);
    if (devs == NULL) {
        ag_console_puts("ble: no memory\n");
        return 1;
    }

    ag_console_puts("scanning...\n");
    uint32_t       found = 0;
    const ag_err_t err = ag_port_ble_scan(devs, BLE_LIST_MAX, &found, 5);
    if (err != AG_OK) {
        ag_console_printf("ble scan: %s\n",
                          ag_loader_api()->sys->strerror(err));
        ag_port_free(devs);
        return 1;
    }

    const uint32_t shown = (found < BLE_LIST_MAX) ? found : BLE_LIST_MAX;
    if (shown == 0u) {
        ag_console_puts("nothing advertising\n");
        s_ble_list_n = 0;
        ag_port_free(devs);
        return 0;
    }

    ag_console_puts("  #  address            dBm  conn  name / info\n");
    s_ble_list_n = 0;
    for (uint32_t i = 0; i < shown; i++) {
        const ag_port_ble_dev_t *d = &devs[i];
        char                     text[18];
        (void)ag_mac_str(d->addr, text, sizeof(text));
        ag_console_printf("%3u  %s %4d   %s   %s", (unsigned)(i + 1u), text,
                          (int)d->rssi, d->connectable ? "y" : "n",
                          (d->name[0] != '\0') ? d->name : "(no name)");
        if (d->appearance != 0u) {
            ag_console_printf(" [look %04x]", (unsigned)d->appearance);
        }
        if (d->company != 0xffffu) {
            ag_console_printf(" [mfr %04x]", (unsigned)d->company);
        }
        for (uint8_t k = 0; k < d->n_uuids; k++) {
            ag_console_printf(" %04x", (unsigned)d->uuids[k]);
        }
        ag_console_puts("\n");

        memcpy(s_ble_list[i].addr, d->addr, 6);
        s_ble_list[i].addr_type = d->addr_type;
        snprintf(s_ble_list[i].name, sizeof(s_ble_list[i].name), "%s", d->name);
        s_ble_list_n = i + 1u;
    }
    if (found > shown) {
        ag_console_printf("%u more not shown\n", (unsigned)(found - shown));
    }
    ag_console_puts("connect one with: ble connect #<number>\n");
    ag_port_free(devs);
    return 0;
}

/* Discover services and characteristics of the open device and print them as a
 * tree - each characteristic under the service whose handle range holds it. */
static int ble_discover_print(void)
{
    const ag_err_t derr = ag_port_ble_discover(8000);
    if (derr != AG_OK) {
        ag_console_printf("discover: %s\n",
                          ag_loader_api()->sys->strerror(derr));
        return 1;
    }

    ag_ble_svc_t *svcs = ag_port_alloc(sizeof(ag_ble_svc_t) * 12u,
                                       AG_MEM_FAST | AG_MEM_BYTE);
    ag_ble_chr_t *chrs = ag_port_alloc(sizeof(ag_ble_chr_t) * 24u,
                                       AG_MEM_FAST | AG_MEM_BYTE);
    if (svcs == NULL || chrs == NULL) {
        ag_port_free(svcs);
        ag_port_free(chrs);
        ag_console_puts("ble: no memory\n");
        return 1;
    }

    const uint32_t nsvc = ag_port_ble_services(svcs, 12u);
    const uint32_t nchr = ag_port_ble_chars(chrs, 24u);
    const uint32_t nsvc_shown = (nsvc < 12u) ? nsvc : 12u;
    const uint32_t nchr_shown = (nchr < 24u) ? nchr : 24u;

    for (uint32_t i = 0; i < nsvc_shown; i++) {
        ag_console_printf("service %s  [%04x-%04x]\n", svcs[i].uuid,
                          (unsigned)svcs[i].start, (unsigned)svcs[i].end);
        for (uint32_t j = 0; j < nchr_shown; j++) {
            if (chrs[j].handle < svcs[i].start ||
                chrs[j].handle > svcs[i].end) {
                continue;
            }
            char props[8];
            ble_props_str(chrs[j].props, props, sizeof(props));
            ag_console_printf("  char %s  handle %04x  %s\n", chrs[j].uuid,
                              (unsigned)chrs[j].handle, props);
        }
    }
    ag_console_puts("read/write with: ble read <handle> | ble write <handle> "
                    "<text>\n");
    ag_port_free(svcs);
    ag_port_free(chrs);
    return 0;
}

static void ble_print_value(const uint8_t *v, uint32_t n)
{
    ag_console_printf("%u bytes:", (unsigned)n);
    for (uint32_t i = 0; i < n; i++) {
        ag_console_printf(" %02x", (unsigned)v[i]);
    }
    ag_console_puts("  \"");
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t c = v[i];
        ag_console_printf("%c", (c >= 0x20 && c < 0x7f) ? (char)c : '.');
    }
    ag_console_puts("\"\n");
}
#endif /* AG_PORT_HAS_BLE_CENTRAL (helpers) */

static int cmd_ble(int argc, char **argv)
{
    ag_powerctl_bus_needed();

#if AG_PORT_HAS_BLE_PERIPH
    if (argc >= 2 && ag_path_icmp(argv[1], "adv") == 0) {
        if (ble_ensure_radio() != AG_OK) {
            ag_console_puts("ble: the radio would not start\n");
            return 1;
        }
        if (argc >= 3 && ag_path_icmp(argv[2], "off") == 0) {
            (void)ag_port_ble_adv_stop();
            ag_console_puts("advertising off\n");
            return 0;
        }
        if (argc >= 3) {
            const ag_err_t err = ag_port_ble_adv_start(argv[2]);
            if (err != AG_OK) {
                ag_console_printf("ble adv: %s\n",
                                  ag_loader_api()->sys->strerror(err));
                if (err == -AG_EIO) {
                    ag_console_puts("  name may be too long for a 31-byte "
                                    "advert; try shorter\n");
                }
                return 1;
            }
            ag_console_printf(
                "advertising as \"%s\" (service fff0: read fff1, write fff2)\n",
                argv[2]);
            ag_console_puts("  connect from a phone/PC; `ble adv` shows what "
                            "was written\n");
            return 0;
        }
        /* bare `ble adv`: status, and the last thing a client wrote. */
        ag_port_ble_adv_status_t ast;
        if (ag_port_ble_adv_status(&ast) != AG_OK || !ast.advertising) {
            ag_console_puts("not advertising - `ble adv <name>` to start\n");
            return 0;
        }
        ag_console_printf("advertising%s, %u write%s received\n",
                          ast.connected ? ", a client is connected" : "",
                          (unsigned)ast.writes, (ast.writes == 1u) ? "" : "s");
        if (ast.writes > 0u) {
            uint8_t       buf[128];
            const int32_t n = ag_port_ble_adv_last_write(buf, sizeof(buf));
            if (n > 0) {
                ag_console_printf("last write, %d bytes: \"", (int)n);
                for (int32_t i = 0; i < n; i++) {
                    const uint8_t c = buf[i];
                    ag_console_printf("%c",
                                      (c >= 0x20 && c < 0x7f) ? (char)c : '.');
                }
                ag_console_puts("\"\n");
            }
        }
        return 0;
    }

    if (argc >= 2 && ag_path_icmp(argv[1], "midi") == 0) {
        if (ble_ensure_radio() != AG_OK) {
            ag_console_puts("ble: the radio would not start\n");
            return 1;
        }
        if (argc >= 3 && ag_path_icmp(argv[2], "note") == 0) {
            if (argc < 4) {
                ag_console_puts("usage: ble midi note <0-127> [velocity]\n");
                return 1;
            }
            const int note = atoi(argv[3]);
            const int vel = (argc > 4) ? atoi(argv[4]) : 100;
            if (note < 0 || note > 127) {
                ag_console_puts("note is 0..127\n");
                return 1;
            }
            const ag_err_t e =
                ag_port_ble_midi_send(0x90u, (uint8_t)note, (uint8_t)vel);
            if (e != AG_OK) {
                ag_console_printf("ble midi note: %s\n",
                                  ag_loader_api()->sys->strerror(e));
                ag_console_puts("  is a MIDI app connected and listening?\n");
                return 1;
            }
            ag_port_task_delay(ag_port_ms_to_ticks(300));
            (void)ag_port_ble_midi_send(0x80u, (uint8_t)note, 0);
            ag_console_printf("played note %d\n", note);
            return 0;
        }
        /* `ble midi [name]`: advertise as a MIDI device. */
        const char    *nm = (argc >= 3) ? argv[2] : "ArgMIDI";
        const ag_err_t e = ag_port_ble_midi_advertise(nm);
        if (e != AG_OK) {
            ag_console_printf("ble midi: %s\n",
                              ag_loader_api()->sys->strerror(e));
            return 1;
        }
        ag_console_printf("advertising as MIDI device \"%s\"\n", nm);
        ag_console_puts("  connect a MIDI app; `ble midi note <0-127>` tests "
                        "it\n");
        return 0;
    }
#endif /* AG_PORT_HAS_BLE_PERIPH */

#if AG_PORT_HAS_BLE_CENTRAL
    if (argc < 2 || ag_path_icmp(argv[1], "scan") == 0) {
        return ble_scan();
    }

    if (ag_path_icmp(argv[1], "connect") == 0) {
        if (argc < 3) {
            ag_console_puts("usage: ble connect <#number|addr> [random]\n");
            ag_console_puts("  #number is a line from the last `ble scan`\n");
            return 1;
        }
        if (ble_ensure_radio() != AG_OK) {
            ag_console_puts("ble: the radio would not start\n");
            return 1;
        }

        uint8_t     addr[6];
        int         addr_type = 0;
        const char *a = argv[2];
        const char *digits = (a[0] == '#') ? a + 1 : a;
        bool        numeric = (digits[0] >= '1' && digits[0] <= '9');
        for (const char *p = digits; numeric && *p != '\0'; p++) {
            if (*p < '0' || *p > '9') {
                numeric = false;
            }
        }
        if (numeric) {
            const uint32_t nfld = (uint32_t)atoi(digits);
            if (s_ble_list_n == 0u || nfld == 0u || nfld > s_ble_list_n) {
                ag_console_printf(
                    "no #%u in the last scan; run `ble scan` first\n",
                    (unsigned)nfld);
                return 1;
            }
            memcpy(addr, s_ble_list[nfld - 1u].addr, 6);
            addr_type = s_ble_list[nfld - 1u].addr_type;
        } else if (!bt_parse_addr(argv[2], addr)) {
            ag_console_puts("that is not a hardware address (or a #number)\n");
            return 1;
        } else if (argc > 3 && ag_path_icmp(argv[3], "random") == 0) {
            addr_type = 1; /* BLE random address, what phones usually use */
        }

        ag_console_puts("connecting...\n");
        const ag_err_t err = ag_port_ble_connect(addr, addr_type, 10000);
        if (err != AG_OK) {
            ag_console_printf("ble connect: %s\n",
                              ag_loader_api()->sys->strerror(err));
            return 1;
        }
        ag_console_puts("connected\n");
        return ble_discover_print();
    }

    if (ag_path_icmp(argv[1], "disconnect") == 0) {
        (void)ag_port_ble_disconnect();
        ag_console_puts("disconnected\n");
        return 0;
    }

    if (!ag_port_ble_connected()) {
        ag_console_puts("not connected - `ble connect <#|addr>` first\n");
        return 1;
    }

    if (ag_path_icmp(argv[1], "services") == 0) {
        return ble_discover_print();
    }

    if (ag_path_icmp(argv[1], "read") == 0) {
        if (argc < 3) {
            ag_console_puts("usage: ble read <handle>   (e.g. 0x002a)\n");
            return 1;
        }
        const unsigned long h = strtoul(argv[2], NULL, 0);
        if (h == 0ul || h > 0xfffful) {
            ag_console_puts("handle is 0x0001..0xffff (see `ble services`)\n");
            return 1;
        }
        uint8_t       buf[AG_BLE_VAL_MAX];
        const int32_t n = ag_port_ble_read((uint16_t)h, buf, sizeof(buf), 5000);
        if (n < 0) {
            ag_console_printf("ble read: %s\n",
                              ag_loader_api()->sys->strerror((ag_err_t)n));
            return 1;
        }
        ble_print_value(buf, (uint32_t)n);
        return 0;
    }

    if (ag_path_icmp(argv[1], "write") == 0) {
        if (argc < 4) {
            ag_console_puts("usage: ble write <handle> <text...>\n");
            return 1;
        }
        const unsigned long h = strtoul(argv[2], NULL, 0);
        if (h == 0ul || h > 0xfffful) {
            ag_console_puts("handle is 0x0001..0xffff (see `ble services`)\n");
            return 1;
        }
        char   msg[AG_BLE_VAL_MAX];
        size_t m = 0;
        for (int i = 3; i < argc && m < sizeof(msg); i++) {
            if (i > 3 && m < sizeof(msg)) {
                msg[m++] = ' ';
            }
            const size_t room = sizeof(msg) - m;
            const size_t al = strlen(argv[i]);
            const size_t take = (al < room) ? al : room;
            memcpy(msg + m, argv[i], take);
            m += take;
        }
        const ag_err_t err =
            ag_port_ble_write((uint16_t)h, msg, (uint32_t)m, true, 5000);
        if (err != AG_OK) {
            ag_console_printf("ble write: %s\n",
                              ag_loader_api()->sys->strerror(err));
            return 1;
        }
        ag_console_printf("wrote %u bytes\n", (unsigned)m);
        return 0;
    }

    ag_console_puts("usage: ble [scan | connect <#|addr> [random] | services | "
                    "read <handle> | write <handle> <text> | disconnect"
#if AG_PORT_HAS_BLE_PERIPH
                    " | adv <name> | adv off | midi [name] | midi note <n>"
#endif
                    "]\n");
    return 1;
#else  /* peripheral-only build: no central verbs */
    (void)argc;
    (void)argv;
    ag_console_puts(
        "usage: ble [adv <name> | adv off | midi [name] | midi note <n>]\n");
    return 1;
#endif /* AG_PORT_HAS_BLE_CENTRAL */
}

#endif /* AG_PORT_HAS_BLE_CENTRAL || AG_PORT_HAS_BLE_PERIPH */

#endif /* AG_PORT_HAS_BT */

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

    /*
     * Analogue input.  A separate word rather than a mode of `io <pin>`,
     * because what is asked for is a channel and not a pin: which pin a
     * channel measures is the chip's business, and the two numbers are the
     * same only by accident and only on some parts.  Without an argument it
     * reads every channel the chip has, which is what somebody looking for
     * where a sensor is actually wired wants.
     */
    if (argc >= 2 && ag_path_icmp(argv[1], "adc") == 0) {
        if (!AG_HAS(io, adc_read)) {
            ag_console_puts("this build has no analogue input "
                            "(CONFIG_ARGON_ENABLE_ADC)\n");
            return 1;
        }
        const int first = (argc >= 3) ? atoi(argv[2]) : 0;
        const int last = (argc >= 3) ? first : AG_PORT_ADC_CHANNELS - 1;

        for (int ch = first; ch <= last; ch++) {
            const int32_t raw = io->adc_read(ch);
            if (raw == -AG_ENOTSUP) {
                if (argc >= 3) {
                    ag_console_printf("adc %d: no pin on this chip\n", ch);
                }
                continue; /* in a sweep, a channel that goes nowhere is noise */
            }
            if (raw < 0) {
                ag_console_printf("adc %d: %s\n", ch,
                                  ag_loader_api()->sys->strerror((ag_err_t)raw));
                continue;
            }
            ag_console_printf("adc %d (pin %d): %d\n", ch,
                              AG_PORT_ADC_GPIO(ch), (int)raw);
        }
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

    /*
     * And what was refused.  A dropped write leaves no other trace anywhere in
     * the system: the API returns nothing, the pin does not move, and whatever
     * was being driven simply misbehaves.  This line is where that becomes
     * visible.
     */
    int      bad_pin = -1;
    ag_pid_t bad_pid = 0;
    const uint32_t refused = ag_io_refused(&bad_pin, &bad_pid);
    if (refused != 0) {
        ag_console_printf("%u write%s refused - last pin %d by pid %u "
                          "(not its claim)\n",
                          (unsigned)refused, (refused == 1u) ? "" : "s",
                          bad_pin, (unsigned)bad_pid);
    }
    return 0;
}

/*
 * The file manager lives in the kernel image (same sources as apps/fm) but runs
 * as a real process in the current session slot so Alt+N leaves it alive.
 */
int ag_fm_main(int argc, char **argv);

static int cmd_fm(int argc, char **argv)
{
    if (ag_session_is_system()) {
        ag_console_puts("fm: use a user slot (Alt+1..4)\n");
        return 1;
    }

    const int target_slot = ag_session_focused();

    /* Already have FM in this slot — just focus it. */
    {
        ag_session_slot_t slots[AG_SESSION_SLOTS];
        ag_session_info(slots);
        if (slots[target_slot].pid != AG_PID_KERNEL &&
            ag_path_icmp(slots[target_slot].name, "FM") == 0) {
            (void)ag_session_focus(target_slot);
            return 0;
        }
    }

    ag_pid_t       pid = 0;
    /*
     * Panels need ~100 KB + viewer/copy headroom — not the 1 MB default heap.
     * Stack 8 KB is enough for FM; smaller stacks still prefer scarce SRAM.
     */
    const ag_err_t err = ag_proc_spawn_builtin(
        "FM", ag_fm_main, argc, argv,
        (uint32_t)AG_SPAWN_BACKGROUND | (uint32_t)AG_SPAWN_NO_SESSION,
        /*
         * The default arena rather than a quarter of a megabyte.  A fixed
         * request is a refusal to start on any machine that does not have it,
         * and this board has 93 KB free in total: `fm` answered "asked for a
         * 256 KB arena; there is none".  The panels now take what they are
         * given and show fewer names when that is less.
         */
        8u * 1024u, 0u, &pid);
    if (err != AG_OK) {
        ag_console_printf("fm: could not start (%d)\n", (int)err);
        return 1;
    }

    if (ag_session_bind_to(pid, "FM", target_slot) != AG_OK) {
        (void)ag_session_bind(pid, "FM");
    }

    const int slot = ag_session_slot_of(pid);
    /* Journal only — printing here races the FM screen and paints over it. */
    ag_log(AG_LOG_INFO, "shell", "fm pid %u in slot %d", (unsigned)pid,
           slot >= 0 ? ag_session_display_number(slot) : -1);

    if (slot >= 0 && ag_session_focused() == target_slot) {
        (void)ag_session_focus(slot);
    }
    return 0;
}

static ag_pid_t resolve_slot_or_pid(const char *arg)
{
    const int n = atoi(arg);
    if (n >= 1 && n <= AG_SESSION_SLOTS) {
        ag_session_slot_t slots[AG_SESSION_SLOTS];
        ag_session_info(slots);
        return slots[n - 1].pid;
    }
    return (ag_pid_t)n;
}

static int cmd_prio(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        ag_console_puts("usage: prio <slot|pid> [low|normal|high]\n");
        return 1;
    }

    const ag_pid_t pid = resolve_slot_or_pid(argv[1]);
    if (pid == AG_PID_KERNEL) {
        ag_console_puts("no process in that slot\n");
        return 1;
    }

    if (argc == 2) {
        ag_proc_prio_t cur;
        if (ag_proc_get_priority(pid, &cur) != AG_OK) {
            ag_console_printf("pid %u not found\n", (unsigned)pid);
            return 1;
        }
        ag_console_printf("pid %u priority %s\n", (unsigned)pid,
                          ag_proc_prio_name(cur));
        return 0;
    }

    ag_proc_prio_t want = AG_PRIO_NORMAL;
    if (ag_path_icmp(argv[2], "low") == 0) {
        want = AG_PRIO_LOW;
    } else if (ag_path_icmp(argv[2], "normal") == 0) {
        want = AG_PRIO_NORMAL;
    } else if (ag_path_icmp(argv[2], "high") == 0) {
        want = AG_PRIO_HIGH;
    } else {
        ag_console_puts("priority must be low, normal, or high\n");
        return 1;
    }

    const ag_err_t err = ag_proc_set_priority(pid, want);
    if (err != AG_OK) {
        ag_console_printf("could not set priority: %d\n", (int)err);
        return 1;
    }
    ag_console_printf("pid %u priority %s\n", (unsigned)pid,
                      ag_proc_prio_name(want));
    return 0;
}

static int cmd_edit(int argc, char **argv)
{
    return ag_edit_main(argc, argv);
}

static int cmd_gfxdump(int argc, char **argv)
{
    if (argc < 2) {
        ag_console_puts("usage: gfxdump [/live] <file.ppm>\n");
        return 1;
    }
    if (!ag_display_ready()) {
        ag_console_puts("no display\n");
        return 1;
    }

    int argi = 1;
    bool live = false;
    if (ag_path_icmp(argv[1], "/live") == 0) {
        live = true;
        argi++;
    }
    if (argi >= argc) {
        ag_console_puts("usage: gfxdump [/live] <file.ppm>\n");
        return 1;
    }

    /* /live: paint the text console onto the soft fb, then dump that buffer. */
    if (live && !ag_display_acquired() && ag_console_ready()) {
        ag_console_lock();
        ag_screen_mark_all_dirty(ag_console_screen());
        ag_display_render_console(ag_console_screen());
        ag_console_unlock();
    }

    const ag_err_t err = ag_display_dump_ppm(argv[argi], ag_shell_cwd(), live);
    if (err != AG_OK) {
        ag_console_printf("gfxdump: %d\n", (int)err);
        return 1;
    }
    ag_console_printf("wrote %s\n", argv[argi]);
    return 0;
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

static int cmd_slots(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ag_session_slot_t slots[AG_SESSION_SLOTS];
    ag_session_info(slots);
    const int focused = ag_session_focused();

    ag_console_puts("  slot  pid   prio     name\n");
    ag_console_printf("  sys%s  -     -        system\n",
                      focused == AG_SESSION_SYSTEM ? "*" : " ");
    for (int i = 0; i < AG_SESSION_SLOTS; i++) {
        const int shown = ag_session_display_number(i);
        if (slots[i].pid == AG_PID_KERNEL) {
            ag_console_printf("  %d%s   -     -        shell\n", shown,
                              focused == i ? "*" : " ");
        } else {
            ag_proc_prio_t pr = AG_PRIO_NORMAL;
            (void)ag_proc_get_priority(slots[i].pid, &pr);
            ag_console_printf("  %d%s   %-4u  %-7s  %s\n", shown,
                              focused == i ? "*" : " ",
                              (unsigned)slots[i].pid, ag_proc_prio_name(pr),
                              slots[i].name[0] ? slots[i].name : "?");
        }
    }
    return 0;
}

static int cmd_fg(int argc, char **argv)
{
    if (argc != 2) {
        ag_console_printf("usage: fg <slot|pid>   (slot is 1..4)\n");
        return 1;
    }

    if (ag_path_icmp(argv[1], "sys") == 0) {
        ag_console_puts("fg: system shell is Ctrl+\\ only\n");
        return 1;
    }

    const int n = atoi(argv[1]);

    /* 1..4 → display slot number */
    if (n >= 1 && n <= AG_SESSION_SLOTS) {
        (void)ag_session_focus(n - 1);
        return 0;
    }

    const ag_pid_t pid = (ag_pid_t)n;
    const int      slot = ag_session_slot_of(pid);
    if (slot < 0) {
        ag_console_printf("no process with pid %u\n", (unsigned)pid);
        return 1;
    }
    (void)ag_session_focus(slot);
    return 0;
}

/*
 * A tone, for finding out whether this machine can make a sound at all.
 *
 * /dev/pcm0 exists only where the port has an output (argon/port/audio.h), so
 * on a board with nothing wired up this says so, rather than playing into a
 * sink that discards it - which is the whole question being asked.
 */

/*
 * A sine without a table and without floating point.
 *
 * Phase runs 0..65535 over one turn and the curve is the parabola 4x(1-|x|),
 * which follows a sine to within about four percent.  That is a distortion
 * floor near -25 dB, and what it feeds is an eight-bit converter whose own
 * floor is -48 dB and a speaker the size of a coin.
 */
static int16_t beep_sine(uint16_t phase)
{
    const int32_t t = (int32_t)phase - 32768;
    const int32_t a = (t < 0) ? -t : t;
    const int32_t y = (t * (32768 - a)) >> 13;
    return (int16_t)(y > 32767 ? 32767 : y);
}

static int cmd_beep(int argc, char **argv)
{
    unsigned hz = 880u;
    unsigned ms = 300u;

    if (argc > 1) {
        hz = (unsigned)strtoul(argv[1], NULL, 10);
    }
    if (argc > 2) {
        ms = (unsigned)strtoul(argv[2], NULL, 10);
    }
    if (hz < 30u || hz > 8000u || ms == 0u || ms > 10000u) {
        ag_console_printf("usage: beep [hz 30..8000] [ms 1..10000]\n");
        return 1;
    }

    ag_device_t *dev = ag_dev_find("pcm0");
    if (dev == NULL) {
        ag_console_printf(
            "no sound output on this machine (/dev/pcm0 is absent)\n");
        return 1;
    }

    ag_err_t err = ag_dev_open(dev, AG_O_WRONLY);
    if (err != AG_OK) {
        ag_console_printf("pcm0: %d\n", (int)err);
        return 1;
    }

    const uint32_t rate = 22050u;
    ag_audio_fmt_t fmt = {rate, 1u, 16u};
    err = ag_dev_ioctl(dev, AG_IOC_AUDIO_SETFMT, &fmt, sizeof(fmt));
    if (err != AG_OK) {
        (void)ag_dev_close(dev);
        ag_console_printf("pcm0 setfmt: %d\n", (int)err);
        return 1;
    }

    const uint32_t total = (rate * ms) / 1000u;
    /* Phase advances by a whole turn every rate/hz samples, and a turn is
     * 65536, so this is the step per sample. */
    const uint16_t step = (uint16_t)(((uint64_t)hz << 16) / rate);
    /* Five milliseconds of fade at each end, or a tenth of the tone if it is
     * shorter than that: without it the start and the stop are clicks of
     * their own and the tone is not what is being heard. */
    uint32_t ramp = rate / 200u;
    if (ramp > total / 10u) {
        ramp = total / 10u;
    }

    int16_t  buf[128];
    uint16_t phase = 0;
    uint32_t sent = 0;

    while (sent < total) {
        uint32_t n = total - sent;
        if (n > (uint32_t)(sizeof(buf) / sizeof(buf[0]))) {
            n = (uint32_t)(sizeof(buf) / sizeof(buf[0]));
        }
        for (uint32_t i = 0; i < n; i++) {
            const int32_t  v = beep_sine(phase);
            const uint32_t at = sent + i;
            int32_t        gain = 192; /* of 256: room under the rails */

            phase = (uint16_t)(phase + step);
            if (ramp != 0u) {
                if (at < ramp) {
                    gain = (int32_t)((192u * at) / ramp);
                } else if (at + ramp > total) {
                    gain = (int32_t)((192u * (total - at)) / ramp);
                }
            }
            buf[i] = (int16_t)((v * gain) >> 8);
        }
        const int32_t wrote =
            ag_dev_write(dev, buf, (size_t)n * sizeof(int16_t), 0);
        if (wrote <= 0) {
            ag_console_printf("pcm0 write: %d\n", (int)wrote);
            break;
        }
        sent += n;
    }

    (void)ag_dev_close(dev);
    ag_console_printf("%u Hz for %u ms on /dev/pcm0\n", hz, ms);
    return 0;
}

static const ag_command_t k_commands[] = {
    {"help", "", "list these commands", cmd_help},
    {"ver", "", "version and hardware", cmd_ver},
    {"mem", "", "memory usage", cmd_mem},
    {"boot", "[recovery|normal]", "boot report; set/clear recovery marker",
     cmd_boot},
    {"log", "[-n N|clear]", "system journal", cmd_log},
    {"uptime", "", "time since reset", cmd_uptime},
    {"cls", "", "clear the screen", cmd_cls},
    {"echo", "<text>", "print text", cmd_echo},
    {"color", "<fg> <bg>", "set text colours", cmd_color},
    {"chcp", "[437|866|1251]", "screen code page", cmd_chcp},
    {"fm", "[left] [right]", "file manager, two panels", cmd_fm},
    {"edit", "[file]", "create or edit a text file", cmd_edit},
    {"gfxdump", "[/live] <file.ppm>", "save framebuffer as PPM", cmd_gfxdump},
    {"dev", "[name]", "list devices, or describe one", cmd_dev},
    {"drv", "[load|unload|install|uninstall|probe]",
     "modules: list, load, install to C:, unload, I2C probe", cmd_drv},
    {"io", "[pin [mode]] | i2c <bus> | adc [ch]", "pins and buses", cmd_io},
    {"beep", "[hz] [ms]", "a tone on /dev/pcm0", cmd_beep},
    {"power", "[full|eco|doze|screen on|off|auto on|off]",
     "the clock, the screen, and what applications make of it", ag_cmd_power},
#if AG_PORT_HAS_WIFI
    {"wifi", "[on|off|scan|connect <#n|ssid> [pass]|ap <ssid> [pass]|forget]",
     "the radio", cmd_wifi},
#endif
#if AG_PORT_HAS_ESPNOW
    {"espnow", "[on|off|peer <mac> [ch]|send <mac|bcast> <text>|listen]",
     "board-to-board, no access point", cmd_espnow},
#endif
#if AG_PORT_HAS_WIFIMON
    {"mon", "[on|off|watch|hop|channel <n>|tx <hex>|deauth <bssid>|beacon]",
     "watch the air and inject frames", cmd_mon},
#endif
#if AG_HAS_NET
    {"net", "[wait|resolve <name>]", "address, waiting, and names into addresses",
     ag_cmd_net},
    {"wget", "<url> [file]", "fetch a file over http or ftp",
     ag_cmd_wget},
    {"ftp", "<host> [user] [pass]", "file transfer session", ag_cmd_ftp},
    {"httpd", "[port] [dir] [/w]", "serve a directory (/w: accept files)",
     ag_cmd_httpd},
#endif
#if AG_PORT_HAS_BT
    {"bt", "[on|off|scan|open <#|addr>|close|forget]", "bluetooth input", cmd_bt},
#endif
#if AG_PORT_HAS_BLE_CENTRAL || AG_PORT_HAS_BLE_PERIPH
    {"ble", "[scan|connect|services|read|write|disconnect|adv <name>]",
     "scan, talk to, or be a BLE device", cmd_ble},
#endif
    {"ps", "", "list running applications", cmd_ps},
    {"prio", "<slot|pid> [low|normal|high]", "show or set process priority",
     cmd_prio},
    {"slots", "", "list session slots (* = focused)", cmd_slots},
    {"kill", "<pid>", "stop an application", cmd_kill},
    {"fg", "<slot|pid>", "focus a session slot (or pid's slot)", cmd_fg},
    {"dir", "[path]", "list a directory", ag_cmd_dir},
    {"cd", "[path]", "change directory", ag_cmd_cd},
    {"type", "<file>", "print a file", ag_cmd_type},
    {"copy", "<src> <dst>", "copy a file", ag_cmd_copy},
    {"del", "<pattern>", "delete files", ag_cmd_del},
    {"md", "<path>", "make a directory", ag_cmd_mkdir},
    {"rd", "<path>", "remove a directory", ag_cmd_rmdir},
    {"ren", "<old> <new>", "rename a file", ag_cmd_rename},
    {"mount", "[a:]", "list drives, or mount the card again", ag_cmd_mount},
    {"eject", "", "release the card so it can be taken out", ag_cmd_eject},
    {"format", "<drive> [/y]", "make a fresh filesystem", ag_cmd_format},
    {"hexdump", "<file>", "dump a file as bytes", ag_cmd_hexdump},
    {"recv", "<file>", "receive a file as hex", ag_cmd_recv},
    {"unzip", "<zip> [dest]", "list or extract a zip archive", ag_cmd_unzip},
    {"run", "[/b] <file> [args]", "run an application or .bat script", cmd_run},
    {"call", "<script.bat>", "run a shell command script", cmd_call},
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

void ag_shell_clear_interrupted(void) { ag_supervisor_clear_interrupt(); }

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

    /*
     * While a command runs, show journal lines on the console (module load,
     * probe, etc.).  Echo stays off at the live prompt so async noise does
     * not fight the edit line.
     */
    ag_log_set_echo(true);

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

    char resolved[AG_PATH_MAX];
    if (!found) {
        const ag_err_t rerr =
            ag_shell_resolve_cmd(argv[0], s_cwd, shell_path_env(), resolved,
                                 sizeof(resolved));
        if (rerr == AG_OK) {
            argv[0] = resolved;
            status = launch_resolved(resolved, argc, argv, false);
            found = true;
        }
    }

    if (!found && ag_shell_open_associated(argv[0])) {
        status = 0;
        found = true;
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

    ag_log_set_echo(false);
    return status;
}

bool ag_shell_open_associated(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return false;
    }

    char abs[AG_PATH_MAX];
    if (ag_path_resolve(path, s_cwd, abs, sizeof(abs)) != AG_OK) {
        return false;
    }

    const ag_cfg_t *cfg = ag_sysconfig();
    const char *handler = ag_shell_assoc_lookup(cfg, abs);
    if (handler == NULL || handler[0] == '\0') {
        return false;
    }

    if (ag_path_icmp(handler, "edit") == 0) {
        char *eargv[2] = {(char *)"edit", abs};
        (void)ag_edit_main(2, eargv);
        return true;
    }

    char prog[AG_PATH_MAX];
    if (ag_shell_resolve_cmd(handler, s_cwd, shell_path_env(), prog,
                             sizeof(prog)) != AG_OK) {
        ag_console_printf("assoc: cannot find handler '%s'\n", handler);
        return true; /* association existed; failed to run it */
    }

    char *pargv[2] = {prog, abs};
    (void)spawn_in_slot(prog, 2, pargv, false);
    return true;
}

int ag_shell_run_script(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return 1;
    }

    char abs[AG_PATH_MAX];
    if (ag_path_resolve(path, s_cwd, abs, sizeof(abs)) != AG_OK) {
        ag_console_printf("%s: bad path\n", path);
        return 1;
    }

    if (s_script_depth >= AG_SHELL_SCRIPT_MAX_DEPTH) {
        ag_console_printf("%s: script nesting too deep\n", abs);
        return 1;
    }

    ag_stat_t st;
    if (ag_vfs_stat(abs, NULL, &st) != AG_OK || (st.attr & AG_A_DIR) != 0) {
        ag_console_printf("%s: not found\n", abs);
        return 1;
    }

    const ag_handle_t h = ag_vfs_open(abs, NULL, AG_O_RDONLY);
    if (h < 0) {
        ag_console_printf("%s: open failed (%d)\n", abs, (int)h);
        return 1;
    }

    s_script_depth++;
    int status = 0;

    char    line[AG_LINE_MAX];
    size_t  len = 0;
    char    ch;
    int32_t n;
    while ((n = ag_vfs_read(h, &ch, 1)) == 1) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            line[len] = '\0';
            if (!ag_shell_autoexec_skip_line(line)) {
                status = ag_shell_execute(line);
                if (ag_shell_interrupted()) {
                    break;
                }
            }
            len = 0;
            continue;
        }
        if (len + 1 < sizeof(line)) {
            line[len++] = ch;
        }
    }
    if (len > 0 && !ag_shell_interrupted()) {
        line[len] = '\0';
        if (!ag_shell_autoexec_skip_line(line)) {
            status = ag_shell_execute(line);
        }
    }
    ag_vfs_close(h);
    s_script_depth--;
    return status;
}

static void run_autoexec(void)
{
    if (ag_boot_in_recovery()) {
        return;
    }

    const ag_cfg_t *cfg = ag_sysconfig();
    const char *configured =
        (cfg != NULL) ? ag_cfg_get(cfg, "shell.autoexec", NULL) : NULL;

    char path[AG_PATH_MAX];
    if (configured != NULL && configured[0] != '\0') {
        if (ag_path_resolve(configured, s_cwd, path, sizeof(path)) != AG_OK) {
            ag_log(AG_LOG_WARN, "shell", "autoexec: bad path '%s'", configured);
            return;
        }
    } else {
        strncpy(path, "/sys/AUTOEXEC.BAT", sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }

    ag_stat_t st;
    if (ag_vfs_stat(path, NULL, &st) != AG_OK || (st.attr & AG_A_DIR) != 0) {
        return;
    }

    ag_console_printf("AUTOEXEC: %s\n", path);
    const int status = ag_shell_run_script(path);
    if (status != 0) {
        ag_log(AG_LOG_WARN, "shell", "autoexec finished with %d", status);
    }
}

static void show_prompt(void)
{
    build_prompt(s_prompt, sizeof(s_prompt));

    ag_console_lock();
    ag_screen_t *sc = ag_console_screen();
    /* Start the prompt on a fresh line so output never runs into it. */
    if (sc->cur_x != 0) {
        ag_screen_puts(sc, "\n");
    }
    ag_screen_puts(sc, s_prompt);
    s_prompt_pos.row = sc->cur_y;
    s_prompt_pos.col0 = sc->cur_x;
    /* Armed under the same lock so a log cannot land between paint and live. */
    ag_console_set_live(live_restore, NULL);
    ag_console_unlock();
}

void ag_shell_run(void)
{
    ag_lineedit_init(&s_line);
    pick_initial_cwd();

    /*
     * Boot already echoed to the screen.  At the live prompt, system messages
     * stay in the journal only — pcmvirt stats and friends must not break the
     * edit line (`log` still shows them).  ag_shell_execute turns echo back
     * on for the duration of each command so install/load progress is visible.
     */
    ag_log_set_echo(false);

    if (ag_boot_in_recovery()) {
        ag_console_printf(
            "\nRECOVERY: modules skipped (%s). Use 'boot normal' then reboot "
            "when the board is healthy.\n",
            ag_boot_recovery_reason());
    }

    /* Streak clears once a prompt is reachable — not when AUTOEXEC finishes. */
    ag_boot_recovery_shell_ok();

    run_autoexec();

    ag_console_puts("\nType 'help' for a list of commands.\n");
    ag_console_puts(
        "Alt+1..4 / Alt+Tab = user slots; Ctrl+\\ = system shell "
        "(again = kill last app).\n");

    for (;;) {
        /* App in the focused slot owns the keyboard; shell waits. */
        while (!ag_session_shell_owns_keyboard()) {
            ag_port_task_delay(ag_port_ms_to_ticks(50));
        }

        sync_cwd_from_session();
        ag_shell_clear_interrupted();

        ag_lineedit_reset(&s_line);
        show_prompt();
        redraw_line(&s_line, &s_prompt_pos);

        bool done = false;
        while (!done) {
            if (!ag_session_shell_owns_keyboard()) {
                ag_console_lock();
                ag_console_set_live(NULL, NULL);
                ag_console_unlock();
                break;
            }

            ag_event_t ev;
            if (!ag_console_read_event(&ev, 50)) {
                continue;
            }
            if (ev.type == AG_EV_QUIT) {
                /* Session switch injected this — redraw prompt for new slot. */
                ag_console_lock();
                ag_console_set_live(NULL, NULL);
                ag_console_unlock();
                done = true;
                break;
            }

            switch (ag_lineedit_key(&s_line, &ev)) {
            case AG_LINE_CHANGED:
                redraw_line(&s_line, &s_prompt_pos);
                break;

            case AG_LINE_DONE:
                ag_console_lock();
                ag_console_set_live(NULL, NULL);
                ag_screen_puts(ag_console_screen(), "\n");
                ag_console_unlock();
                if (s_line.len > 0) {
                    ag_lineedit_remember(&s_line, s_line.buf);
                    s_last_status = ag_shell_execute(s_line.buf);
                }
                done = true;
                break;

            case AG_LINE_CANCEL:
                ag_console_lock();
                ag_console_set_live(NULL, NULL);
                ag_screen_puts(ag_console_screen(), "^C\n");
                ag_console_unlock();
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
