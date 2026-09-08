/*
 * ArgonOS DESKTOP - the graphical shell, in the manner of Windows 3.11.
 *
 * Plan, decisions and the list of what is deliberately not here:
 * docs/plans/desktop.md.  This file is the event loop, the desktop's own
 * furniture and its menus; the windows live in dsk_wm.c, the drop-downs in
 * dsk_menu.c, the dialogs in dsk_dlg.c.
 *
 * The loop blocks in ag_poll_event with no timeout unless something is
 * animating, which is the difference between a shell that costs nothing while
 * nobody touches it and one that eats a core to show a static picture.
 *
 *   run c:\desktop.axe          Esc or Q leaves
 *   run c:\desktop.axe 30       ... and leaves by itself after 30 seconds,
 *                               which is how the scripted runs stay bounded
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>
#include <argon/libc.h>

#include "dsk.h"
#include "dsk_cursor.h"
#include "dsk_dlg.h"
#include "dsk_folder.h"
#include "dsk_icons.h"
#include "dsk_menu.h"
#include "dsk_paint.h"
#include "dsk_run.h"
#include "dsk_wm.h"

/*
 * Eight kilobytes of stack, not the default sixteen.
 *
 * The stack comes out of internal SRAM for the whole life of the process, and
 * on a board showing a 320x240 surface the largest block left is about fifteen
 * kilobytes - so the default is within a whisker of not fitting at all.  This
 * shell keeps its directory listings on the heap and recurses only as deep as
 * a directory tree, which is what the eight is measured against.
 */
AG_APP_SIZED("DESKTOP", "0.2", "argon", AG_AXE_NEEDS_GFX, 8 * 1024, 0);

static dsk_metrics_t s_m;
static dsk_damage_t  s_damage;
static bool          s_running = true;

/* What the status strip says, and the counters behind it. */
static uint32_t s_ptr_events;
static uint32_t s_key_events;
static uint8_t  s_buttons;
static uint32_t s_repaints;
static uint32_t s_reflushes;
static bool     s_double_buf;
static const char *s_note = "";

static void damage(dsk_rect_t r) { dsk_damage_add(&s_damage, r); }

/* Everything, from the strips to the windows.  Used after a program exits. */
static void repaint_all(void) { damage(s_m.screen); }

/* A file the folder window wants opened, or a directory it handled itself. */
static void open_from_folder(const char *path, bool is_dir)
{
    if (is_dir) {
        return; /* the folder window walks into directories on its own */
    }
    dsk_run_open(path, &s_note);
    damage(s_m.statusbar);
}

/* ---- the double click -------------------------------------------------- */

/*
 * Four hundred milliseconds and three pixels (docs/plans/desktop.md §3.8).
 * The distance matters as much as the time: a touchscreen reports a second tap
 * a few pixels from the first, and without the slack a double tap is two
 * singles.
 */
#define DBL_MS 400u
#define DBL_PX 3

static uint32_t s_last_down_ms;
static int16_t  s_last_down_x, s_last_down_y;

static bool is_double(uint32_t now, int16_t x, int16_t y)
{
    const int16_t dx = (int16_t)(x - s_last_down_x);
    const int16_t dy = (int16_t)(y - s_last_down_y);
    const bool near = (dx >= -DBL_PX && dx <= DBL_PX && dy >= -DBL_PX &&
                       dy <= DBL_PX);
    const bool soon = (now - s_last_down_ms) <= DBL_MS;
    s_last_down_ms = now;
    s_last_down_x = x;
    s_last_down_y = y;
    if (near && soon) {
        /* Do not let a third click read as another double. */
        s_last_down_ms = now - DBL_MS - 1u;
        return true;
    }
    return false;
}

/* ---- the drives, along the left of the desktop -------------------------- */

/*
 * One icon per mounted drive, in a column from the top-left.
 *
 * The letters are asked for rather than listed: the shell knows which of A: to
 * H: answered mountinfo, and a machine with no card in it should not show a
 * card.  Their positions are fixed, so a double click is a coordinate the
 * scripted check can work out for itself.
 */
#define DRIVE_CELL_W 56
#define DRIVE_CELL_H 44
#define DRIVE_MAX    8

typedef struct {
    char       path[8];
    char       label[12];
    dsk_icon_t icon;
} drive_t;

static drive_t s_drives[DRIVE_MAX];
static int     s_ndrives;

/*
 * The drives, asked for by mount point rather than by letter.
 *
 * mountinfo takes the POSIX name the kernel mounted the filesystem under -
 * "/sys", not "C:" and not "c:\\" - which is worth writing down because
 * passing it a DOS path fails silently and the desktop then simply has no
 * drives on it, with nothing anywhere saying why.  The letters below are the
 * spelling the rest of the system uses, and what a window's title shows.
 */
static void find_drives(void)
{
    static const struct {
        const char *mount;
        const char *root;
        dsk_icon_t  icon;
    } k_drives[] = {
        {"/sd", "a:\\", DSK_ICON_FLOPPY},
        {"/sys", "c:\\", DSK_ICON_DRIVE},
        {"/host", "h:\\", DSK_ICON_HOST},
        {"/tmp", "t:\\", DSK_ICON_DRIVE},
    };

    s_ndrives = 0;
    for (unsigned i = 0;
         i < sizeof(k_drives) / sizeof(k_drives[0]) && s_ndrives < DRIVE_MAX;
         i++) {
        ag_fsinfo_t fs;
        if (ag_mountinfo(k_drives[i].mount, &fs) != AG_OK) {
            continue;
        }
        drive_t *d = &s_drives[s_ndrives++];
        ag_strlcpy(d->path, k_drives[i].root, sizeof(d->path));
        d->label[0] = (char)(k_drives[i].root[0] - 32); /* upper case */
        d->label[1] = ':';
        d->label[2] = 0;
        /* A removable /sd is a card; one that is not is a disk. */
        d->icon = (k_drives[i].icon == DSK_ICON_FLOPPY && !fs.removable)
                      ? DSK_ICON_DRIVE
                      : k_drives[i].icon;
    }
}

static dsk_rect_t drive_rect(int i)
{
    if (i < 0 || i >= s_ndrives) {
        return dsk_rect_none();
    }
    const int16_t per_col = (int16_t)(s_m.work.h / DRIVE_CELL_H);
    const int16_t col = (per_col > 0) ? (int16_t)(i / per_col) : 0;
    const int16_t row = (per_col > 0) ? (int16_t)(i % per_col) : 0;
    return dsk_rect((int16_t)(s_m.work.x + 4 + col * DRIVE_CELL_W),
                    (int16_t)(s_m.work.y + 4 + row * DRIVE_CELL_H),
                    DRIVE_CELL_W, DRIVE_CELL_H);
}

static void draw_drives(void)
{
    for (int i = 0; i < s_ndrives; i++) {
        const dsk_rect_t r = drive_rect(i);
        if (dsk_rect_empty(r) || !dsk_visible(r)) {
            continue;
        }
        const int16_t ix = (int16_t)(r.x + (r.w - 2 * DSK_ICON_W) / 2);
        dsk_icon_draw(s_drives[i].icon, ix, (int16_t)(r.y + 2), 2, DSK_TEAL);
        const int16_t tw = dsk_text_small_width(s_drives[i].label);
        dsk_text_small((int16_t)(r.x + (r.w - tw) / 2),
                       (int16_t)(r.y + 2 + 2 * DSK_ICON_H + 2),
                       s_drives[i].label, DSK_WHITE, DSK_TEAL);
    }
}

/* Which drive is under the point, or -1. */
static int drive_at(int16_t x, int16_t y)
{
    for (int i = 0; i < s_ndrives; i++) {
        if (dsk_rect_has(drive_rect(i), x, y)) {
            return i;
        }
    }
    return -1;
}

/* ---- the desktop's menus ----------------------------------------------- */

enum {
    ID_NEW = 1,
    ID_RUN,
    ID_EXIT,
    ID_CASCADE,
    ID_TILE,
    ID_CLOSE,
    ID_CLOSE_ALL,
    ID_ABOUT,
    ID_WINDOW_FIRST = 100, /* + the window's z index */
};

static dsk_menu_t s_menus[3];

static void set_item(dsk_menu_t *m, const char *label, uint16_t id,
                     bool enabled)
{
    if (m->n >= DSK_MENU_ITEMS_MAX) {
        return;
    }
    dsk_menu_item_t *it = &m->items[m->n++];
    it->label = label;
    it->id = id;
    it->separator = false;
    it->enabled = enabled;
    it->checked = false;
}

static void set_separator(dsk_menu_t *m)
{
    if (m->n >= DSK_MENU_ITEMS_MAX) {
        return;
    }
    dsk_menu_item_t *it = &m->items[m->n++];
    it->label = "";
    it->id = 0;
    it->separator = true;
    it->enabled = false;
    it->checked = false;
}

/* Titles for the window list, held so the menu can point at them. */
static char s_win_labels[DSK_WIN_MAX][DSK_TITLE_MAX + 4];

static void rebuild_menus(void)
{
    const int n = dsk_wm_count();
    dsk_win_t *active = dsk_wm_active();

    s_menus[0].title = "File";
    s_menus[0].n = 0;
    set_item(&s_menus[0], "New window", ID_NEW, true);
    set_item(&s_menus[0], "Run...", ID_RUN, true);
    set_separator(&s_menus[0]);
    set_item(&s_menus[0], "Exit", ID_EXIT, true);

    s_menus[1].title = "Window";
    s_menus[1].n = 0;
    set_item(&s_menus[1], "Cascade", ID_CASCADE, n > 0);
    set_item(&s_menus[1], "Tile", ID_TILE, n > 0);
    set_separator(&s_menus[1]);
    set_item(&s_menus[1], "Close", ID_CLOSE, active != NULL);
    set_item(&s_menus[1], "Close all", ID_CLOSE_ALL, n > 0);
    if (n > 0) {
        set_separator(&s_menus[1]);
    }
    /* Topmost first, which is the order somebody looking at the screen sees. */
    for (int i = n - 1; i >= 0; i--) {
        dsk_win_t *w = dsk_wm_at(i);
        char      *label = s_win_labels[i];
        ag_strlcpy(label, w->title, DSK_TITLE_MAX + 4);
        set_item(&s_menus[1], label, (uint16_t)(ID_WINDOW_FIRST + i), true);
        s_menus[1].items[s_menus[1].n - 1].checked = (w == active);
    }

    s_menus[2].title = "Help";
    s_menus[2].n = 0;
    set_item(&s_menus[2], "About...", ID_ABOUT, true);

    dsk_menu_set(s_menus, 3);
}

static void about_done(dsk_answer_t a, void *ctx)
{
    (void)a;
    (void)ctx;
}

static void exit_done(dsk_answer_t a, void *ctx)
{
    (void)ctx;
    if (a == DSK_ANSWER_YES) {
        s_running = false;
    }
}

static void run_typed(dsk_answer_t a, const char *text, void *ctx)
{
    (void)ctx;
    if (a == DSK_ANSWER_OK && text != NULL && text[0] != 0) {
        dsk_run_command(text, &s_note);
        damage(s_m.statusbar);
    }
}

static void menu_chose(uint16_t id)
{
    if (id >= ID_WINDOW_FIRST) {
        dsk_wm_activate(dsk_wm_at((int)(id - ID_WINDOW_FIRST)));
        return;
    }
    switch (id) {
    case ID_NEW:
        /* A window onto C:, which is the drive every machine here has. */
        if (dsk_folder_open("c:\\") == NULL) {
            s_note = "no room for another window";
            damage(s_m.statusbar);
        }
        break;
    case ID_RUN:
        (void)dsk_dlg_input("Run", "Program to run:", "c:\\", run_typed, NULL);
        break;
    case ID_EXIT:
        (void)dsk_dlg_message("Exit", "Leave the desktop?", NULL,
                              DSK_DLG_YESNO, exit_done, NULL);
        break;
    case ID_CASCADE:
        dsk_wm_cascade();
        break;
    case ID_TILE:
        dsk_wm_tile();
        break;
    case ID_CLOSE:
        dsk_wm_close(dsk_wm_active());
        break;
    case ID_CLOSE_ALL:
        dsk_wm_close_all();
        break;
    case ID_ABOUT:
        (void)dsk_dlg_message("About", "ArgonOS Desktop 0.2",
                              "phase 1 - windows and menus", DSK_DLG_OK,
                              about_done, NULL);
        break;
    default:
        break;
    }
}

/* ---- the desktop's own furniture --------------------------------------- */

static void draw_statusbar(void)
{
    if (dsk_rect_empty(s_m.statusbar) || !dsk_visible(s_m.statusbar)) {
        return;
    }
    char        line[96];
    char        num[24];
    const int16_t ty = (int16_t)(s_m.statusbar.y + 2);

    dsk_fill(s_m.statusbar, DSK_LGRAY);
    dsk_hline(s_m.statusbar.x, s_m.statusbar.y, s_m.statusbar.w, DSK_WHITE);

    ag_strlcpy(line, "ptr ", sizeof(line));
    ag_strlcat(line, ag_utoa((uint64_t)(uint32_t)dsk_cursor_x(), num,
                             sizeof(num), 0, false),
               sizeof(line));
    ag_strlcat(line, ",", sizeof(line));
    ag_strlcat(line, ag_utoa((uint64_t)(uint32_t)dsk_cursor_y(), num,
                             sizeof(num), 0, false),
               sizeof(line));
    ag_strlcat(line, "  win ", sizeof(line));
    ag_strlcat(line,
               ag_utoa((uint64_t)(uint32_t)dsk_wm_count(), num, sizeof(num), 0,
                       false),
               sizeof(line));
    ag_strlcat(line, "  ptrev ", sizeof(line));
    ag_strlcat(line, ag_utoa((uint64_t)s_ptr_events, num, sizeof(num), 0,
                             false),
               sizeof(line));
    ag_strlcat(line, "  keyev ", sizeof(line));
    ag_strlcat(line, ag_utoa((uint64_t)s_key_events, num, sizeof(num), 0,
                             false),
               sizeof(line));
    if (s_note[0] != '\0') {
        ag_strlcat(line, "  ", sizeof(line));
        ag_strlcat(line, s_note, sizeof(line));
    }

    dsk_text_small((int16_t)(s_m.statusbar.x + 4), ty, line, DSK_BLACK,
                   DSK_LGRAY);
}

/* Paint everything that falls inside r.  The only painting path there is. */
static void draw_region(dsk_rect_t r)
{
    if (dsk_rect_empty(r)) {
        return;
    }
    dsk_clip(r);
    dsk_fill(s_m.work, DSK_TEAL);
    draw_drives();
    dsk_clip_reset();

    dsk_wm_draw(r);

    dsk_clip(r);
    dsk_menu_draw_bar(r);
    draw_statusbar();
    dsk_clip_reset();

    /* Last of all, because a menu is above every window. */
    dsk_menu_draw_open(r);
}

/*
 * Repaint what is damaged, then show it.
 *
 * The order is the whole of the software-cursor contract: the pointer comes off
 * the screen first, so that nothing is drawn under a stale copy of the pixels
 * it saved, and goes back on last.
 */
static void commit(void)
{
    if (s_damage.n == 0) {
        return;
    }
    dsk_damage_clip(&s_damage, s_m.screen);

    /*
     * Pointer off, then outline off, then paint, then both back on in the
     * other order - because the pointer is above the outline and each of them
     * holds a copy of what it covered.
     */
    dsk_cursor_hide();
    dsk_wm_outline_off();
    for (uint8_t i = 0; i < s_damage.n; i++) {
        draw_region(s_damage.r[i]);
    }
    dsk_wm_outline_on();
    dsk_cursor_show();

    /* The pointer's square needs a flush even though it needed no repaint. */
    dsk_damage_add(&s_damage, dsk_cursor_rect());
    for (uint8_t i = 0; i < s_damage.n; i++) {
        dsk_flush(s_damage.r[i]);
    }
    dsk_damage_clear(&s_damage);
}

/*
 * The status strip is rate limited, and the reason is the wire to the CYD.
 *
 * The pointer's own cost is two squares of sixteen pixels; repainting a strip
 * the whole width of the screen next to it multiplies that by twenty, for text
 * nobody can read while the mouse is moving anyway.  So a move marks the strip
 * dirty and it is redrawn at most ten times a second - which also means the
 * position it finally settles at is the position it displays.
 */
#define DSK_STATUS_MS 100u

static bool     s_status_dirty;
static uint32_t s_status_at;

static uint32_t status_due_in(uint32_t now)
{
    if (!s_status_dirty) {
        return UINT32_MAX;
    }
    const uint32_t since = now - s_status_at;
    return (since >= DSK_STATUS_MS) ? 0u : (DSK_STATUS_MS - since);
}

static void status_settle(uint32_t now)
{
    if (status_due_in(now) != 0u) {
        return;
    }
    s_status_dirty = false;
    s_status_at = now;
    damage(s_m.statusbar);
}

/* ---- input ------------------------------------------------------------- */

static void on_pointer(dsk_ptr_t type, int16_t x, int16_t y, uint8_t buttons,
                       uint32_t now)
{
    s_ptr_events++;
    s_buttons = buttons;
    dsk_cursor_move(x, y);
    s_status_dirty = true;

    const bool dbl = (type == DSK_PTR_DOWN) ? is_double(now, x, y) : false;

    /* The menu is above everything, so it is asked first. */
    if (dsk_menu_pointer(type, x, y)) {
        return;
    }
    if (dsk_wm_pointer(type, x, y, buttons, dbl)) {
        return;
    }
    /* Nothing wanted it: the click was on the desktop itself. */
    if (type == DSK_PTR_DOWN && dbl) {
        const int d = drive_at(x, y);
        if (d >= 0 && dsk_folder_open(s_drives[d].path) == NULL) {
            s_note = "no room for another window";
            damage(s_m.statusbar);
        }
    }
}

static void on_key(uint16_t keycode, uint32_t unicode, uint16_t mods)
{
    s_key_events++;
    s_status_dirty = true;

    /*
     * The shell's own keys come before anybody else's - and they are Ctrl
     * chords, not Alt ones, for a reason that is not taste: **the supervisor
     * has already taken Alt+Tab and Alt+1..4** for its session slots, and it
     * takes them before an application sees a thing (see hotkeys() in
     * src/proc/supervisor.c).  A shell that bound Alt+Tab to its own windows
     * would appear to work and then, one press in, hand the screen to an empty
     * slot with a shell prompt on it - which is what happened the first time
     * this sequence was scripted.
     *
     * Ctrl+Tab and Ctrl+F4 are what Windows 3.11 used for the windows *inside*
     * an application, which is exactly what these are, so nothing is being
     * invented to dodge the collision.  Alt+F4 stays what it was there too:
     * leave the application.
     */
    if ((mods & DSK_MOD_CTRL) != 0 && keycode == AG_KEY_TAB) {
        dsk_menu_close();
        dsk_wm_cycle();
        return;
    }
    if ((mods & DSK_MOD_CTRL) != 0 && keycode == AG_KEY_F4) {
        dsk_menu_close();
        dsk_wm_close(dsk_wm_active());
        return;
    }
    if ((mods & DSK_MOD_ALT) != 0 && keycode == AG_KEY_F4) {
        dsk_menu_close();
        menu_chose(ID_EXIT);
        return;
    }
    if (keycode == AG_KEY_F5) {
        s_repaints++;
        damage(s_m.screen);
        return;
    }
    /*
     * F6 sends the frame again without drawing a thing.  It exists to separate
     * two failures that look identical in a photograph: pixels this shell
     * never wrote, and pixels it wrote that never reached the panel.
     */
    if (keycode == AG_KEY_F6) {
        s_reflushes++;
        dsk_flush(s_m.screen);
        return;
    }

    if (dsk_menu_key(keycode, unicode, mods)) {
        return;
    }
    if (dsk_wm_key(keycode, unicode, mods)) {
        return;
    }

    /* Nobody wanted it.  Only then does a bare key mean "leave". */
    if (keycode == AG_KEY_ESC || keycode == AG_KEY_Q) {
        s_running = false;
    }
}

/* ---- main -------------------------------------------------------------- */

static uint32_t parse_seconds(int argc, char **argv)
{
    if (argc < 2 || argv[1] == NULL) {
        return 0;
    }
    uint32_t v = 0;
    for (const char *p = argv[1]; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        v = v * 10u + (uint32_t)(*p - '0');
        if (v > 86400u) {
            return 86400u;
        }
    }
    return v;
}

static uint32_t soonest(uint32_t a, uint32_t b) { return (a < b) ? a : b; }

/*
 * One event at a time, except that a backlog of pointer moves is one event.
 *
 * A move that has been superseded is worth nothing: only the latest position
 * matters, and acting on the older ones costs a repaint each while the pointer
 * falls further behind the hand.  That is not a theoretical tidiness - during
 * an outline drag the shell was seconds behind the mouse, and the button had
 * come up long before it read the move that was supposed to precede it, so the
 * window went to the wrong place and the outline stayed on the screen.
 *
 * So: take the newest move and drop the ones it replaced, and hold back
 * whatever non-move event ended the run rather than losing it.
 */
static bool       s_have_held;
static ag_event_t s_held;
static uint32_t   s_moves_dropped;

static bool next_event(ag_event_t *ev, uint32_t wait)
{
    if (s_have_held) {
        *ev = s_held;
        s_have_held = false;
        return true;
    }
    if (!ag_poll_event(ev, wait)) {
        return false;
    }
    if (ev->type != AG_EV_POINTER_MOVE) {
        return true;
    }
    for (;;) {
        ag_event_t nxt;
        if (!ag_poll_event(&nxt, 0)) {
            break;
        }
        if (nxt.type == AG_EV_POINTER_MOVE) {
            *ev = nxt;
            s_moves_dropped++;
            continue;
        }
        s_held = nxt;
        s_have_held = true;
        break;
    }
    return true;
}

int ag_main(int argc, char **argv)
{
    ag_gfxinfo_t info;

    if (ag_api()->gfx == NULL) {
        ag_printf("desktop: this machine has no display\n");
        return 1;
    }
    const ag_err_t err = ag_gfx_acquire(&info);
    if (err != AG_OK) {
        ag_printf("desktop: cannot take the display (%d)\n", (int)err);
        return 1;
    }
    if (info.fb == NULL) {
        /*
         * A board with no system surface at all - the pixels would have to be
         * the shell's own and go out in bands through gfx->present.  That
         * backend is planned (docs/plans/desktop.md §9.2) and is not here, and
         * drawing anyway would paint into nothing and say nothing about it.
         */
        ag_gfx_release();
        ag_printf("desktop: this display has no surface (%ux%u); "
                  "the band renderer is not built yet\n",
                  (unsigned)info.width, (unsigned)info.height);
        return 1;
    }

    s_double_buf = info.double_buf;

    /*
     * Wait to be given the screen before drawing on it.
     *
     * A process is adopted into its session slot a little after it starts
     * running - measured here at ninety milliseconds after the first
     * instruction - and until then it is nobody's foreground.  Painting in
     * that window is work thrown away: the kernel is right to ignore a flush
     * from a slot that is not focused, and this shell would rather find out by
     * asking than by looking at a screen that still shows the console.
     *
     * Bounded, and then it paints anyway: a machine with no session layer at
     * all must not be a machine where this hangs.
     */
    for (uint32_t waited = 0; !ag_focused() && waited < 2000u; waited += 10u) {
        ag_delay(10);
    }

    /*
     * Said on the console as well as drawn on the screen, because a script
     * driving this cannot read the screen: the surface it got is the first
     * thing that has to be checkable from the transcript.
     */
    ag_printf("desktop: surface %ux%u %s, focus %s\n", (unsigned)info.width,
              (unsigned)info.height, info.double_buf ? "double" : "single",
              ag_focused() ? "yes" : "no");

    dsk_metrics_init(&s_m, (int16_t)info.width, (int16_t)info.height);
    dsk_paint_bind_surface(info.fb, info.stride, s_m.screen_w, s_m.screen_h);
    dsk_cursor_init(s_m.screen_w, s_m.screen_h);
    dsk_wm_init(&s_m, damage);
    dsk_menu_init(&s_m, damage, menu_chose);
    dsk_dlg_init(&s_m);
    dsk_folder_init(&s_m, open_from_folder);
    dsk_run_init(repaint_all);
    find_drives();
    rebuild_menus();

    /* First paint: everything, once. */
    dsk_damage_clear(&s_damage);
    damage(s_m.screen);
    commit();

    const uint32_t deadline_s = parse_seconds(argc, argv);
    const uint32_t started = ag_millis();

    s_status_at = started;
    s_status_dirty = false;

    while (s_running) {
        /*
         * With nothing pending this blocks for ever, which is the point: a
         * shell showing a static picture must cost nothing at all.  A dirty
         * status strip, a blinking caret or an armed deadline shortens the
         * wait to whichever comes first.
         */
        uint32_t   now = ag_millis();
        uint32_t   wait = soonest(status_due_in(now), dsk_dlg_wait_ms(now));
        ag_event_t ev;

        if (deadline_s != 0u) {
            const uint32_t elapsed = now - started;
            const uint32_t total = deadline_s * 1000u;
            wait = soonest(wait, (elapsed >= total) ? 0u : total - elapsed);
        }

        if (next_event(&ev, s_have_held ? 0u : wait)) {
            now = ag_millis();
            switch (ev.type) {
            case AG_EV_POINTER_MOVE:
                on_pointer(DSK_PTR_MOVE, ev.ptr.x, ev.ptr.y, ev.ptr.buttons,
                           now);
                break;
            case AG_EV_POINTER_DOWN:
                on_pointer(DSK_PTR_DOWN, ev.ptr.x, ev.ptr.y, ev.ptr.buttons,
                           now);
                break;
            case AG_EV_POINTER_UP:
                on_pointer(DSK_PTR_UP, ev.ptr.x, ev.ptr.y, ev.ptr.buttons,
                           now);
                break;
            case AG_EV_WHEEL:
                s_ptr_events++;
                s_status_dirty = true;
                break;
            case AG_EV_KEY_DOWN:
                on_key(ev.key.keycode, ev.key.unicode, ev.key.mods);
                break;
            case AG_EV_CHAR:
                /*
                 * A character with no key event behind it - a terminal, or a
                 * paste.  Only the dialogs want these, and they see them
                 * through the same path.
                 */
                if (dsk_dlg_up()) {
                    (void)dsk_wm_key(0, ev.key.unicode, 0);
                } else if (ev.key.unicode == 'q' || ev.key.unicode == 'Q') {
                    s_running = false;
                }
                break;
            case AG_EV_FOCUS_GAINED:
                /*
                 * The screen is ours again and whatever was on it in the
                 * meantime is not ours.  Repaint the lot; there is no cheaper
                 * correct answer, because nothing tells us what changed.
                 */
                s_repaints++;
                damage(s_m.screen);
                break;
            case AG_EV_FOCUS_LOST:
                /*
                 * Stop drawing.  The kernel ignores a flush from an unfocused
                 * slot anyway, so painting here would be work thrown away, and
                 * on a board it is work taken from whoever is in front.
                 */
                dsk_damage_clear(&s_damage);
                s_status_dirty = false;
                break;
            case AG_EV_QUIT:
                s_running = false;
                break;
            default:
                break;
            }
            /*
             * The Window menu lists what is open and ticks what is on top, so
             * it is rebuilt after anything that could have changed either.
             * Cheap: it is a dozen pointer assignments, not an allocation.
             */
            rebuild_menus();
        }

        if (ag_interrupted()) {
            s_running = false;
        }
        now = ag_millis();
        if (deadline_s != 0u && (now - started) >= deadline_s * 1000u) {
            s_running = false;
        }
        dsk_dlg_tick(now);
        status_settle(now);
        commit();
    }

    dsk_cursor_hide();
    ag_gfx_release();
    ag_color(AG_LGRAY, AG_BLACK);
    ag_cls();
    ag_cursor(true);
    /*
     * Two lines, and neither of them long.
     *
     * A script reads these out of the console transcript, and the console is
     * eighty columns: a longer line is wrapped, with escape sequences inserted
     * through the middle of it, and a pattern that matched yesterday quietly
     * stops matching.  Which is exactly what happened when the sixth counter
     * went on the end of one line.
     */
    ag_printf("desktop: %u pointer events, %u key events, %u repaints\n",
              (unsigned)s_ptr_events, (unsigned)s_key_events,
              (unsigned)s_repaints);
    ag_printf("desktop: %u windows, %u reflushes, %u moves coalesced\n",
              (unsigned)dsk_wm_count(), (unsigned)s_reflushes,
              (unsigned)s_moves_dropped);
    return 0;
}
