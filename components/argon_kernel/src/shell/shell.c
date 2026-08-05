/*
 * ArgonOS - built-in shell.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/shell.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <argon/cmdline.h>
#include <argon/console.h>
#include <argon/kernel.h>
#include <argon/lineedit.h>
#include <argon/path.h>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "boot/platform.h"

typedef struct {
    const char *name;
    const char *usage;
    const char *help;
    int (*fn)(int argc, char **argv);
} ag_command_t;

static char          s_cwd[AG_PATH_MAX] = "/sd";
static ag_lineedit_t s_line;
static int           s_last_status;

/* ---------------------------------------------------------------------- */
/* Prompt                                                                 */
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

/*
 * Renders the working directory the way DOS would have: A:\APPS> rather than
 * /sd/apps>.  The kernel deals only in POSIX paths; this is presentation.
 */
static void build_prompt(char *out, size_t n)
{
    for (size_t i = 0; i < sizeof(k_drives) / sizeof(k_drives[0]); i++) {
        const char  *mount = k_drives[i].mount;
        const size_t len = strlen(mount);

        if (strncmp(s_cwd, mount, len) != 0) {
            continue;
        }
        if (s_cwd[len] != '\0' && s_cwd[len] != '/') {
            continue;
        }

        size_t w = 0;
        if (w + 4 < n) {
            out[w++] = k_drives[i].letter;
            out[w++] = ':';
            out[w++] = '\\';
        }
        for (const char *p = s_cwd + len + (s_cwd[len] == '/' ? 1 : 0);
             *p != '\0' && w + 2 < n; p++) {
            out[w++] = (*p == '/') ? '\\' : *p;
        }
        if (w + 2 < n) {
            out[w++] = '>';
        }
        out[w] = '\0';
        return;
    }

    snprintf(out, n, "%s>", s_cwd);
}

/* ---------------------------------------------------------------------- */
/* Line rendering                                                         */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint16_t row;
    uint16_t col0;
} prompt_pos_t;

/* Display columns occupied by the first `bytes` bytes of UTF-8 text. */
static uint16_t cols_of(const char *s, uint16_t bytes)
{
    uint16_t cols = 0;
    for (uint16_t i = 0; i < bytes; i++) {
        if (((unsigned char)s[i] & 0xc0u) != 0x80u) {
            cols++;
        }
    }
    return cols;
}

static uint16_t byte_of_col(const char *s, uint16_t len, uint16_t col)
{
    uint16_t cols = 0;
    for (uint16_t i = 0; i < len; i++) {
        if (((unsigned char)s[i] & 0xc0u) != 0x80u) {
            if (cols == col) {
                return i;
            }
            cols++;
        }
    }
    return len;
}

static void redraw_line(const ag_lineedit_t *le, const prompt_pos_t *pos)
{
    ag_console_lock();
    ag_screen_t *sc = ag_console_screen();

    const uint16_t avail =
        (sc->cols > pos->col0 + 1) ? (uint16_t)(sc->cols - pos->col0) : 1;
    const uint16_t cursor_col = cols_of(le->buf, le->cursor);

    /*
     * A line longer than the screen scrolls horizontally rather than wrapping;
     * wrapping would move the prompt row and lose track of where to redraw.
     */
    const uint16_t offset =
        (cursor_col >= avail) ? (uint16_t)(cursor_col - avail + 1) : 0;

    ag_screen_gotoxy(sc, pos->col0, pos->row);

    uint16_t shown = 0;
    uint16_t i = byte_of_col(le->buf, le->len, offset);
    while (i < le->len && shown < avail) {
        uint16_t next = (uint16_t)(i + 1);
        while (next < le->len &&
               ((unsigned char)le->buf[next] & 0xc0u) == 0x80u) {
            next++;
        }
        ag_screen_write(sc, le->buf + i, (size_t)(next - i));
        i = next;
        shown++;
    }
    for (uint16_t x = shown; x < avail; x++) {
        ag_screen_putc_raw(sc, ' ');
    }

    ag_screen_gotoxy(sc, (uint16_t)(pos->col0 + cursor_col - offset), pos->row);
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
    ag_console_printf("board %s, ABI %u.%u, application core %u\n", si->board,
                      si->abi_major, si->abi_minor, si->app_core);
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

/* Filesystem commands wait for the VFS; say so plainly rather than lying. */
static int cmd_not_ready(int argc, char **argv)
{
    (void)argc;
    ag_console_printf("%s: no filesystem mounted (drive not ready)\n", argv[0]);
    return 1;
}

static int cmd_ps(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ag_console_puts("  pid  name              state        memory\n");
    ag_console_puts("  no applications loaded\n");
    return 0;
}

static const ag_command_t k_commands[] = {
    {"help", "", "list these commands", cmd_help},
    {"ver", "", "version and hardware", cmd_ver},
    {"mem", "", "memory usage", cmd_mem},
    {"boot", "", "boot stage report", cmd_boot},
    {"uptime", "", "time since reset", cmd_uptime},
    {"cls", "", "clear the screen", cmd_cls},
    {"echo", "<text>", "print text", cmd_echo},
    {"color", "<fg> <bg>", "set text colours", cmd_color},
    {"ps", "", "list running applications", cmd_ps},
    {"dir", "[path]", "list a directory", cmd_not_ready},
    {"cd", "<path>", "change directory", cmd_not_ready},
    {"type", "<file>", "print a file", cmd_not_ready},
    {"run", "<file>", "run an application", cmd_not_ready},
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
        "and down.\n");
    return 0;
}

/* ---------------------------------------------------------------------- */

const char *ag_shell_cwd(void) { return s_cwd; }

int ag_shell_execute(const char *line)
{
    char  work[AG_LINE_MAX];
    char *argv[AG_ARGV_MAX];

    if (line == NULL) {
        return 0;
    }
    strncpy(work, line, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    const int argc = ag_cmdline_split(work, argv, AG_ARGV_MAX);
    if (argc == 0) {
        return 0;
    }

    for (int i = 0; k_commands[i].name != NULL; i++) {
        if (ag_path_icmp(argv[0], k_commands[i].name) == 0) {
            return k_commands[i].fn(argc, argv);
        }
    }

    /* The message every DOS user knows. */
    ag_console_printf("Bad command or file name: %s\n", argv[0]);
    return 127;
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
