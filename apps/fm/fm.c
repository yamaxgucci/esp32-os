/*
 * ArgonOS file manager - panels, drawing and keys.
 *
 * Two panels, a cursor bar, and the ten function keys everyone already knows.
 * It exists partly because a machine you can only talk to through a command line
 * is a machine you argue with, and partly because it is the first application
 * that uses the system the way a real one does: it draws its own screen, reads
 * whole key events, walks directories, copies files, and starts other programs.
 *
 *   run t:\fm.axe          starts in the current directory
 *   run t:\fm.axe c:\      starts somewhere else
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "fm.h"
#include "fm_ui.h"

#ifdef AG_BUILTIN
#include <argon/shell.h>
#endif

/* Only the plain text application build declares an image header; the built-in
 * lives in the kernel image, and GFXFM supplies its own header. */
#if !defined(AG_BUILTIN) && !defined(FM_GFX_BUILD)
AG_APP("FM", "1.0", "argon", 0);
#endif

fm_panel_t g_panel[2];
int        g_active;

/* ---------------------------------------------------------------------- */
/* Drawing                                                               */
/* ---------------------------------------------------------------------- */

void fm_put(int x, int y, const char *s, uint8_t attr)
{
    if (s == NULL) {
        return;
    }
    for (int i = 0; s[i] != '\0' && x + i < FM_COLS; i++) {
        fm_ui_poke(x + i, y, s[i], attr);
    }
}

/*
 * Text in a fixed field: padded to the width so what was there before is gone,
 * and cut short with no complaint when it does not fit.  Every panel line is
 * drawn this way, which is why redrawing never leaves a tail of the last name.
 */
void fm_put_clipped(int x, int y, int width, const char *s, uint8_t attr)
{
    int i = 0;

    for (; s != NULL && s[i] != '\0' && i < width; i++) {
        fm_ui_poke(x + i, y, s[i], attr);
    }
    for (; i < width; i++) {
        fm_ui_poke(x + i, y, ' ', attr);
    }
}

void fm_clear_row(int y, uint8_t attr)
{
    fm_ui_fill(0, y, FM_COLS, 1, ' ', attr);
}

void fm_frame(int x, int y, int w, int h, uint8_t attr)
{
    fm_ui_frame(x, y, w, h, attr);
}

void fm_message(const char *text)
{
    fm_clear_row(FM_ROW_MESSAGE, FM_ATTR_STATUS);
    fm_put(1, FM_ROW_MESSAGE, text, FM_ATTR_MESSAGE);
}

void fm_error(const char *what, ag_err_t err)
{
    char line[FM_COLS];

    ag_strlcpy(line, (what != NULL) ? what : "failed", sizeof(line));
    ag_strlcat(line, ": ", sizeof(line));
    ag_strlcat(line, ag_strerror(err), sizeof(line));

    fm_clear_row(FM_ROW_MESSAGE, FM_ATTR_STATUS);
    fm_put(1, FM_ROW_MESSAGE, line, FM_ATTR_ERROR);
}

/* ---------------------------------------------------------------------- */
/* Paths                                                                 */
/* ---------------------------------------------------------------------- */

void fm_join(const char *dir, const char *name, char *out, size_t len)
{
    ag_strlcpy(out, dir, len);

    const size_t at = strlen(out);
    if (at == 0 || out[at - 1] != '/') {
        ag_strlcat(out, "/", len);
    }
    ag_strlcat(out, name, len);
}

/*
 * The drive letters, so a panel header reads the way the shell's prompt does.
 * The table mirrors the kernel's (see ag_path_drive) and is duplicated here
 * knowingly: it is presentation only, and asking the system for it needs the cfg
 * subtable, which does not exist yet.
 */
static const struct {
    const char *mount;
    char        letter;
} k_drives[] = {
    {"/sd", 'A'},
    {"/sys", 'C'},
    {"/tmp", 'T'},
    {"/dev", 'D'},
};

void fm_dos_path(const char *posix, char *out, size_t len)
{
    for (size_t i = 0; i < sizeof(k_drives) / sizeof(k_drives[0]); i++) {
        const char  *mount = k_drives[i].mount;
        const size_t n = strlen(mount);

        if (ag_stricmp(posix, mount) != 0 &&
            !(memcmp(posix, mount, n) == 0 && posix[n] == '/')) {
            continue;
        }

        char head[4] = {k_drives[i].letter, ':', '\\', '\0'};
        ag_strlcpy(out, head, len);
        ag_strlcat(out, &posix[(posix[n] == '/') ? n + 1 : n], len);

        for (size_t j = 0; out[j] != '\0'; j++) {
            if (out[j] == '/') {
                out[j] = '\\';
            }
        }
        return;
    }

    /* Something not under a known mount: shown as it is, which is honest. */
    ag_strlcpy(out, posix, len);
}

/* The directory above, or false at the root of a drive. */
static bool parent_of(const char *path, char *out, size_t len)
{
    ag_strlcpy(out, path, len);

    size_t at = strlen(out);
    while (at > 0 && out[at - 1] == '/') {
        at--;
    }
    while (at > 0 && out[at - 1] != '/') {
        at--;
    }
    if (at == 0) {
        return false;
    }
    /* Keep the leading slash of "/tmp" -> "/". */
    if (at > 1) {
        at--;
    }
    out[at] = '\0';
    return strlen(out) > 0 && ag_strcmp(out, path) != 0;
}

/* ---------------------------------------------------------------------- */
/* Reading a directory                                                    */
/* ---------------------------------------------------------------------- */

fm_panel_t *fm_active(void) { return &g_panel[g_active]; }
fm_panel_t *fm_other(void) { return &g_panel[g_active ^ 1]; }

fm_entry_t *fm_current(void)
{
    fm_panel_t *p = fm_active();

    if (p->count == 0 || p->cursor < 0 || p->cursor >= p->count) {
        return NULL;
    }
    return &p->entries[p->cursor];
}

/*
 * Directories first, then names, both without regard to case - the order anybody
 * looking for a file expects, and the order FAT does not give.
 */
static bool sorts_before(const fm_entry_t *a, const fm_entry_t *b)
{
    if (a->is_up != b->is_up) {
        return a->is_up;
    }
    if (a->is_dir != b->is_dir) {
        return a->is_dir;
    }
    return ag_stricmp(a->name, b->name) < 0;
}

static void sort_entries(fm_entry_t *list, int count)
{
    /* Insertion sort: a few hundred entries, and it is stable and in place. */
    for (int i = 1; i < count; i++) {
        const fm_entry_t key = list[i];
        int              j = i - 1;

        while (j >= 0 && sorts_before(&key, &list[j])) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = key;
    }
}

ag_err_t fm_read_panel(fm_panel_t *p, const char *path)
{
    char resolved[AG_PATH_MAX];

    /*
     * Through chdir so the kernel does the resolving: it understands drive
     * letters, backslashes and "..", and this application should not have to.
     */
    ag_err_t err = ag_chdir(path);
    if (err != AG_OK) {
        return err;
    }
    err = ag_getcwd(resolved, sizeof(resolved));
    if (err != AG_OK) {
        return err;
    }

    const ag_handle_t dir = ag_opendir(resolved);
    if (dir < 0) {
        return dir;
    }

    p->count = 0;
    p->truncated = 0;
    p->total_bytes = 0;
    ag_strlcpy(p->path, resolved, sizeof(p->path));

    /* ".." first, unless this is the root of a drive. */
    char up[AG_PATH_MAX];
    if (parent_of(resolved, up, sizeof(up))) {
        fm_entry_t *e = &p->entries[p->count++];
        memset(e, 0, sizeof(*e));
        ag_strlcpy(e->name, "..", sizeof(e->name));
        e->is_dir = true;
        e->is_up = true;
    }

    ag_dirent_t de;
    while (ag_readdir(dir, &de) == AG_OK) {
        if (p->count >= FM_MAX_ENTRIES) {
            p->truncated++;
            continue;
        }
        fm_entry_t *e = &p->entries[p->count++];
        memset(e, 0, sizeof(*e));
        ag_strlcpy(e->name, de.name, sizeof(e->name));
        e->size = de.st.size;
        e->mtime = de.st.mtime;
        e->is_dir = (de.st.attr & AG_A_DIR) != 0;
        if (!e->is_dir) {
            p->total_bytes += de.st.size;
        }
    }
    (void)ag_closedir(dir);

    sort_entries(p->entries, p->count);

    if (p->cursor >= p->count) {
        p->cursor = (p->count > 0) ? p->count - 1 : 0;
    }
    if (p->cursor < 0) {
        p->cursor = 0;
    }
    p->top = 0;
    return AG_OK;
}

ag_err_t fm_reload(fm_panel_t *p)
{
    char keep[FM_NAME_MAX] = "";
    if (p->count > 0 && p->cursor < p->count) {
        ag_strlcpy(keep, p->entries[p->cursor].name, sizeof(keep));
    }

    const ag_err_t err = fm_read_panel(p, p->path);
    if (err != AG_OK) {
        return err;
    }

    /* Back onto the same name if it is still there: reloading should not move
     * the cursor to somewhere the user was not looking. */
    for (int i = 0; i < p->count; i++) {
        if (ag_stricmp(p->entries[i].name, keep) == 0) {
            p->cursor = i;
            break;
        }
    }
    return AG_OK;
}

/* ---------------------------------------------------------------------- */
/* Drawing the panels                                                     */
/* ---------------------------------------------------------------------- */

static void scroll_into_view(fm_panel_t *p)
{
    if (p->cursor < p->top) {
        p->top = p->cursor;
    }
    if (p->cursor >= p->top + FM_VISIBLE) {
        p->top = p->cursor - FM_VISIBLE + 1;
    }
    if (p->top < 0) {
        p->top = 0;
    }
}

/* "1 234 567" for a file, "<DIR>" for a directory, right-aligned in 10. */
static void size_text(const fm_entry_t *e, char *out, size_t len)
{
    if (e->is_dir) {
        ag_strlcpy(out, e->is_up ? "    <UP>" : "   <DIR>", len);
        return;
    }
    char buf[24];
    ag_strlcpy(out, ag_utoa(e->size, buf, sizeof(buf), 8, true), len);
}

void fm_draw_panel(int which)
{
    fm_panel_t *p = &g_panel[which];
    const int   x = which * FM_PANEL_W;
    const bool  active = (which == g_active);

    scroll_into_view(p);

    fm_frame(x, 0, FM_PANEL_W, FM_PANEL_ROWS, FM_ATTR_FRAME);

    /* The path in the top border, tail first when it does not fit: the end of a
     * path says where you are, the start says which drive. */
    char shown[AG_PATH_MAX];
    fm_dos_path(p->path, shown, sizeof(shown));

    const int room = FM_PANEL_W - 4;
    const int len = (int)strlen(shown);
    char      header[FM_PANEL_W];
    ag_strlcpy(header, " ", sizeof(header));
    if (len > room) {
        ag_strlcat(header, "...", sizeof(header));
        ag_strlcat(header, &shown[len - (room - 3)], sizeof(header));
    } else {
        ag_strlcat(header, shown, sizeof(header));
    }
    ag_strlcat(header, " ", sizeof(header));
    fm_put(x + 1, 0, header, active ? FM_ATTR_CURSOR : FM_ATTR_FRAME);

    for (int row = 0; row < FM_VISIBLE; row++) {
        const int index = p->top + row;
        const int y = row + 1;

        if (index >= p->count) {
            fm_put_clipped(x + 1, y, FM_PANEL_W - 2, "", FM_ATTR_PANEL);
            continue;
        }

        const fm_entry_t *e = &p->entries[index];
        const bool        on = active && (index == p->cursor);

        uint8_t attr = FM_ATTR_PANEL;
        if (e->is_dir) {
            attr = FM_ATTR_DIR;
        } else if (ag_ends_with_i(e->name, ".axe")) {
            attr = FM_ATTR_EXEC;
        }
        if (on) {
            attr = FM_ATTR_CURSOR;
        }

        /* name field, then the size field right up against the border */
        char line[FM_PANEL_W];
        char size[16];
        size_text(e, size, sizeof(size));

        const int name_w = FM_PANEL_W - 2 - 1 - (int)strlen(size);
        ag_strlcpy(line, e->is_dir && !e->is_up ? "[" : " ", sizeof(line));
        ag_strlcat(line, e->name, sizeof(line));
        if (e->is_dir && !e->is_up) {
            ag_strlcat(line, "]", sizeof(line));
        }

        fm_put_clipped(x + 1, y, name_w, line, attr);
        fm_put_clipped(x + 1 + name_w, y, (int)strlen(size) + 1, size, attr);
    }

    /* Totals in the bottom border. */
    char totals[FM_PANEL_W];
    char count[24];
    char bytes[24];
    ag_strlcpy(totals, " ", sizeof(totals));
    ag_strlcat(totals, ag_utoa((uint64_t)p->count, count, sizeof(count), 0,
                               false),
               sizeof(totals));
    ag_strlcat(totals, " items, ", sizeof(totals));
    ag_strlcat(totals, ag_utoa(p->total_bytes, bytes, sizeof(bytes), 0, true),
               sizeof(totals));
    ag_strlcat(totals, " bytes ", sizeof(totals));
    fm_put(x + 2, FM_PANEL_ROWS - 1, totals, FM_ATTR_FRAME);
}

void fm_draw_detail(void)
{
    const fm_entry_t *e = fm_current();

    fm_clear_row(FM_ROW_DETAIL, FM_ATTR_STATUS);
    if (e == NULL) {
        return;
    }

    char line[FM_COLS];
    char full[AG_PATH_MAX];
    char shown[AG_PATH_MAX];

    fm_join(fm_active()->path, e->name, full, sizeof(full));
    fm_dos_path(full, shown, sizeof(shown));
    ag_strlcpy(line, shown, sizeof(line));

    if (!e->is_dir) {
        char bytes[24];
        ag_strlcat(line, "   ", sizeof(line));
        ag_strlcat(line, ag_utoa(e->size, bytes, sizeof(bytes), 0, true),
                   sizeof(line));
        ag_strlcat(line, " bytes", sizeof(line));
    }
    fm_put_clipped(0, FM_ROW_DETAIL, FM_COLS, line, FM_ATTR_STATUS);
}

static void draw_keys(void)
{
    static const char *k_labels[10] = {
        "1Help", "2Reld", "3View", "4    ", "5Copy",
        "6Move", "7MkDr", "8Del ", "9    ", "10Quit",
    };

    fm_clear_row(FM_ROW_KEYS, FM_ATTR_KEYS);
    for (int i = 0; i < 10; i++) {
        fm_put(i * 8, FM_ROW_KEYS, k_labels[i], FM_ATTR_KEYS);
    }
}

/*
 * The last redraw's cost, on screen.  Not decoration: over a serial line the
 * screen is 2 KB of traffic, and this is the number that says whether the thing
 * feels alive.  It is small because unchanged cells are not sent - the screen
 * tracks which rows actually changed - so repainting everything after a keypress
 * costs only what moved.
 */
static uint32_t s_last_redraw_us;

static void draw_status(void)
{
    char line[FM_COLS];
    char number[24];

    ag_strlcpy(line, "ArgonOS file manager   arena ", sizeof(line));

    ag_meminfo_t mem;
    ag_meminfo(&mem);
    ag_strlcat(line, ag_utoa(mem.arena_free / 1024u, number, sizeof(number), 0,
                             false),
               sizeof(line));
    ag_strlcat(line, " KB free   redraw ", sizeof(line));
    ag_strlcat(line,
               ag_utoa(s_last_redraw_us, number, sizeof(number), 0, false),
               sizeof(line));
    ag_strlcat(line, " us   Tab, Enter, F10", sizeof(line));

    fm_clear_row(FM_ROW_STATUS, FM_ATTR_STATUS);
    fm_put_clipped(0, FM_ROW_STATUS, FM_COLS, line, FM_ATTR_STATUS);
}

void fm_draw_all(void)
{
    const ag_time_t t0 = ag_micros();

    fm_draw_panel(0);
    fm_draw_panel(1);
    fm_draw_detail();
    draw_keys();

    /* Measured before the status line is drawn, since it shows the measurement. */
    s_last_redraw_us = (uint32_t)(ag_micros() - t0);
    draw_status();
    fm_ui_present();
}

/*
 * What moving the cursor changes, and nothing else.  Cells that keep their value
 * are not sent to the terminal, but they are still 2000 syscalls, each of which
 * queues behind the renderer while it transmits - so not touching them is worth
 * about three times the speed on a keypress.
 */
void fm_draw_light(void)
{
    const ag_time_t t0 = ag_micros();

    fm_draw_panel(g_active);
    fm_draw_detail();

    s_last_redraw_us = (uint32_t)(ag_micros() - t0);
    draw_status();
    fm_ui_present();
}

/* ---------------------------------------------------------------------- */
/* Keys                                                                   */
/* ---------------------------------------------------------------------- */

static void move_cursor(int delta)
{
    fm_panel_t *p = fm_active();

    if (p->count == 0) {
        return;
    }
    p->cursor += delta;
    if (p->cursor < 0) {
        p->cursor = 0;
    }
    if (p->cursor >= p->count) {
        p->cursor = p->count - 1;
    }
}

static void enter_current(void)
{
    fm_entry_t *e = fm_current();
    fm_panel_t *p = fm_active();

    if (e == NULL) {
        return;
    }

    if (e->is_dir) {
        char target[AG_PATH_MAX];

        if (e->is_up) {
            /* Remember where we came from, so the cursor lands on it. */
            char leaving[FM_NAME_MAX];
            const char *base = p->path;
            for (const char *s = p->path; *s != '\0'; s++) {
                if (*s == '/' && s[1] != '\0') {
                    base = s + 1;
                }
            }
            ag_strlcpy(leaving, base, sizeof(leaving));
            fm_join(p->path, "..", target, sizeof(target));

            if (fm_read_panel(p, target) == AG_OK) {
                for (int i = 0; i < p->count; i++) {
                    if (ag_stricmp(p->entries[i].name, leaving) == 0) {
                        p->cursor = i;
                        break;
                    }
                }
            }
            return;
        }

        fm_join(p->path, e->name, target, sizeof(target));
        const ag_err_t err = fm_read_panel(p, target);
        if (err != AG_OK) {
            fm_error(e->name, err);
        } else {
            p->cursor = 0;
        }
        return;
    }

    if (ag_ends_with_i(e->name, ".axe")) {
        fm_run(e);
        return;
    }

#ifdef AG_BUILTIN
    {
        char full[AG_PATH_MAX];
        fm_join(p->path, e->name, full, sizeof(full));
        fm_ui_end();
        if (ag_shell_open_associated(full)) {
            fm_pause("press any key");
            fm_ui_begin();
            (void)fm_reload(fm_active());
            (void)fm_reload(fm_other());
            return;
        }
        fm_ui_begin();
    }
#endif

    /* Anything else: show it, which is what F3 does. */
    fm_view();
}

/* Alt+F1 and Alt+F2 in the manager everyone remembers: pick a drive. */
static void choose_drive(int which)
{
    static const char *k_roots[] = {"/sd", "/sys", "/tmp"};
    char               prompt[64];
    char               answer[8] = "";

    ag_strlcpy(prompt, "Drive for the ", sizeof(prompt));
    ag_strlcat(prompt, (which == 0) ? "left" : "right", sizeof(prompt));
    ag_strlcat(prompt, " panel (A, C, T):", sizeof(prompt));

    if (!fm_ask(prompt, answer, sizeof(answer)) || answer[0] == '\0') {
        return;
    }

    const char letter = ag_upper(answer[0]);
    const char *root = NULL;
    if (letter == 'A') {
        root = k_roots[0];
    } else if (letter == 'C') {
        root = k_roots[1];
    } else if (letter == 'T') {
        root = k_roots[2];
    }
    if (root == NULL) {
        fm_message("No such drive. A is the card, C the flash, T the RAM disk.");
        return;
    }

    fm_panel_t *p = &g_panel[which];
    const ag_err_t err = fm_read_panel(p, root);
    if (err != AG_OK) {
        fm_error(root, err);
        return;
    }
    p->cursor = 0;
    fm_message("");
}

int FM_ENTRY(int argc, char **argv)
{
    /*
     * Both panels' entries come out of the process arena, which is the whole
     * point of having one: 512 entries a panel is a quarter of a megabyte, and
     * when this process ends it all goes back without anybody freeing it.
     */
    g_active = 0;
    for (int i = 0; i < 2; i++) {
        /* Cleared rather than assumed empty: as a built-in this runs more than
         * once, and what the last run left behind is not a starting state. */
        memset(&g_panel[i], 0, sizeof(g_panel[i]));
        g_panel[i].entries =
            (fm_entry_t *)ag_malloc(sizeof(fm_entry_t) * FM_MAX_ENTRIES);
        if (g_panel[i].entries == NULL) {
            ag_free(g_panel[0].entries);
            ag_print("not enough memory for the panels\n");
            return 1;
        }
    }

    char start[AG_PATH_MAX];
    if (ag_getcwd(start, sizeof(start)) != AG_OK) {
        ag_strlcpy(start, "/", sizeof(start));
    }

    const char *left = (argc > 1) ? argv[1] : start;
    const char *right = (argc > 2) ? argv[2] : start;

    ag_err_t err = fm_read_panel(&g_panel[0], left);
    if (err != AG_OK) {
        ag_printf("%s: %s\n", left, ag_strerror(err));
        return 1;
    }
    err = fm_read_panel(&g_panel[1], right);
    if (err != AG_OK) {
        /* One good panel is enough to start with. */
        (void)fm_read_panel(&g_panel[1], g_panel[0].path);
    }
    /* The panel that is not active must not leave the cwd where it looked last. */
    (void)ag_chdir(g_panel[0].path);

    const char *exit_why = NULL;

    /*
     * Wait until the session adopts us before painting — otherwise the shell's
     * "started…" line (or a late focus) lands on top of the panels.
     */
    ag_log(AG_LOG_INFO, "fm", "waiting for focus (pid %u)",
           (unsigned)ag_getpid());
    while (!ag_focused() && !ag_interrupted()) {
        ag_event_t ev;
        if (ag_poll_event(&ev, 50)) {
            ag_log(AG_LOG_INFO, "fm", "pre-focus ev type=%u focused=%d",
                   (unsigned)ev.type, ag_focused() ? 1 : 0);
            /* QUIT while unfocused is for the shell — never exit on it. */
            if (ev.type == AG_EV_FOCUS_GAINED) {
                break;
            }
        }
        ag_heartbeat();
    }
    if (ag_interrupted()) {
        exit_why = "interrupted before focus";
        goto out;
    }
    ag_log(AG_LOG_INFO, "fm", "focused - painting");

    fm_ui_begin();
    fm_draw_all();
    fm_message("F1 for the keys");
    fm_ui_present();

    bool running = true;
    while (running) {
        if (ag_interrupted()) {
            exit_why = "interrupted";
            break;
        }

        ag_event_t ev;
        if (!ag_poll_event(&ev, 50)) {
            ag_heartbeat();
            continue;
        }

        if (ev.type == AG_EV_FOCUS_LOST) {
            ag_log(AG_LOG_INFO, "fm", "FOCUS_LOST");
            continue;
        }
        if (ev.type == AG_EV_FOCUS_GAINED) {
            ag_log(AG_LOG_INFO, "fm", "FOCUS_GAINED - redraw");
            fm_ui_begin();
            fm_draw_all();
            fm_ui_present();
            continue;
        }

        /*
         * Ctrl+C while focused → QUIT.  A QUIT left in the console queue from
         * enter_shell_view must not kill us when we are (or were) in the
         * background.
         */
        if (ev.type == AG_EV_QUIT) {
            if (!ag_focused()) {
                ag_log(AG_LOG_INFO, "fm", "ignore QUIT while unfocused");
                continue;
            }
            exit_why = "QUIT while focused";
            break;
        }
        if (!ag_focused() || ev.type != AG_EV_KEY_DOWN) {
            continue;
        }

        const uint16_t key = ev.key.keycode;
        const bool     alt = (ev.key.mods & AG_MOD_ALT) != 0;
        bool           moved_only = false;

        if (alt && key == AG_KEY_F1) {
            choose_drive(0);
            fm_draw_all();
            continue;
        }
        if (alt && key == AG_KEY_F2) {
            choose_drive(1);
            fm_draw_all();
            continue;
        }

        switch (key) {
        case AG_KEY_UP:       move_cursor(-1); moved_only = true; break;
        case AG_KEY_DOWN:     move_cursor(1); moved_only = true; break;
        case AG_KEY_PAGEUP:   move_cursor(-FM_VISIBLE); moved_only = true; break;
        case AG_KEY_PAGEDOWN: move_cursor(FM_VISIBLE); moved_only = true; break;
        case AG_KEY_HOME:     move_cursor(-FM_MAX_ENTRIES); moved_only = true; break;
        case AG_KEY_END:      move_cursor(FM_MAX_ENTRIES); moved_only = true; break;

        case AG_KEY_TAB:
            g_active ^= 1;
            /* The active panel owns the working directory: a program started
             * from here should see what the panel shows. */
            (void)ag_chdir(fm_active()->path);
            break;

        case AG_KEY_ENTER:
            enter_current();
            break;

        case AG_KEY_BACKSPACE: {
            fm_panel_t *p = fm_active();
            char        target[AG_PATH_MAX];
            fm_join(p->path, "..", target, sizeof(target));
            (void)fm_read_panel(p, target);
            break;
        }

        case AG_KEY_F1:  fm_help(); break;
        case AG_KEY_F2:  (void)fm_reload(fm_active()); fm_message("reloaded"); break;
        case AG_KEY_F3:  fm_view(); break;
        case AG_KEY_F5:  fm_copy(); break;
        case AG_KEY_F6:  fm_move(); break;
        case AG_KEY_F7:  fm_mkdir(); break;
        case AG_KEY_F8:  fm_delete(); break;
        case AG_KEY_F10:
        case AG_KEY_ESC:
            exit_why = (key == AG_KEY_ESC) ? "ESC" : "F10";
            running = false;
            break;

        default:
            break;
        }

        if (running && ag_focused()) {
            if (moved_only) {
                fm_draw_light();
            } else {
                fm_draw_all();
            }
        }
    }

    fm_ui_end();

out:
    if (exit_why == NULL) {
        exit_why = "out";
    }
    ag_log(AG_LOG_INFO, "fm", "exit: %s (focused=%d)", exit_why,
           ag_focused() ? 1 : 0);
    /* Arena is reclaimed with the process; free anyway for tidy builtins. */
    for (int i = 0; i < 2; i++) {
        ag_free(g_panel[i].entries);
        g_panel[i].entries = NULL;
    }
    return 0;
}
