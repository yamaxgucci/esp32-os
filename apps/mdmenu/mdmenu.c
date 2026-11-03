/*
 * ArgonOS - MDMENU: the Mega Drive's front end.
 *
 *   run c:\mdmenu.axe
 *
 * A main screen - LOAD GAME, CONTINUE (the last game), EXIT - and behind LOAD
 * GAME a window that walks the drives and folders, showing the folders and
 * the cartridges in them.  A cartridge is a .SWP (byte-swapped, the way MD
 * reads it out of flash in place) or a plain .BIN / .MD / .GEN, which is made
 * into a .SWP beside itself the first time it is chosen.  The game runs with
 * the Mega Drive image next to this program (MDS3.AXE, MD-NO-PSRAM.AXE or
 * MD.AXE); QUIT TO LIST in its pause menu brings this screen back.  Drawn as
 * text on the console, so all the memory is the game's while one runs.
 *
 * The pad: up/down to move, A, C or START to choose, B for the folder above,
 * MENU (Esc) to go back.  The phone's pad (PHONE.SYS, #pad), a keyboard's
 * arrows/Enter/Esc or a real pad - the pad layer reads them all.
 *
 * Where it was and what it played last are kept in C:\MDMENU.CFG.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>
#include <argon/libc.h>

#include <stdio.h>
#include <string.h>

AG_APP_SIZED("MDMENU", "2.0", "argon", 0, 4 * 1024, 24 * 1024);

#define CFG_PATH   "C:\\MDMENU.CFG"
#define MAX_ITEMS  128
#define ITEM_LEN   48
#define PATH_LEN   160

typedef struct {
    char name[ITEM_LEN];
    bool dir;
} item_t;

static item_t      *s_items;
static int          s_count;
static char         s_md[PATH_LEN];
static char         s_dir[PATH_LEN];   /* "" is the list of drives */
static char         s_last[PATH_LEN];  /* the last game played */
static ag_coninfo_t s_ci;

/* ---- small helpers ---------------------------------------------------- */

static char up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static bool ends_with(const char *s, const char *ext)
{
    const size_t n = strlen(s), e = strlen(ext);
    if (n < e) {
        return false;
    }
    for (size_t i = 0; i < e; i++) {
        if (up(s[n - e + i]) != ext[i]) {
            return false;
        }
    }
    return true;
}

/* names compared as a person reads them: case aside */
static int name_cmp(const char *a, const char *b)
{
    while (*a != 0 && up(*a) == up(*b)) {
        a++;
        b++;
    }
    return (int)(unsigned char)up(*a) - (int)(unsigned char)up(*b);
}

static bool is_raw_rom(const char *name)
{
    return ends_with(name, ".BIN") || ends_with(name, ".MD") || ends_with(name, ".GEN");
}

static bool is_rom(const char *name)
{
    return ends_with(name, ".SWP") || is_raw_rom(name);
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

static void join(char *out, size_t cap, const char *dir, const char *name)
{
    const size_t d = strlen(dir);
    const bool sep = d > 0 && dir[d - 1] != '\\' && dir[d - 1] != '/';
    (void)snprintf(out, cap, "%s%s%s", dir, sep ? "\\" : "", name);
}

/* the folder above `dir`, or "" (the drives) above a drive's root */
static void parent(char *dir)
{
    size_t n = strlen(dir);
    if (n <= 3) {           /* "A:\" */
        dir[0] = '\0';
        return;
    }
    if (dir[n - 1] == '\\') {
        dir[--n] = '\0';
    }
    while (n > 0 && dir[n - 1] != '\\') {
        n--;
    }
    dir[n] = '\0';
    if (n == 2) {           /* "A:" -> "A:\" */
        dir[2] = '\\';
        dir[3] = '\0';
    }
}

/* ---- what is kept between runs --------------------------------------- */

static void cfg_load(void)
{
    s_dir[0] = '\0';
    s_last[0] = '\0';
    const ag_handle_t h = ag_open(CFG_PATH, AG_O_RDONLY);
    if (h < 0) {
        return;
    }
    char buf[2 * PATH_LEN + 4];
    const int32_t n = ag_read(h, buf, sizeof(buf) - 1);
    (void)ag_close(h);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl != NULL) {
        *nl = '\0';
        char *rest = nl + 1;
        char *nl2 = strchr(rest, '\n');
        if (nl2 != NULL) {
            *nl2 = '\0';
        }
        (void)snprintf(s_last, sizeof(s_last), "%s", rest);
    }
    (void)snprintf(s_dir, sizeof(s_dir), "%s", buf);
}

static void cfg_save(void)
{
    const ag_handle_t h = ag_open(CFG_PATH, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (h < 0) {
        return;
    }
    char buf[2 * PATH_LEN + 4];
    const int n = snprintf(buf, sizeof(buf), "%s\n%s\n", s_dir, s_last);
    (void)ag_write(h, buf, (size_t)n);
    (void)ag_close(h);
}

/* ---- the screen ------------------------------------------------------- */

static void text_at(int x, int y, int w, const char *s, uint8_t attr)
{
    if (y < 0 || y >= (int)s_ci.rows) {
        return;
    }
    ag_fill((uint16_t)x, (uint16_t)y, (uint16_t)w, 1, ' ', attr);
    for (int i = 0; s[i] != '\0' && i < w; i++) {
        ag_poke((uint16_t)(x + i), (uint16_t)y, s[i], attr);
    }
}

static void header(const char *title, const char *hint)
{
    const int cols = (int)s_ci.cols;
    text_at(0, 0, cols, title, AG_ATTR(AG_YELLOW, AG_BLUE));
    text_at(0, (int)s_ci.rows - 1, cols, hint, AG_ATTR(AG_LGRAY, AG_BLUE));
}

/* `n` lines of a list, `sel` lit, scrolled by *top, between rows y0 and y1 */
static void list_draw(int y0, int y1, int n, int sel, int *top,
                      void (*line)(int i, char *out, size_t cap, bool *dim))
{
    const int rows = y1 - y0;
    if (sel < *top) {
        *top = sel;
    } else if (sel >= *top + rows) {
        *top = sel - rows + 1;
    }
    char buf[ITEM_LEN + 16];
    for (int r = 0; r < rows; r++) {
        const int i = *top + r;
        if (i >= n) {
            text_at(0, y0 + r, (int)s_ci.cols, "", AG_ATTR(AG_WHITE, AG_BLACK));
            continue;
        }
        bool dim = false;
        line(i, buf, sizeof(buf), &dim);
        const uint8_t a = (i == sel) ? AG_ATTR(AG_BLACK, AG_CYAN)
                          : dim      ? AG_ATTR(AG_YELLOW, AG_BLACK)
                                     : AG_ATTR(AG_WHITE, AG_BLACK);
        text_at(0, y0 + r, (int)s_ci.cols, buf, a);
    }
}

/* ---- the pad ---------------------------------------------------------- */

enum { B_UP = 1, B_DOWN = 2, B_OK = 4, B_BACK = 8, B_MENU = 16 };

static unsigned buttons(void)
{
    unsigned m = 0;
    m |= ag_btnp(0, AG_BTN_UP) ? B_UP : 0u;
    m |= ag_btnp(0, AG_BTN_DOWN) ? B_DOWN : 0u;
    m |= (ag_btnp(0, AG_BTN_B1) || ag_btnp(0, AG_BTN_C) || ag_btnp(0, AG_BTN_START))
             ? B_OK : 0u;
    m |= ag_btnp(0, AG_BTN_B2) ? B_BACK : 0u;
    m |= ag_btnp(0, AG_BTN_QUIT) ? B_MENU : 0u;
    return m;
}

static unsigned s_held;

/* the next press, with up/down repeating while held */
static unsigned next_press(void)
{
    uint32_t held_since = 0;
    for (;;) {
        ag_event_t ev;
        while (ag_poll_event(&ev, 0)) {
        }
        const unsigned now = buttons();
        const unsigned press = now & ~s_held;
        const uint32_t t = ag_millis();
        if (press != 0u) {
            s_held = now;
            held_since = t;
            return press;
        }
        if ((now & (B_UP | B_DOWN)) && held_since != 0u && t - held_since > 350u) {
            held_since = t - 250u;      /* then every 100 ms */
            return now & (B_UP | B_DOWN);
        }
        if (now == 0u) {
            held_since = 0;
        } else if (held_since == 0u) {
            held_since = t;
        }
        s_held = now;
        ag_delay(20);
    }
}

static void settle(void)
{
    for (int i = 0; i < 100 && buttons() != 0u; i++) {
        ag_delay(20);
    }
    s_held = buttons();
}

/* ---- the file window -------------------------------------------------- */

static void scan(void)
{
    s_count = 0;
    if (s_dir[0] == '\0') {
        /* the drives that answer: not D: (devices) nor T: (memory) */
        for (char d = 'A'; d <= 'Z' && s_count < MAX_ITEMS; d++) {
            if (d == 'D' || d == 'T') {
                continue;
            }
            char root[4] = {d, ':', '\\', 0};
            const ag_handle_t h = ag_opendir(root);
            if (h < 0) {
                continue;
            }
            (void)ag_closedir(h);
            (void)snprintf(s_items[s_count].name, ITEM_LEN, "%s", root);
            s_items[s_count].dir = true;
            s_count++;
        }
        return;
    }
    (void)snprintf(s_items[0].name, ITEM_LEN, "..");
    s_items[0].dir = true;
    s_count = 1;
    const ag_handle_t h = ag_opendir(s_dir);
    if (h < 0) {
        return;
    }
    ag_dirent_t e;
    while (s_count < MAX_ITEMS && ag_readdir(h, &e) == AG_OK) {
        const bool dir = (e.st.attr & AG_A_DIR) != 0;
        if (e.name[0] == '.' || (!dir && !is_rom(e.name)) ||
            strcmp(e.name, "System Volume Information") == 0) {
            continue;
        }
        (void)snprintf(s_items[s_count].name, ITEM_LEN, "%s", e.name);
        s_items[s_count].dir = dir;
        s_count++;
    }
    (void)ag_closedir(h);
    /* folders first, then by name */
    for (int i = 2; i < s_count; i++) {
        item_t t = s_items[i];
        int j = i;
        while (j > 1 && ((t.dir && !s_items[j - 1].dir) ||
                         (t.dir == s_items[j - 1].dir &&
                          name_cmp(t.name, s_items[j - 1].name) < 0))) {
            s_items[j] = s_items[j - 1];
            j--;
        }
        s_items[j] = t;
    }
}

static void file_line(int i, char *out, size_t cap, bool *dim)
{
    const item_t *it = &s_items[i];
    *dim = it->dir;
    if (it->dir) {
        (void)snprintf(out, cap, " [%s]", it->name);
    } else {
        (void)snprintf(out, cap, " %s", it->name);
    }
}

/*
 * A plain cartridge made into the .SWP beside it: MD reads the cartridge out
 * of flash in place, a word at a time, as the 68000 sees it.  Into `out`.
 */
static bool make_swp(const char *src, char *out, size_t cap)
{
    /*
     * Which way round it is, by the header's "SEGA" at 0x100: a dump as it
     * comes is "SEGA" and is swapped; one already swapped reads "ESAG" and
     * is played as it is - swapped twice it was a game named UGSNAT REHORSE
     * that did not start (3 Nov 2026).  Anything else (an interleaved .SMD)
     * is not a cartridge this can read.
     */
    {
        const ag_handle_t h = ag_open(src, AG_O_RDONLY);
        if (h < 0) {
            return false;
        }
        uint8_t hdr[4] = {0};
        const bool got = ag_seek(h, 0x100, AG_SEEK_SET) >= 0 &&
                         ag_read(h, hdr, 4) == 4;
        (void)ag_close(h);
        if (!got) {
            return false;
        }
        if (memcmp(hdr, "ESAG", 4) == 0) {
            (void)snprintf(out, cap, "%s", src);
            return true;
        }
        if (memcmp(hdr, "SEGA", 4) != 0) {
            return false;
        }
    }
    (void)snprintf(out, cap, "%s", src);
    char *dot = strrchr(out, '.');
    if (dot == NULL || (size_t)(dot - out) + 5u > cap) {
        return false;
    }
    memcpy(dot, ".SWP", 5);
    if (exists(out)) {
        return true;
    }
    const ag_handle_t in = ag_open(src, AG_O_RDONLY);
    if (in < 0) {
        return false;
    }
    const ag_handle_t o = ag_open(out, AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
    if (o < 0) {
        (void)ag_close(in);
        return false;
    }
    uint8_t *buf = (uint8_t *)ag_malloc(4096);
    bool ok = (buf != NULL);
    uint32_t done = 0;
    while (ok) {
        const int32_t n = ag_read(in, buf, 4096);
        if (n <= 0) {
            ok = (n == 0);
            break;
        }
        for (int32_t k = 0; k + 1 < n; k += 2) {
            const uint8_t t = buf[k];
            buf[k] = buf[k + 1];
            buf[k + 1] = t;
        }
        ok = ag_write(o, buf, (size_t)n) == n;
        done += (uint32_t)n;
        if ((done & 0xffffu) == 0u) {
            char msg[40];
            (void)snprintf(msg, sizeof(msg), " preparing... %u KB", (unsigned)(done / 1024u));
            text_at(0, (int)s_ci.rows - 2, (int)s_ci.cols, msg, AG_ATTR(AG_YELLOW, AG_BLACK));
        }
    }
    ag_free(buf);
    (void)ag_close(in);
    (void)ag_close(o);
    if (!ok) {
        (void)ag_unlink(out);
    }
    return ok;
}

static void play(const char *rom)
{
    (void)snprintf(s_last, sizeof(s_last), "%s", rom);
    cfg_save();
    ag_color(AG_WHITE, AG_BLACK);
    ag_cls();
    ag_printf("starting %s\n", rom);
    const char *argv[2] = {s_md, rom};
    const int32_t rc = ag_exec(s_md, 2, argv);
    if (rc < 0) {
        ag_printf("%s: %s - any button\n", s_md, ag_strerror((ag_err_t)rc));
        settle();
        (void)next_press();
    }
    ag_cursor(false);
    settle();
}

/* the window; true when a game was played (the main screen then redraws) */
static void browse(void)
{
    int sel = 0, top = 0;
    char title[96];
    for (;;) {
        scan();
        if (sel >= s_count) {
            sel = s_count - 1;
        }
        if (sel < 0) {
            sel = 0;
        }
        ag_cls();
        (void)snprintf(title, sizeof(title), " LOAD GAME  %s",
                       s_dir[0] ? s_dir : "(drives)");
        header(title, " A/START: OPEN  B: UP  MENU: BACK");
        for (;;) {
            list_draw(2, (int)s_ci.rows - 2, s_count, sel, &top, file_line);
            const unsigned p = next_press();
            if (p & B_UP) {
                sel = (sel + s_count - 1) % (s_count ? s_count : 1);
            } else if (p & B_DOWN) {
                sel = (sel + 1) % (s_count ? s_count : 1);
            } else if (p & B_MENU) {
                cfg_save();
                return;
            } else if ((p & B_BACK) || ((p & B_OK) && s_count > 0 &&
                                         strcmp(s_items[sel].name, "..") == 0)) {
                if (s_dir[0] == '\0') {
                    return;
                }
                parent(s_dir);
                sel = top = 0;
                break;
            } else if ((p & B_OK) && s_count > 0) {
                const item_t *it = &s_items[sel];
                if (it->dir) {
                    if (s_dir[0] == '\0') {
                        (void)snprintf(s_dir, sizeof(s_dir), "%s", it->name);
                    } else {
                        char next[PATH_LEN];
                        join(next, sizeof(next), s_dir, it->name);
                        (void)snprintf(s_dir, sizeof(s_dir), "%s", next);
                    }
                    sel = top = 0;
                    break;
                }
                char rom[PATH_LEN];
                join(rom, sizeof(rom), s_dir, it->name);
                if (is_raw_rom(it->name)) {
                    char swp[PATH_LEN];
                    if (!make_swp(rom, swp, sizeof(swp))) {
                        text_at(0, (int)s_ci.rows - 2, (int)s_ci.cols,
                                " not a cartridge this reads, or the card is full",
                                AG_ATTR(AG_LRED, AG_BLACK));
                        continue;
                    }
                    (void)snprintf(rom, sizeof(rom), "%s", swp);
                }
                play(rom);
                return;
            }
        }
    }
}

/* ---- the main screen -------------------------------------------------- */

static int s_main_n;
static const char *s_main[3];

static void main_line(int i, char *out, size_t cap, bool *dim)
{
    *dim = false;
    (void)snprintf(out, cap, "   %s", s_main[i]);
}

int ag_main(int argc, char **argv)
{
    char here[PATH_LEN];
    (void)snprintf(here, sizeof(here), "%s", (argc > 0) ? argv[0] : "C:\\");
    /* argv[0] comes either way round: C:\X.AXE or /sys/x.axe */
    char *cut = strrchr(here, '\\');
    char *cut2 = strrchr(here, '/');
    if (cut2 > cut) {
        cut = cut2;
    }
    if (cut != NULL) {
        cut[1] = '\0';
    }
    static const char *const k_md[] = {"MDS3.AXE", "MD-NO-PSRAM.AXE", "MD.AXE"};
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
    s_items = (item_t *)ag_malloc(sizeof(item_t) * MAX_ITEMS);
    if (s_items == NULL) {
        ag_printf("mdmenu: no memory for the list\n");
        return 1;
    }
    cfg_load();
    ag_coninfo(&s_ci);
    ag_cursor(false);
    settle();

    int sel = 0, top = 0;
    char cont[ITEM_LEN + 16];
    for (;;) {
        s_main_n = 0;
        s_main[s_main_n++] = "LOAD GAME";
        const bool can_continue = s_last[0] != '\0' && exists(s_last);
        if (can_continue) {
            const char *name = strrchr(s_last, '\\');
            (void)snprintf(cont, sizeof(cont), "CONTINUE: %s", name ? name + 1 : s_last);
            s_main[s_main_n++] = cont;
        }
        s_main[s_main_n++] = "EXIT";
        if (sel >= s_main_n) {
            sel = 0;
        }
        ag_cls();
        header("  MEGA DRIVE", " UP/DOWN  A/START: CHOOSE");
        list_draw(3, 3 + s_main_n, s_main_n, sel, &top, main_line);
        const unsigned p = next_press();
        if (p & B_UP) {
            sel = (sel + s_main_n - 1) % s_main_n;
        } else if (p & B_DOWN) {
            sel = (sel + 1) % s_main_n;
        } else if (p & B_OK) {
            const char *what = s_main[sel];
            if (strcmp(what, "EXIT") == 0) {
                break;
            }
            if (what == cont) {
                play(s_last);
            } else {
                browse();
            }
        }
    }
    ag_color(AG_WHITE, AG_BLACK);
    ag_cls();
    ag_cursor(true);
    ag_free(s_items);
    return 0;
}
