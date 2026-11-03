/*
 * ArgonOS - MDMENU: the Mega Drive's game list.
 *
 *   run c:\mdmenu.axe [folder]
 *
 * The cartridges (*.SWP, byte-swapped as MD reads them) in the folder this
 * program was started from, or the one named, and its GAMES folder, as a list
 * on the console - drawn as text, so the whole of the machine's memory is the
 * game's while one runs.  Chosen with the pad (the phone's, a keyboard's
 * arrows and Enter, or a real one) and started with the Mega Drive next to
 * this program: MDS3.AXE, MD-NO-PSRAM.AXE or MD.AXE, whichever is there.  When
 * the game ends - QUIT TO LIST in its pause menu - the list is back.  A game
 * with a save state beside it (the same name, .SAV) is marked.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>
#include <argon/libc.h>

#include <stdio.h>
#include <string.h>

AG_APP_SIZED("MDMENU", "1.0", "argon", 0, 4 * 1024, 16 * 1024);

#define MAX_GAMES 64
#define NAME_LEN  40

#define PATH_LEN  96

typedef struct {
    char path[PATH_LEN];
    char name[NAME_LEN];
    bool saved;
} game_t;

static game_t *s_games;
static int     s_count;
static char    s_md[AG_PATH_MAX];

/* the folder part of `path`, with its separator */
static void dir_of(const char *path, char *out, size_t cap)
{
    size_t n = strlen(path);
    while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/' && path[n - 1] != ':') {
        n--;
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

static bool exists(const char *path)
{
    const ag_handle_t h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        return false;
    }
    (void)ag_close(h);
    return true;
}

static bool ends_with(const char *s, const char *ext)
{
    const size_t n = strlen(s), e = strlen(ext);
    if (n < e) {
        return false;
    }
    for (size_t i = 0; i < e; i++) {
        char c = s[n - e + i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c != ext[i]) {
            return false;
        }
    }
    return true;
}

static void join(char *out, size_t cap, const char *dir, const char *name)
{
    const size_t d = strlen(dir);
    const bool sep = d > 0 && dir[d - 1] != '\\' && dir[d - 1] != '/' && dir[d - 1] != ':';
    (void)snprintf(out, cap, "%s%s%s", dir, sep ? "\\" : "", name);
}

static void scan(const char *dir)
{
    const ag_handle_t d = ag_opendir(dir);
    if (d < 0) {
        return;
    }
    ag_dirent_t e;
    while (s_count < MAX_GAMES && ag_readdir(d, &e) == AG_OK) {
        if ((e.st.attr & AG_A_DIR) || !ends_with(e.name, ".SWP")) {
            continue;
        }
        game_t *g = &s_games[s_count];
        join(g->path, sizeof(g->path), dir, e.name);
        /* the name without .SWP, its underscores as spaces */
        size_t n = strlen(e.name) - 4u;
        if (n >= NAME_LEN) {
            n = NAME_LEN - 1;
        }
        for (size_t i = 0; i < n; i++) {
            g->name[i] = (e.name[i] == '_') ? ' ' : e.name[i];
        }
        g->name[n] = '\0';
        char sav[AG_PATH_MAX];
        (void)snprintf(sav, sizeof(sav), "%s", g->path);
        memcpy(sav + strlen(sav) - 4u, ".sav", 5);
        g->saved = exists(sav);
        s_count++;
    }
    (void)ag_closedir(d);
}

/* ---- the screen -------------------------------------------------------- */

static ag_coninfo_t s_ci;
static int          s_top; /* the first game on screen */

static void line(int y, const char *text, uint8_t fg, uint8_t bg)
{
    ag_fill(0, (uint16_t)y, s_ci.cols, 1, ' ', AG_ATTR(fg, bg));
    for (int x = 0; text[x] != '\0' && x < s_ci.cols; x++) {
        ag_poke((uint16_t)x, (uint16_t)y, text[x], AG_ATTR(fg, bg));
    }
}

static void draw(int sel)
{
    const int rows = (int)s_ci.rows;
    const int list = rows - 4;
    if (sel < s_top) {
        s_top = sel;
    } else if (sel >= s_top + list) {
        s_top = sel - list + 1;
    }
    char buf[96];
    line(0, "  MEGA DRIVE", AG_YELLOW, AG_BLUE);
    line(1, "", AG_WHITE, AG_BLACK);
    for (int i = 0; i < list; i++) {
        const int g = s_top + i;
        if (g >= s_count) {
            line(2 + i, "", AG_WHITE, AG_BLACK);
            continue;
        }
        (void)snprintf(buf, sizeof(buf), "  %s%s", s_games[g].name,
                       s_games[g].saved ? "  *" : "");
        if (g == sel) {
            line(2 + i, buf, AG_BLACK, AG_CYAN);
        } else {
            line(2 + i, buf, AG_WHITE, AG_BLACK);
        }
    }
    line(rows - 2, "", AG_WHITE, AG_BLACK);
    line(rows - 1, " UP/DOWN  A,START: PLAY  MENU: EXIT  *: SAVED", AG_LGRAY, AG_BLUE);
}

/* ---- the pad ----------------------------------------------------------- */

enum { B_UP = 1, B_DOWN = 2, B_PLAY = 4, B_QUIT = 8 };

static unsigned buttons(void)
{
    unsigned m = 0;
    m |= ag_btnp(0, AG_BTN_UP) ? B_UP : 0u;
    m |= ag_btnp(0, AG_BTN_DOWN) ? B_DOWN : 0u;
    m |= (ag_btnp(0, AG_BTN_B1) || ag_btnp(0, AG_BTN_C) || ag_btnp(0, AG_BTN_START))
             ? B_PLAY : 0u;
    m |= ag_btnp(0, AG_BTN_QUIT) ? B_QUIT : 0u;
    return m;
}

/* until nothing is held: the press that started or ended a game is not one
 * for the list */
static unsigned settle(void)
{
    unsigned m;
    for (int i = 0; i < 100 && (m = buttons()) != 0u; i++) {
        ag_delay(20);
    }
    return buttons();
}

static void play(const game_t *g)
{
    ag_cls();
    ag_printf("starting %s\n", g->name);
    const char *argv[2] = {s_md, g->path};
    const int32_t rc = ag_exec(s_md, 2, argv);
    if (rc < 0) {
        ag_printf("%s: %s - any button\n", s_md, ag_strerror((ag_err_t)rc));
        (void)settle();
        while (buttons() == 0u) {
            ag_delay(20);
        }
    }
}

int ag_main(int argc, char **argv)
{
    char dir[AG_PATH_MAX];
    if (argc > 1) {
        (void)snprintf(dir, sizeof(dir), "%s", argv[1]);
    } else if (argc > 0) {
        dir_of(argv[0], dir, sizeof(dir));
    } else {
        (void)snprintf(dir, sizeof(dir), "C:\\");
    }
    const char *const k_md[] = {"MDS3.AXE", "MD-NO-PSRAM.AXE", "MD.AXE"};
    char here[AG_PATH_MAX];
    if (argc > 0) {
        dir_of(argv[0], here, sizeof(here));
    } else {
        (void)snprintf(here, sizeof(here), "%s", dir);
    }
    s_md[0] = '\0';
    for (unsigned i = 0; i < sizeof(k_md) / sizeof(k_md[0]); i++) {
        join(s_md, sizeof(s_md), here, k_md[i]);
        if (exists(s_md)) {
            break;
        }
        s_md[0] = '\0';
    }
    if (s_md[0] == '\0') {
        ag_printf("mdmenu: no MDS3.AXE, MD-NO-PSRAM.AXE or MD.AXE in %s\n", here);
        return 1;
    }

    s_games = (game_t *)ag_malloc(sizeof(game_t) * MAX_GAMES);
    if (s_games == NULL) {
        ag_printf("mdmenu: no memory for the list\n");
        return 1;
    }
    int sel = 0;
    for (;;) {
        s_count = 0;
        scan(dir);
        char games[AG_PATH_MAX];
        join(games, sizeof(games), dir, "GAMES");
        scan(games);
        if (s_count == 0) {
            ag_printf("mdmenu: no *.SWP cartridges in %s or %s\n", dir, games);
            break;
        }
        if (sel >= s_count) {
            sel = s_count - 1;
        }
        ag_coninfo(&s_ci);
        ag_cursor(false);
        ag_cls();
        draw(sel);
        unsigned held = settle();
        bool quit = false;
        for (;;) {
            ag_event_t ev;
            while (ag_poll_event(&ev, 0)) {
            }
            const unsigned now = buttons();
            const unsigned press = now & ~held;
            held = now;
            if (press & B_UP) {
                sel = (sel + s_count - 1) % s_count;
                draw(sel);
            } else if (press & B_DOWN) {
                sel = (sel + 1) % s_count;
                draw(sel);
            } else if (press & B_PLAY) {
                break;
            } else if (press & B_QUIT) {
                quit = true;
                break;
            }
            ag_delay(20);
        }
        if (quit) {
            break;
        }
        play(&s_games[sel]);
    }
    ag_color(AG_WHITE, AG_BLACK);
    ag_cls();
    ag_cursor(true);
    ag_free(s_games);
    return 0;
}
