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
#include "dsk_clock.h"
#include "dsk_folder.h"
#include "dsk_icons.h"
#include "dsk_menu.h"
#include "dsk_ini.h"
#include "dsk_ops.h"
#include "dsk_paint.h"
#include "dsk_run.h"
#include "dsk_term.h"
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
/*
 * How near two clicks have to be to count as one double click.  The TIME is a
 * setting (`[desktop] dblclick` in DESKTOP.INI) because it is about the hand
 * holding the mouse; the DISTANCE is not, because it is about the mouse.
 */
#define DBL_PX 3

static dsk_ini_t s_ini;

static uint32_t s_last_down_ms;
static int16_t  s_last_down_x, s_last_down_y;

static bool is_double(uint32_t now, int16_t x, int16_t y)
{
    const int16_t dx = (int16_t)(x - s_last_down_x);
    const int16_t dy = (int16_t)(y - s_last_down_y);
    const bool near = (dx >= -DBL_PX && dx <= DBL_PX && dy >= -DBL_PX &&
                       dy <= DBL_PX);
    const bool soon = (now - s_last_down_ms) <= s_ini.dblclick_ms;
    s_last_down_ms = now;
    s_last_down_x = x;
    s_last_down_y = y;
    if (near && soon) {
        /* Do not let a third click read as another double. */
        s_last_down_ms = now - (uint32_t)s_ini.dblclick_ms - 1u;
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
    char       path[8];  /* "c:\\", the spelling everything else uses      */
    char       mount[12]; /* "/sys", which is what mountinfo answers to    */
    char       label[12];
    dsk_icon_t icon;
    /*
     * Where its cell sits, in work-area pixels.  Laid out in a column to
     * begin with and then wherever it was dragged to - which is why this is a
     * position and not an index: an index would mean the icons shuffle when a
     * card is taken out, and an icon that moves on its own is an icon nobody
     * can arrange.
     */
    int16_t x, y;
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
        const int at = s_ndrives++;
        drive_t  *d = &s_drives[at];
        ag_strlcpy(d->path, k_drives[i].root, sizeof(d->path));
        ag_strlcpy(d->mount, k_drives[i].mount, sizeof(d->mount));
        d->label[0] = (char)(k_drives[i].root[0] - 32); /* upper case */
        d->label[1] = ':';
        d->label[2] = 0;
        /* A removable /sd is a card; one that is not is a disk. */
        d->icon = (k_drives[i].icon == DSK_ICON_FLOPPY && !fs.removable)
                      ? DSK_ICON_DRIVE
                      : k_drives[i].icon;

        /* Down the left edge to begin with, then wherever it was left. */
        const int16_t per_col = (int16_t)(s_m.work.h / DRIVE_CELL_H);
        const int16_t col = (per_col > 0) ? (int16_t)(at / per_col) : 0;
        const int16_t row = (per_col > 0) ? (int16_t)(at % per_col) : 0;
        d->x = (int16_t)(4 + col * DRIVE_CELL_W);
        d->y = (int16_t)(4 + row * DRIVE_CELL_H);
        (void)dsk_ini_icon_of(&s_ini, d->label, &d->x, &d->y);
    }

    /*
     * Say which ones there are, because a scripted run cannot see them.
     *
     * The scenario aims at "the second icon" by arithmetic and then checks
     * DESKTOP.INI for a drive by name - and which drive is second depends
     * on what happens to be mounted.  When the two disagree the run fails
     * with "no position for the icon that was dragged", which reads as a
     * shell that forgot and is a test that aimed at the wrong cell.
     */
    char line[DRIVE_MAX * 4 + 1];
    line[0] = '\0';
    for (int i = 0; i < s_ndrives; i++) {
        ag_strlcat(line, s_drives[i].label, sizeof(line));
        ag_strlcat(line, " ", sizeof(line));
    }
    ag_printf("desktop: drives %s\n", line);
}

/*
 * Which drive icon is picked, or -1.
 *
 * One click picks, two open.  Windows 3.11 had no properties on a drive and no
 * right button to ask with, so the way to ask about one has to be the way to
 * ask about anything else: pick it, then File > Properties.  Without a
 * selection there is nothing for that menu item to be about, and a feature
 * that cannot be reached is a feature that is not there.
 */
static int s_drive_sel = -1;

/*
 * Where the time goes, in the strip where Maxim can read it (`-lag`).
 *
 * "It hangs sometimes" cannot be chased from here: the shell sees events
 * arrive and pixels go out, and a stall is either one arriving late or the
 * other taking long, and those have nothing to do with each other.  So both
 * are measured and both are shown:
 *
 *   p  the longest a single repaint took, milliseconds
 *   g  the longest gap between two pointer events, milliseconds
 *
 * A large p is this shell being slow.  A large g with a small p is the
 * pointer not being delivered - the driver, the bus, or the console tick that
 * polls it - and nothing this shell does will help.
 *
 * Both are worst-of-the-last-three-seconds rather than worst-ever: a number
 * that only ever grows says what happened once, and what is wanted is what is
 * happening now.
 */
static bool     s_lag;
static uint32_t s_worst_paint, s_worst_gap;
static uint32_t s_lag_since, s_last_ptr;

/* Its cell, in screen pixels.  The stored position is relative to the work
 * area, so a shell that starts on a different surface still puts them on it. */
static dsk_rect_t drive_rect(int i)
{
    if (i < 0 || i >= s_ndrives) {
        return dsk_rect_none();
    }
    return dsk_rect((int16_t)(s_m.work.x + s_drives[i].x),
                    (int16_t)(s_m.work.y + s_drives[i].y), DRIVE_CELL_W,
                    DRIVE_CELL_H);
}

/*
 * Keeps a dragged icon on the desk.
 *
 * Clamped rather than refused: a drag that ends off the edge should leave the
 * icon at the edge, not back where it started - an icon that springs back
 * looks like a shell that ignored the drag.
 */
static void clamp_icon(int16_t *x, int16_t *y)
{
    const int16_t max_x = (int16_t)(s_m.work.w - DRIVE_CELL_W);
    const int16_t max_y = (int16_t)(s_m.work.h - DRIVE_CELL_H);
    if (*x < 0) {
        *x = 0;
    }
    if (*y < 0) {
        *y = 0;
    }
    if (*x > max_x) {
        *x = (max_x > 0) ? max_x : 0;
    }
    if (*y > max_y) {
        *y = (max_y > 0) ? max_y : 0;
    }
}

/*
 * The desk itself: a colour, and a tile over it when one is chosen.
 *
 * Only the damaged part is drawn - the clip sees to that - and the tile is
 * anchored to the screen, so a piece repainted on its own lands in step
 * with the rest of it.
 */
static void draw_desk(void)
{
    const uint8_t *rows = dsk_pattern_rows((int)s_ini.pattern);
    if (rows == NULL) {
        dsk_fill(s_m.work, s_ini.background);
        return;
    }
    /*
     * The tile's ink is the desk colour lightened towards white, rather
     * than a second colour to choose: two colours is two decisions and one
     * of them is always wrong at this size.  Windows did the same - its
     * patterns were one colour on the desk colour.
     */
    const uint32_t r8 = (s_ini.background >> 16) & 0xFFu;
    const uint32_t g8 = (s_ini.background >> 8) & 0xFFu;
    const uint32_t b8 = s_ini.background & 0xFFu;
    const uint32_t ink = (((r8 + 0x40u > 0xFFu) ? 0xFFu : r8 + 0x40u) << 16) |
                         (((g8 + 0x40u > 0xFFu) ? 0xFFu : g8 + 0x40u) << 8) |
                         ((b8 + 0x40u > 0xFFu) ? 0xFFu : b8 + 0x40u);

    dsk_pattern(s_m.work, rows, ink, s_ini.background);
}

static void draw_drives(void)
{
    for (int i = 0; i < s_ndrives; i++) {
        const dsk_rect_t r = drive_rect(i);
        if (dsk_rect_empty(r) || !dsk_visible(r)) {
            continue;
        }
        const bool    picked = (i == s_drive_sel);
        const uint32_t behind = picked ? DSK_NAVY : s_ini.background;
        const int16_t ix = (int16_t)(r.x + (r.w - 2 * DSK_ICON_W) / 2);
        const int16_t tw = dsk_text_small_width(s_drives[i].label);
        const int16_t tx = (int16_t)(r.x + (r.w - tw) / 2);
        const int16_t ty = (int16_t)(r.y + 2 + 2 * DSK_ICON_H + 2);

        /*
         * The caption is what carries the selection, not the icon: the icon
         * is composed over the colour behind it, and inverting a 32x32 image
         * to say "this one" is both harder to read and harder to draw than
         * inverting the six characters underneath it.
         */
        if (picked) {
            /*
             * Clipped to the cell, and that is not belt and braces.
             *
             * The highlight is the caption's box with a pixel of air around
             * it, and the arithmetic put its last row one pixel below the
             * cell: rows r.y+35..r.y+44 against a cell of r.y..r.y+43.  What
             * gets repainted when an icon moves is the cell, so that one row
             * survived - and it survived only when the icon was dragged
             * UPWARDS, because dragging down leaves the stray row inside the
             * new cell, which is painted.  On the CYD, where a surface pixel
             * is two rows of glass, it was a blue line trailing the icon.
             *
             * A long caption does the same thing sideways, so the rule is the
             * one worth stating rather than the pixel: a cell's decoration
             * cannot leave the cell that gets repainted for it.
             */
            dsk_fill(dsk_rect_clip(dsk_rect((int16_t)(tx - 2),
                                            (int16_t)(ty - 1),
                                            (int16_t)(tw + 4),
                                            DSK_SMALL_H + 2),
                                   r),
                     behind);
        }
        /*
         * Over the desk rather than over a rectangle of its colour.
         *
         * With a tile on the desk the difference is the whole point: an
         * icon composed against the flat background left a square hole in
         * the pattern, and so did the caption.  A picked caption keeps its
         * solid block - that block is what "picked" looks like.
         */
        const bool over = (dsk_pattern_rows((int)s_ini.pattern) != NULL);
        dsk_icon_draw(s_drives[i].icon, ix, (int16_t)(r.y + 2), 2,
                      over ? DSK_TRANSPARENT : s_ini.background);
        dsk_text_small(tx, ty, s_drives[i].label, DSK_WHITE,
                       (over && !picked) ? DSK_TRANSPARENT : behind);
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

/* Both defined further down, and both wanted here: an icon dropped saves the
 * arrangement, and the arrangement is what puts the windows back. */
static void save_arrangement(void);
static void restore_windows(void);

/* ---- putting the windows back ------------------------------------------- */

/*
 * Reopens what was open, in the order it was open, and no more than that.
 *
 * A directory that is no longer there is skipped rather than reported: a card
 * that was in the slot last time and is not now is the ordinary case, and a
 * shell that starts with a message box about it is a shell that has to be
 * dismissed before it can be used.  The window is simply not there, which is
 * the truth.
 *
 * The frame is clamped to the work area for the case the arrangement was
 * saved on a bigger screen - the same shell runs on a 640x400 window and a
 * 320x240 panel, and a window restored off the edge of the smaller one is a
 * window that cannot be reached or closed.
 */
static void restore_windows(void)
{
    for (int i = 0; i < s_ini.nwins; i++) {
        const dsk_ini_win_t *want = &s_ini.win[i];

        ag_stat_t st;
        if (ag_stat(want->path, &st) != AG_OK || (st.attr & AG_A_DIR) == 0) {
            continue;
        }

        dsk_win_t *w = dsk_folder_open(want->path);
        if (w == NULL) {
            break; /* out of windows: the rest are not going to fit either */
        }

        dsk_rect_t f = want->frame;
        if (f.w > s_m.work.w) {
            f.w = s_m.work.w;
        }
        if (f.h > s_m.work.h) {
            f.h = s_m.work.h;
        }
        if (dsk_rect_x2(f) > dsk_rect_x2(s_m.work)) {
            f.x = (int16_t)(dsk_rect_x2(s_m.work) - f.w);
        }
        if (dsk_rect_y2(f) > dsk_rect_y2(s_m.work)) {
            f.y = (int16_t)(dsk_rect_y2(s_m.work) - f.h);
        }
        if (f.x < s_m.work.x) {
            f.x = s_m.work.x;
        }
        if (f.y < s_m.work.y) {
            f.y = s_m.work.y;
        }
        dsk_wm_move(w, f);

        if (want->state == (uint8_t)DSK_WIN_MAXIMISED) {
            dsk_wm_maximise(w);
        } else if (want->state == (uint8_t)DSK_WIN_MINIMISED) {
            dsk_wm_minimise(w);
        }
    }
}

/*
 * Back into the column they started in, and written down as such.
 *
 * The way out of an arrangement that has gone wrong, and the reason it is
 * saved rather than merely applied: an "arrange" that came back rearranged
 * after a restart would be an arrange that did not work.
 */
static void arrange_icons(void)
{
    const int16_t per_col = (int16_t)(s_m.work.h / DRIVE_CELL_H);

    for (int i = 0; i < s_ndrives; i++) {
        damage(drive_rect(i));
        const int16_t col = (per_col > 0) ? (int16_t)(i / per_col) : 0;
        const int16_t row = (per_col > 0) ? (int16_t)(i % per_col) : 0;
        s_drives[i].x = (int16_t)(4 + col * DRIVE_CELL_W);
        s_drives[i].y = (int16_t)(4 + row * DRIVE_CELL_H);
        dsk_ini_set_icon(&s_ini, s_drives[i].label, s_drives[i].x,
                         s_drives[i].y);
        damage(drive_rect(i));
    }
    save_arrangement();
}

/* ---- dragging an icon --------------------------------------------------- */

/*
 * The icon moves with the pointer rather than behind an outline.
 *
 * A window is dragged as an outline because a window is big and its contents
 * cost a repaint per step; an icon cell is 56x44 of flat colour with a 32x32
 * picture on it, so moving the thing itself costs two small damage rectangles
 * a step and looks like what it is.  Windows 3.11 dragged a ghost of the icon
 * for the same reason.
 */
static struct {
    int     which; /* -1 when nothing is being dragged */
    int16_t dx, dy; /* where in the cell it was grabbed */
    bool    moved;  /* past the threshold, so this is a drag and not a click */
} s_drag = {-1, 0, 0, false};

static void drag_icon_to(int16_t x, int16_t y)
{
    drive_t *d = &s_drives[s_drag.which];
    int16_t  nx = (int16_t)(x - s_drag.dx - s_m.work.x);
    int16_t  ny = (int16_t)(y - s_drag.dy - s_m.work.y);
    clamp_icon(&nx, &ny);

    if (nx == d->x && ny == d->y) {
        return;
    }
    /*
     * A few pixels of slop before it counts as a drag: a click with a hand
     * that moved one pixel is a click, and treating it as a drag would rewrite
     * DESKTOP.INI every time anyone selected a drive.
     */
    if (!s_drag.moved) {
        const int16_t adx = (int16_t)((nx > d->x) ? nx - d->x : d->x - nx);
        const int16_t ady = (int16_t)((ny > d->y) ? ny - d->y : d->y - ny);
        if (adx <= DBL_PX && ady <= DBL_PX) {
            return;
        }
        s_drag.moved = true;
    }

    damage(drive_rect(s_drag.which)); /* where it was */
    d->x = nx;
    d->y = ny;
    damage(drive_rect(s_drag.which)); /* and where it now is */
}

static void drag_icon_end(void)
{
    const int which = s_drag.which;
    s_drag.which = -1;
    if (!s_drag.moved || which < 0) {
        return;
    }
    s_drag.moved = false;

    dsk_ini_set_icon(&s_ini, s_drives[which].label, s_drives[which].x,
                     s_drives[which].y);
    save_arrangement();
    s_note = "icon moved";
    damage(s_m.statusbar);
}

/* ---- what gets written down -------------------------------------------- */

/*
 * Collects the arrangement and writes it.
 *
 * The windows are taken bottom to top so that reopening them in the same order
 * puts them back in the same pile.  Their PATHS are what is saved, not their
 * contents: a directory that has changed since is read again, which is the
 * only honest thing to do with a list that was a snapshot.
 *
 * Errors are not reported.  This runs on the way out and after an icon has
 * been dragged, and in both cases there is either nobody left to tell or
 * nothing the person could do about it - a message box saying the arrangement
 * could not be saved is a message box in the way of the thing they were doing.
 * It goes in the log instead.
 */
static void save_arrangement(void)
{
    s_ini.nwins = 0;
    for (int z = 0; z < dsk_wm_count() && s_ini.nwins < DSK_INI_WINS; z++) {
        dsk_win_t  *w = dsk_wm_at(z);
        const char *path = dsk_folder_path(w);
        if (path == NULL) {
            continue; /* a dialog: it belongs to a moment, not to an arrangement */
        }
        dsk_ini_win_t *out = &s_ini.win[s_ini.nwins++];
        ag_strlcpy(out->path, path, sizeof(out->path));
        /*
         * A maximised or minimised window is saved by what it will go back to,
         * plus which of the two it is: saving the maximised frame would make
         * "restore" restore it to the whole screen.
         */
        out->frame = (w->state == DSK_WIN_NORMAL) ? w->frame : w->restore;
        out->state = (uint8_t)w->state;
    }

    const ag_err_t err = dsk_ini_save(&s_ini);
    if (err != AG_OK) {
        ag_log(AG_LOG_WARN, "desktop", "could not write %s: %d", DSK_INI_PATH,
               (int)err);
    }
}

/* ---- the desktop's menus ----------------------------------------------- */

enum {
    ID_NEW = 1,
    ID_RUN,
    ID_COPY,
    ID_MOVE,
    ID_RENAME,
    ID_DELETE,
    ID_MKDIR,
    ID_OPEN,

    /*
     * Settings, as menu items rather than a dialog.
     *
     * Every one of them is a choice between three or four named things, and
     * a menu with a tick beside the current one says that in the space it
     * already occupies - which on a 320x240 screen is the whole argument.
     * It is also how Program Manager did it, and it needs no keyboard: this
     * board has none, and until now the font could only be changed by
     * editing DESKTOP.INI on a machine that has one.
     */
    ID_CUT,
    ID_CLIPCOPY,
    ID_PASTE,
    ID_MARK,
    ID_MARK_ALL,
    ID_MARK_NONE,
    ID_PATTERN,
    ID_KBD_CYCLE,
    ID_FONT_LARGE,
    ID_FONT_SMALL,
    ID_BG_TEAL,
    ID_BG_NAVY,
    ID_BG_GREEN,
    ID_BG_BLACK,
    ID_DBL_CYCLE,
    ID_DIM,
    ID_PROPS,
    ID_ARRANGE,
    ID_EXIT,
    ID_CASCADE,
    ID_TILE,
    ID_CLOSE,
    ID_CLOSE_ALL,
    ID_ABOUT,
    ID_CONSOLE,
    ID_WINDOW_FIRST = 100, /* + the window's z index */
};

static dsk_menu_t s_menus[6];

/* ---- file operations ---------------------------------------------------- */

/*
 * What the question was about, kept here because the dialogs answer through a
 * callback and there is only ever one question up at a time (dsk_dlg refuses
 * a second).  A copy of the path rather than a pointer into the folder's
 * entries: re-reading the directory while the box is open would move them.
 */
static struct {
    char path[AG_PATH_MAX]; /* what was selected, whole                     */
    char name[64];          /* its last component, for prompts and errors   */
    char dir[AG_PATH_MAX];  /* the directory it is in                       */
    bool is_dir;
    /* The folder window it came from: a dialog is the active window while
     * it is up, so "which folder?" cannot be asked again afterwards. */
    dsk_win_t *win;
} s_op;

/* dir + name in the DOS spelling the rest of the shell uses. */
static void path_join(const char *dir, const char *name, char *out, size_t len)
{
    ag_strlcpy(out, dir, len);
    const size_t n = strlen(out);
    if (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/') {
        ag_strlcat(out, "\\", len);
    }
    ag_strlcat(out, name, len);
}

/*
 * The selection of the active window, remembered.  False when there is
 * nothing selected - the active window is not a folder, or the cursor is on
 * "..", which is a place and not a file.
 */
static bool take_selection(void)
{
    dsk_win_t *w = dsk_wm_active();
    const char *dir = dsk_folder_path(w);
    if (dir == NULL) {
        s_note = "no folder window is active";
        damage(s_m.statusbar);
        return false;
    }
    if (!dsk_folder_selected(w, s_op.path, sizeof(s_op.path), s_op.name,
                             sizeof(s_op.name), &s_op.is_dir)) {
        /*
         * With marks it does not matter where the cursor is - it can be on
         * ".." - so the first marked entry stands in for it.  What the
         * operation acts on is the marks; this only fills in the name the
         * question is asked about and the directory it is asked from.
         */
        if (dsk_folder_marked(w) == 0 ||
            !dsk_folder_marked_at(w, 0, s_op.path, sizeof(s_op.path),
                                  s_op.name, sizeof(s_op.name),
                                  &s_op.is_dir)) {
            s_note = "nothing is selected";
            damage(s_m.statusbar);
            return false;
        }
    }
    ag_strlcpy(s_op.dir, dir, sizeof(s_op.dir));
    /*
     * Which window this is about, held rather than asked for again.
     *
     * A dialog is a window and it is the active one while it is up, so a
     * callback that asks "which folder is active?" can be answered with the
     * dialog - and then the marks belong to nobody and a two-file copy
     * quietly becomes a one-file copy.  The answer is decided here, where
     * the folder window is unambiguously the thing being acted on.
     */
    s_op.win = w;
    return true;
}

/*
 * Where a copy or a move should go by default: the directory of another open
 * folder window, the way a two-panel manager offers the other panel.  With
 * only one window open there is nowhere better to suggest than where it
 * already is, and the user has to type.
 */
static const char *other_folder_dir(void)
{
    dsk_win_t *active = dsk_wm_active();
    for (int i = 0; i < dsk_wm_count(); i++) {
        dsk_win_t  *w = dsk_wm_at(i);
        const char *p = dsk_folder_path(w);
        if (w != active && p != NULL) {
            return p;
        }
    }
    return s_op.dir;
}

/*
 * Re-read the windows an operation could have changed, and nothing else.
 *
 * Two directories at most: where it came from and where it went.  Refreshing
 * everything would be simpler and is what a first version does, and on HostFS
 * it costs hundreds of milliseconds per open window for directories nothing
 * touched.
 */
static void after_op(const char *touched)
{
    if (!dsk_ops_changed()) {
        return;
    }
    dsk_folder_refresh(s_op.dir);
    if (touched != NULL) {
        dsk_folder_refresh(touched);
    }
}

/*
 * Turns what was typed into a finished destination path.
 *
 * A name with no directory in it is taken as a name in the source's own
 * directory; a path that IS a directory gets the source's name appended.  That
 * second rule is what "copy this there" means, and doing it here rather than
 * in fsops is deliberate: it is a decision about what the user meant, and it
 * belongs where they typed it.
 */
static void resolve_target(const char *typed, char *out, size_t len)
{
    if (typed[0] == '\0') {
        out[0] = '\0';
        return;
    }
    /* Bare name: beside the original. */
    bool has_dir = false;
    for (const char *p = typed; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            has_dir = true;
        }
    }
    if (!has_dir) {
        path_join(s_op.dir, typed, out, len);
    } else {
        ag_strlcpy(out, typed, len);
    }

    ag_stat_t st;
    if (ag_stat(out, &st) == AG_OK && (st.attr & AG_A_DIR) != 0) {
        char joined[AG_PATH_MAX];
        path_join(out, s_op.name, joined, sizeof(joined));
        ag_strlcpy(out, joined, len);
    }
}

/* The directory part of a path, for deciding which windows to re-read. */
static void dir_of(const char *path, char *out, size_t len)
{
    ag_strlcpy(out, path, len);
    int n = (int)strlen(out);
    while (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/') {
        out[--n] = '\0';
    }
    /* "c:\" keeps its separator; anything deeper loses it. */
    if (n > 3) {
        out[n - 1] = '\0';
    }
}

/*
 * Several files into one directory.
 *
 * The single-file path asks for the finished NAME, because renaming while
 * copying is half of what a copy is for.  With more than one there is no
 * such thing as the finished name, so what is typed is a directory and each
 * file keeps the name it has.  Two rules for two situations, and the strip
 * says which one is in force by saying how many are picked.
 */
static void copy_marked(dsk_win_t *w, const char *dir, bool moving, int marks)
{
    /*
     * The directory is made if it is not there.
     *
     * "Into directory:" is a promise about where things are going, and a
     * person who types a name that does not exist yet means make it - the
     * alternative is one error box per file, which is what the first version
     * of this did: two files, two boxes, and thirty keystrokes of the test
     * disappearing into them.
     */
    ag_stat_t st;
    if (ag_stat(dir, &st) != AG_OK) {
        if (!dsk_ops_mkdir(dir, dir)) {
            return;
        }
    } else if ((st.attr & AG_A_DIR) == 0) {
        (void)dsk_dlg_message(moving ? "Move" : "Copy",
                              "That name is a file, not a directory.",
                              "Several things need somewhere to go.",
                              DSK_DLG_OK, NULL, NULL);
        return;
    }

    for (int i = 0; i < marks; i++) {
        char from[AG_PATH_MAX];
        char name[DSK_INPUT_MAX];
        bool is_dir = false;
        if (!dsk_folder_marked_at(w, i, from, sizeof(from), name,
                                  sizeof(name), &is_dir)) {
            break;
        }
        char to[AG_PATH_MAX];
        path_join(dir, name, to, sizeof(to));
        if (ag_stricmp(to, from) == 0) {
            continue; /* into the directory it is already in: nothing to do */
        }
        if (moving) {
            dsk_ops_move(from, to, name);
        } else {
            dsk_ops_copy(from, to, name);
        }
    }
    /*
     * The marks go, whichever it was.  After a move the rows are not there;
     * after a copy they are, and leaving them marked would make the next
     * Delete mean far more than the person meant.
     */
    dsk_folder_mark(w, -1, DSK_MARK_NONE);
}

static void copy_typed(dsk_answer_t a, const char *text, void *ctx)
{
    const bool moving = (ctx != NULL);
    if (a != DSK_ANSWER_OK || text == NULL) {
        return;
    }

    dsk_win_t *const w = s_op.win;
    const int        marks = dsk_folder_marked(w);
    if (marks > 1) {
        char dir[AG_PATH_MAX];
        /* Whatever was typed is a directory here, so it is taken as one
         * without asking the filesystem to agree: it may not exist yet. */
        bool has_dir = false;
        for (const char *q = text; *q != '\0'; q++) {
            if (*q == '/' || *q == '\\' || *q == ':') {
                has_dir = true;
            }
        }
        if (has_dir) {
            ag_strlcpy(dir, text, sizeof(dir));
        } else {
            path_join(s_op.dir, text, dir, sizeof(dir));
        }
        copy_marked(w, dir, moving, marks);
        after_op(dir);
        return;
    }

    char to[AG_PATH_MAX];
    resolve_target(text, to, sizeof(to));
    if (to[0] == '\0') {
        return;
    }
    if (ag_stricmp(to, s_op.path) == 0) {
        (void)dsk_dlg_message(moving ? "Move" : "Copy",
                              "That is where it already is.", NULL,
                              DSK_DLG_OK, NULL, NULL);
        return;
    }

    if (moving) {
        dsk_ops_move(s_op.path, to, s_op.name);
    } else {
        dsk_ops_copy(s_op.path, to, s_op.name);
    }

    char where[AG_PATH_MAX];
    dir_of(to, where, sizeof(where));
    after_op(where);
}

static void rename_typed(dsk_answer_t a, const char *text, void *ctx)
{
    (void)ctx;
    if (a != DSK_ANSWER_OK || text == NULL || text[0] == '\0') {
        return;
    }
    /* A name, not a path: rename moves nothing, and a name with a separator
     * in it would move the file without having said so. */
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            (void)dsk_dlg_message("Rename", "That is a path, not a name.",
                                  "Use Move to put it somewhere else.",
                                  DSK_DLG_OK, NULL, NULL);
            return;
        }
    }

    char to[AG_PATH_MAX];
    path_join(s_op.dir, text, to, sizeof(to));
    if (dsk_ops_rename(s_op.path, to, s_op.name)) {
        after_op(NULL);
        /* Leave the cursor on the file that was just named, not on whatever
         * sorts into that row now. */
        dsk_folder_select_name(dsk_wm_active(), text);
    }
}

static void mkdir_typed(dsk_answer_t a, const char *text, void *ctx)
{
    (void)ctx;
    if (a != DSK_ANSWER_OK || text == NULL || text[0] == '\0') {
        return;
    }
    dsk_win_t  *w = dsk_wm_active();
    const char *dir = dsk_folder_path(w);
    if (dir == NULL) {
        return;
    }
    ag_strlcpy(s_op.dir, dir, sizeof(s_op.dir));

    char path[AG_PATH_MAX];
    path_join(dir, text, path, sizeof(path));
    if (dsk_ops_mkdir(path, text)) {
        after_op(NULL);
        dsk_folder_select_name(w, text);
    }
}

static void delete_answered(dsk_answer_t a, void *ctx)
{
    (void)ctx;
    if (a != DSK_ANSWER_YES) {
        return;
    }

    dsk_win_t *const w = s_op.win;
    const int        marks = dsk_folder_marked(w);
    if (marks > 1) {
        /*
         * Backwards through the marks, and that is not a style choice: each
         * delete is answered by the filesystem before the next is asked for,
         * but the list this walks is the window's own and a row that goes
         * shifts the ones after it.  Taking the last one first means every
         * index this loop still has to use is one it has already read.
         */
        for (int i = marks - 1; i >= 0; i--) {
            char from[AG_PATH_MAX];
            char name[DSK_INPUT_MAX];
            if (!dsk_folder_marked_at(w, i, from, sizeof(from), name,
                                      sizeof(name), NULL)) {
                continue;
            }
            dsk_ops_delete(from, name);
        }
        dsk_folder_mark(w, -1, DSK_MARK_NONE);
        after_op(NULL);
        return;
    }

    dsk_ops_delete(s_op.path, s_op.name);
    after_op(NULL);
}

/*
 * The copy or move box, with the destination already filled in.
 *
 * Split out because a drop has a destination and the menu does not: the menu
 * suggests the other window, a drop names the folder it landed on.  Everything
 * after that is the same path, dialog and all - a dropped file is confirmed
 * and copied by exactly the code a menu copy uses, which is the only reason a
 * gesture that moves data is safe to add at all.
 */
static void ask_copy_to(bool moving, const char *dest)
{
    if (!take_selection()) {
        return;
    }
    char prompt[96];
    char num[24];
    const int marks = dsk_folder_marked(dsk_wm_active());

    ag_strlcpy(prompt, moving ? "Move " : "Copy ", sizeof(prompt));
    if (marks > 1) {
        ag_strlcat(prompt, ag_utoa((uint64_t)marks, num, sizeof(num), 0,
                                   false),
                   sizeof(prompt));
        ag_strlcat(prompt, " items into directory:", sizeof(prompt));
    } else {
        ag_strlcat(prompt, s_op.is_dir ? "directory " : "", sizeof(prompt));
        ag_strlcat(prompt, s_op.name, sizeof(prompt));
        ag_strlcat(prompt, " to:", sizeof(prompt));
    }

    (void)dsk_dlg_input(moving ? "Move" : "Copy", prompt, dest, copy_typed,
                        moving ? (void *)&s_op : NULL);
}

static void ask_copy(bool moving)
{
    ask_copy_to(moving, other_folder_dir());
}

/*
 * A different font, without restarting.
 *
 * Every height in the layout is one line of the chosen font plus a pixel
 * each side, so the metrics are rebuilt - in place, because the window
 * manager, the menus and the dialogs hold a pointer to that structure rather
 * than a copy of it.  The windows keep the frames they have: a person who
 * arranged them meant those rectangles, and the shell has no business moving
 * them because the letters changed size.  A window left maximised is the one
 * exception and it is left alone too - it is a keystroke from being right,
 * and guessing at somebody's arrangement is worse than a wrong edge.
 */
static void apply_font(void)
{
    dsk_ui_font_set(s_ini.small_font ? DSK_UI_FONT_SMALL : DSK_UI_FONT_LARGE);
    dsk_metrics_init(&s_m, s_m.screen.w, s_m.screen.h);
    repaint_all();
    save_arrangement();
    /* Said out loud, because a scripted run cannot read letters off the
     * glass and this is the whole of what the item did. */
    ag_printf("desktop: font 8x%d\n", (int)dsk_ui_h());
}

/*
 * A keyboard of its own, or a keyboard drawn on the glass.
 *
 * Asked of the device registry rather than guessed from the screen size: a
 * machine either has something that produces keystrokes or it does not, and
 * the CYD does not - what types at its prompt is a serial cable, which is
 * not in the room with the person holding the board.
 *
 * The setting wins when it says anything: a board with a keyboard plugged in
 * still has a hand on the screen, and a machine whose keyboard is across the
 * room is not one you want to reach for.
 */
static bool machine_has_keys(void)
{
    ag_devinfo_t info;
    for (uint32_t i = 0; ; i++) {
        if (ag_dev_enumerate(i, AG_DEV_INPUT, &info) != AG_OK) {
            return false;
        }
        if (info.name[0] == 'k' && info.name[1] == 'b' &&
            info.name[2] == 'd') {
            return true;
        }
    }
}

static void apply_keyboard(void)
{
    const bool on = (s_ini.keyboard == 1)   ? true
                    : (s_ini.keyboard == 2) ? false
                                            : !machine_has_keys();
    dsk_dlg_keyboard(on);
    ag_printf("desktop: on-screen keyboard %s%s\n", on ? "on" : "off",
              (s_ini.keyboard == 0) ? " (automatic)" : "");
}

/* ---- the clipboard ------------------------------------------------------ */

/*
 * Names, not paths, and one directory for all of them.
 *
 * Why a clipboard exists at all when Copy... has been here from the start:
 * that box asks for a destination, and asking for a destination means typing
 * one.  The board this shell was written for has a touchscreen and no
 * keyboard, so "copy these three into that window" was a thing the machine
 * could do and a person could not ask for.  Pick, walk, paste - no letters.
 *
 * Thirty-two of them, and one source directory: a clipboard filled from two
 * places at once is not a thing anybody has asked for, and holding whole
 * paths would be eight kilobytes on a machine with a hundred and eleven.
 */
#define CLIP_MAX 32

static struct {
    char dir[AG_PATH_MAX];
    char name[CLIP_MAX][64];
    int  n;
    bool moving; /* cut, as against copy */
} s_clip;

static void clip_take(bool moving)
{
    dsk_win_t *const w = dsk_wm_active();
    const char      *dir = dsk_folder_path(w);
    if (dir == NULL) {
        s_note = "no folder window is active";
        damage(s_m.statusbar);
        return;
    }

    s_clip.n = 0;
    s_clip.moving = moving;
    ag_strlcpy(s_clip.dir, dir, sizeof(s_clip.dir));

    const int marks = dsk_folder_marked(w);
    if (marks > 0) {
        for (int i = 0; i < marks && s_clip.n < CLIP_MAX; i++) {
            (void)dsk_folder_marked_at(w, i, NULL, 0, s_clip.name[s_clip.n],
                                       sizeof(s_clip.name[0]), NULL);
            s_clip.n++;
        }
    } else if (dsk_folder_selected(w, NULL, 0, s_clip.name[0],
                                   sizeof(s_clip.name[0]), NULL)) {
        s_clip.n = 1;
    }

    if (s_clip.n == 0) {
        s_note = "nothing is selected";
    } else if (marks > CLIP_MAX) {
        s_note = moving ? "32 cut (the rest would not fit)"
                        : "32 copied (the rest would not fit)";
    } else {
        s_note = moving ? "cut" : "copied";
    }
    damage(s_m.statusbar);
}

static void clip_paste(void)
{
    dsk_win_t *const w = dsk_wm_active();
    const char      *dir = dsk_folder_path(w);
    if (dir == NULL || s_clip.n == 0) {
        s_note = (s_clip.n == 0) ? "the clipboard is empty"
                                 : "no folder window is active";
        damage(s_m.statusbar);
        return;
    }
    if (ag_stricmp(dir, s_clip.dir) == 0) {
        (void)dsk_dlg_message("Paste", "They are already here.",
                              "Open the window you want them in first.",
                              DSK_DLG_OK, NULL, NULL);
        return;
    }

    for (int i = 0; i < s_clip.n; i++) {
        char from[AG_PATH_MAX];
        char to[AG_PATH_MAX];
        path_join(s_clip.dir, s_clip.name[i], from, sizeof(from));
        path_join(dir, s_clip.name[i], to, sizeof(to));
        if (s_clip.moving) {
            dsk_ops_move(from, to, s_clip.name[i]);
        } else {
            dsk_ops_copy(from, to, s_clip.name[i]);
        }
    }

    /*
     * A cut is spent when it is pasted; a copy is not.
     *
     * Moving the same files a second time would be moving files that are no
     * longer where the clipboard says they are, and the second paste would
     * be a row of error boxes.  Copying them again is a perfectly ordinary
     * thing to want.
     */
    if (s_clip.moving) {
        s_clip.n = 0;
    }
    after_op(dir);
}

/* ---- the context menu -------------------------------------------------- */

/*
 * Right button, or a finger held still.
 *
 * This shell had none of this on purpose - 3.11 had no right button, and the
 * way to ask about a thing was to pick it and use the File menu.  Maxim wants
 * one, which settles it: he is the one using it, and on a touchscreen the
 * argument for it is stronger than the argument from 1992, because a finger
 * has no second button and there is no keyboard beside the panel either.
 *
 * The items are the File menu's own, by the same ids and the same handlers -
 * a context menu that did its own copying would be a second way for a file
 * operation to go wrong.
 */
#define LONG_PRESS_MS 500u

static void set_item(dsk_menu_t *m, const char *label, uint16_t id,
                     bool enabled);
static void set_separator(dsk_menu_t *m);
static void fdrag_cancel(void);

static struct {
    bool     armed;
    int16_t  x, y;
    uint32_t at;
} s_press;

static void context_menu_at(int16_t x, int16_t y)
{
    dsk_menu_t *m = &s_menus[5];
    m->title = "";
    m->n = 0;

    dsk_win_t  *w = dsk_wm_active();
    const bool  in_folder = dsk_folder_path(w) != NULL &&
                           dsk_rect_has(w->frame, x, y);
    const bool  sel = in_folder &&
                     dsk_folder_selected(w, NULL, 0, NULL, 0, NULL);

    if (in_folder) {
        set_item(m, "Open  Enter", ID_OPEN, sel);
        set_item(m, (dsk_folder_marked(w) > 0) ? "Mark / unmark  Space"
                                               : "Mark  Space",
                 ID_MARK, sel);
        set_item(m, "Select all", ID_MARK_ALL, true);
        set_separator(m);
        set_item(m, "Cut", ID_CUT, sel);
        set_item(m, "Copy", ID_CLIPCOPY, sel);
        set_item(m, "Paste", ID_PASTE, s_clip.n > 0);
        set_separator(m);
        set_item(m, "Copy...  F8", ID_COPY, sel);
        set_item(m, "Move...  F7", ID_MOVE, sel);
        set_item(m, "Rename...  F2", ID_RENAME, sel);
        set_item(m, "Delete  Del", ID_DELETE, sel);
        set_separator(m);
        set_item(m, "Create directory...", ID_MKDIR, true);
        set_item(m, "Properties...", ID_PROPS, sel);
    } else {
        const int d = drive_at(x, y);
        if (d >= 0) {
            s_drive_sel = d;
            damage(drive_rect(d));
        }
        set_item(m, "New window", ID_NEW, true);
        set_item(m, "Run...", ID_RUN, true);
        set_separator(m);
        set_item(m, "Properties...", ID_PROPS, d >= 0);
        set_item(m, "Arrange icons", ID_ARRANGE, s_ndrives > 0);
        set_item(m, "System console", ID_CONSOLE, true);
    }
    /* Said out loud so a scripted run can prove the menu happened: a
     * drop-down that opens and closes between two photographs leaves no
     * other trace. */
    ag_printf("desktop: context menu at %d,%d, %u items, on %s\n", (int)x,
              (int)y, (unsigned)m->n, in_folder ? "a file" : "the desk");
    dsk_menu_popup(5, x, y);
}

uint32_t press_due_in(uint32_t now)
{
    if (!s_press.armed) {
        return UINT32_MAX; /* no deadline; zero here would mean "due now" */
    }
    const uint32_t since = now - s_press.at;
    return (since >= LONG_PRESS_MS) ? 0u : (LONG_PRESS_MS - since);
}

/* Held still for long enough: the finger's version of the right button. */
static void press_settle(uint32_t now)
{
    if (!s_press.armed || press_due_in(now) != 0u) {
        return;
    }
    /*
     * Not while a window is being moved or resized.
     *
     * A hand puts a finger on a title bar and waits before it moves - half a
     * second is nothing to a person deciding where to put a window - and the
     * menu was popping up in the middle of that and taking the drag with it.
     * A drag in progress is proof the press was not a hold.
     */
    if (dsk_wm_tracking()) {
        s_press.armed = false;
        return;
    }
    s_press.armed = false;
    fdrag_cancel(); /* it was a press, not the start of a drag */
    context_menu_at(s_press.x, s_press.y);
}

/* ---- dragging a file out of a window ----------------------------------- */

/*
 * Windows 3.11's File Manager copied by dragging, and on a touchscreen that
 * is the natural gesture rather than a clever one: a finger has no second
 * button and no keyboard beside it, so a drag is the only way to say "this
 * one, over there" without going through two dialogs.
 *
 * What a drop does NOT do is move data silently.  It opens the copy box with
 * the folder it landed on already typed in, which means the confirmation, the
 * progress box, the overwrite question and the error reporting are all the
 * ones that were already there and already tested.  A gesture that starts a
 * file operation on its own would be a gesture that can destroy a file by
 * being misread by a resistive panel, and this one cannot.
 */
static struct {
    bool    armed; /* a press landed on a row that could be dragged */
    bool    moved; /* and then moved far enough to mean it */
    int16_t x0, y0;
} s_fdrag;


static void fdrag_cancel(void)
{
    s_fdrag.armed = false;
}

static void fdrag_arm(int16_t x, int16_t y)
{
    dsk_win_t *w = dsk_wm_active();
    if (dsk_folder_path(w) == NULL ||
        !dsk_folder_selected(w, NULL, 0, NULL, 0, NULL)) {
        return;
    }
    /*
     * And not when the window has already made that press the corner of a
     * rubber band.  Both cannot run: the drop takes the release, and a
     * band that never hears the release stays drawn on the glass with its
     * marks half made - which is exactly what happened, silently, because
     * this used to ask "was the press on the picked row?" AFTER the window
     * had moved the picked row to be the one under the press.  It is the
     * window's decision and the window is asked for it.
     */
    if (dsk_folder_band_armed(w)) {
        return;
    }
    s_fdrag.armed = true;
    s_fdrag.moved = false;
    s_fdrag.x0 = x;
    s_fdrag.y0 = y;
}

/* The folder a point lands in: a window's, or a drive icon's on the desk. */
static const char *drop_dir_at(int16_t x, int16_t y)
{
    for (int z = dsk_wm_count() - 1; z >= 0; z--) {
        dsk_win_t *w = dsk_wm_at(z);
        if (dsk_rect_has(w->frame, x, y)) {
            /* The topmost window under the point, folder or not: a dialog
             * standing over a folder is not a way into it. */
            return dsk_folder_path(w);
        }
    }
    const int d = drive_at(x, y);
    return (d >= 0) ? s_drives[d].path : NULL;
}

/*
 * True when the event was the drag's, and nothing else should see it.
 *
 * Only the release of a drag that actually moved is consumed - everything
 * else is passed on, because a press that turns out to be a click must still
 * reach the window as a click.
 */
static bool fdrag_pointer(dsk_ptr_t type, int16_t x, int16_t y,
                          uint8_t buttons)
{
    if (!s_fdrag.armed) {
        return false;
    }
    if (type == DSK_PTR_MOVE && (buttons & 1u) != 0) {
        const int16_t dx = (int16_t)((x > s_fdrag.x0) ? x - s_fdrag.x0
                                                      : s_fdrag.x0 - x);
        const int16_t dy = (int16_t)((y > s_fdrag.y0) ? y - s_fdrag.y0
                                                      : s_fdrag.y0 - y);
        if (!s_fdrag.moved && (dx > DBL_PX || dy > DBL_PX)) {
            s_fdrag.moved = true;
            s_note = "drop it on a folder to copy";
            damage(s_m.statusbar);
        }
        return false;
    }
    if (type != DSK_PTR_UP && (buttons & 1u) != 0) {
        return false;
    }

    const bool dropped = s_fdrag.moved;
    s_fdrag.armed = false;
    s_fdrag.moved = false;
    if (!dropped) {
        return false;
    }

    const char *dest = drop_dir_at(x, y);
    dsk_win_t  *from = dsk_wm_active();
    if (dest == NULL) {
        s_note = "nothing to drop it on there";
        damage(s_m.statusbar);
        return true;
    }
    if (dsk_folder_path(from) != NULL &&
        ag_stricmp(dest, dsk_folder_path(from)) == 0) {
        s_note = "that is where it already is";
        damage(s_m.statusbar);
        return true;
    }
    ask_copy_to(false, dest);
    return true;
}

static void ask_delete(void)
{
    if (!take_selection()) {
        return;
    }
    char      line[96];
    char      num[24];
    const int marks = dsk_folder_marked(dsk_wm_active());

    ag_strlcpy(line, "Delete ", sizeof(line));
    if (marks > 1) {
        ag_strlcat(line, ag_utoa((uint64_t)marks, num, sizeof(num), 0, false),
                   sizeof(line));
        ag_strlcat(line, " picked items?", sizeof(line));
    } else {
        ag_strlcat(line, s_op.name, sizeof(line));
        ag_strlcat(line, "?", sizeof(line));
    }

    /*
     * A directory takes everything under it, and the question has to say so:
     * the filesystem used to refuse a directory that was not empty, and that
     * refusal was the only thing standing between a keystroke and a tree.
     */
    (void)dsk_dlg_message("Delete", line,
                          s_op.is_dir ? "Everything in it goes too." : NULL,
                          DSK_DLG_YESNO, delete_answered, NULL);
}

static void ask_rename(void)
{
    if (!take_selection()) {
        return;
    }
    char prompt[96];
    ag_strlcpy(prompt, "Rename ", sizeof(prompt));
    ag_strlcat(prompt, s_op.name, sizeof(prompt));
    ag_strlcat(prompt, " to:", sizeof(prompt));
    (void)dsk_dlg_input("Rename", prompt, s_op.name, rename_typed, NULL);
}

/* ---- properties -------------------------------------------------------- */

/*
 * A date out of a unix timestamp, without a C library and without a timezone.
 *
 * The system keeps seconds since 1970 and this shell has no localtime to hand,
 * so the arithmetic is here: days since the epoch, then walked forward a year
 * at a time.  A board that has never seen an SNTP server has a clock that
 * starts at zero, and a file stamped 1970 is worth showing as 1970 rather than
 * as blank - "no date" and "the clock was never set" are different facts.
 */
/*
 * Two digits with a leading zero.  ag_utoa pads with SPACES, which is what a
 * right-aligned column of file sizes wants and the opposite of what a date
 * wants - "2026- 9- 8  0: 0" is not a date.
 */
static void pad2(char *out, size_t len, uint32_t v)
{
    char two[3];
    two[0] = (char)('0' + (int)((v / 10u) % 10u));
    two[1] = (char)('0' + (int)(v % 10u));
    two[2] = '\0';
    ag_strlcat(out, two, len);
}

static void format_date(uint64_t unix_s, char *out, size_t len)
{
    static const uint16_t k_month[12] = {31, 28, 31, 30, 31, 30,
                                         31, 31, 30, 31, 30, 31};
    char num[24];

    /*
     * Zero is the ABI's "unknown".  The upper bound is here because the
     * arithmetic below walks forward a year at a time and a number that is
     * not a date walks for ever - and one arrived: a filesystem with no mtime
     * answered (time_t)-1, which sign-extended into 2^64 seconds and, with a
     * 32-bit day count, came out as a plausible-looking day in the year three
     * million.  The kernel no longer passes that on; this refuses to render
     * it either, because a shell that draws whatever it is handed is a shell
     * that reports the next such number as fact.
     */
    if (unix_s == 0 || unix_s > 7258118400ull) { /* past the year 2200 */
        ag_strlcpy(out, "not recorded", len);
        return;
    }

    uint32_t days = (uint32_t)(unix_s / 86400u);
    const uint32_t secs = (uint32_t)(unix_s % 86400u);
    int year = 1970;
    for (;;) {
        const bool leap =
            (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        const uint32_t in_year = leap ? 366u : 365u;
        if (days < in_year) {
            break;
        }
        days -= in_year;
        year++;
    }
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    int month = 0;
    for (;;) {
        uint32_t n = k_month[month];
        if (month == 1 && leap) {
            n = 29;
        }
        if (days < n || month == 11) {
            break;
        }
        days -= n;
        month++;
    }

    out[0] = '\0';
    ag_strlcat(out, ag_utoa((uint64_t)year, num, sizeof(num), 0, false), len);
    ag_strlcat(out, "-", len);
    pad2(out, len, (uint32_t)month + 1u);
    ag_strlcat(out, "-", len);
    pad2(out, len, days + 1u);
    ag_strlcat(out, " ", len);
    pad2(out, len, secs / 3600u);
    ag_strlcat(out, ":", len);
    pad2(out, len, (secs / 60u) % 60u);
    ag_strlcat(out, ":", len);
    pad2(out, len, secs % 60u);
    ag_strlcat(out, " UTC", len);
}

/* The properties of whatever the pointer picked: a drive, or a file. */
static void show_drive_props(int which)
{
    char        rows[5][64];
    const char *lines[5];
    char        num[24];
    ag_fsinfo_t fs;

    if (which < 0 || which >= s_ndrives) {
        return;
    }
    for (int i = 0; i < 5; i++) {
        lines[i] = rows[i];
        rows[i][0] = '\0';
    }

    ag_strlcpy(rows[0], "Drive ", sizeof(rows[0]));
    ag_strlcat(rows[0], s_drives[which].label, sizeof(rows[0]));

    if (ag_mountinfo(s_drives[which].mount, &fs) != AG_OK) {
        ag_strlcpy(rows[1], "not mounted", sizeof(rows[1]));
        (void)dsk_dlg_lines("Properties", lines, 2, NULL, NULL);
        return;
    }

    ag_strlcpy(rows[1], "Filesystem: ", sizeof(rows[1]));
    ag_strlcat(rows[1], fs.fs, sizeof(rows[1]));
    if (fs.read_only) {
        ag_strlcat(rows[1], "  (read only)", sizeof(rows[1]));
    }
    if (fs.removable) {
        ag_strlcat(rows[1], "  removable", sizeof(rows[1]));
    }

    ag_strlcpy(rows[2], "Total:  ", sizeof(rows[2]));
    ag_strlcat(rows[2], ag_utoa(fs.total, num, sizeof(num), 0, true),
               sizeof(rows[2]));
    ag_strlcat(rows[2], " bytes", sizeof(rows[2]));

    ag_strlcpy(rows[3], "Free:   ", sizeof(rows[3]));
    ag_strlcat(rows[3], ag_utoa(fs.free, num, sizeof(num), 0, true),
               sizeof(rows[3]));
    ag_strlcat(rows[3], " bytes", sizeof(rows[3]));

    ag_strlcpy(rows[4], "Mounted at ", sizeof(rows[4]));
    ag_strlcat(rows[4], s_drives[which].mount, sizeof(rows[4]));

    (void)dsk_dlg_lines("Properties", lines, 5, NULL, NULL);
}

static void show_file_props(void)
{
    char        rows[5][64];
    const char *lines[5];
    char        num[24];
    ag_stat_t   st;

    if (!take_selection()) {
        return;
    }
    for (int i = 0; i < 5; i++) {
        lines[i] = rows[i];
        rows[i][0] = '\0';
    }

    if (ag_stat(s_op.path, &st) != AG_OK) {
        ag_strlcpy(rows[0], s_op.name, sizeof(rows[0]));
        ag_strlcpy(rows[1], "gone since the list was read", sizeof(rows[1]));
        (void)dsk_dlg_lines("Properties", lines, 2, NULL, NULL);
        return;
    }

    ag_strlcpy(rows[0], s_op.name, sizeof(rows[0]));

    ag_strlcpy(rows[1], "Type:   ", sizeof(rows[1]));
    ag_strlcat(rows[1], s_op.is_dir ? "directory" : "file", sizeof(rows[1]));

    ag_strlcpy(rows[2], "Size:   ", sizeof(rows[2]));
    if (s_op.is_dir) {
        /* Not counted: it is a walk of the whole tree, and a dialog that
         * takes a second to open over HostFS is a dialog that looks stuck. */
        ag_strlcat(rows[2], "-", sizeof(rows[2]));
    } else {
        ag_strlcat(rows[2], ag_utoa(st.size, num, sizeof(num), 0, true),
                   sizeof(rows[2]));
        ag_strlcat(rows[2], " bytes", sizeof(rows[2]));
    }

    ag_strlcpy(rows[3], "Changed: ", sizeof(rows[3]));
    char when[48];
    format_date(st.mtime, when, sizeof(when));
    ag_strlcat(rows[3], when, sizeof(rows[3]));

    ag_strlcpy(rows[4], "Flags:  ", sizeof(rows[4]));
    if ((st.attr & AG_A_READONLY) != 0) {
        ag_strlcat(rows[4], "read-only ", sizeof(rows[4]));
    }
    if ((st.attr & AG_A_HIDDEN) != 0) {
        ag_strlcat(rows[4], "hidden ", sizeof(rows[4]));
    }
    if ((st.attr & AG_A_SYSTEM) != 0) {
        ag_strlcat(rows[4], "system ", sizeof(rows[4]));
    }
    if (rows[4][8] == '\0') {
        ag_strlcat(rows[4], "none", sizeof(rows[4]));
    }

    (void)dsk_dlg_lines("Properties", lines, 5, NULL, NULL);
}

static void ask_mkdir(void)
{
    if (dsk_folder_path(dsk_wm_active()) == NULL) {
        s_note = "no folder window is active";
        damage(s_m.statusbar);
        return;
    }
    (void)dsk_dlg_input("Create directory", "Name of the new directory:", "",
                        mkdir_typed, NULL);
}

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

/* The pattern item's label, held because a menu points at it rather than
 * copying it. */
static char s_pattern_label[32];
static char s_kbd_label[32];
static char s_dbl_label[32];
static char s_dim_label[32];

/*
 * When the last event arrived, and whether the backlight is off now.
 *
 * The shell does not know what the panel's backlight is wired to - that is
 * the driver's business, reached through gfx->backlight - so it keeps only
 * these two facts and tells the driver when they change.
 */
/*
 * What the screen is doing, and since when.
 *
 * Three states rather than two, because "the screen is off" is two
 * different things to the person in front of it.  Idle for the period the
 * Dim item names, and the desktop gives way to the clock; ten seconds more
 * and the light goes out.  Then the first touch brings the clock back - a
 * glance at a board on a desk should cost a tap and show the time, not open
 * whatever the finger landed on - and a touch while the clock is up is the
 * one that means "I want the desktop".
 *
 * The times are ours, the brightnesses are the panel's business: the shell
 * asks for a percentage and a board whose light is on a PWM channel will
 * show the clock dim, while this one, whose light is a transistor, shows it
 * lit.  Neither is written down here.
 */
typedef enum {
    SCREEN_LIVE,  /* the desktop                                          */
    SCREEN_SAVER, /* the clock, at SAVER_PCT                              */
    SCREEN_DARK   /* light out, controller asleep                         */
} screen_state_t;

#define SAVER_MS  10000u /* how long the clock stays up, either way round */
#define SAVER_PCT 20u    /* as dim as the panel can manage and still read */

static screen_state_t s_screen_state = SCREEN_LIVE;
static uint32_t s_screen_since; /* when this state began                  */
static uint32_t s_dim_since;    /* when the last input arrived            */
static uint32_t s_clock_at;     /* the minute the clock face is showing   */
static unsigned s_clock_nudge;
/*
 * Whether the clock will answer the next press.
 *
 * A tap is not one event: the press that lights the clock is followed a
 * moment later by its own release, and that release used to count as the
 * second tap - the clock flashed up and the desktop was back before the
 * finger had left the glass.  So a clock raised BY a touch is not armed
 * until that touch has ended, and only a press that begins afterwards is
 * somebody asking for the desktop.
 */
static bool     s_saver_armed;
static bool     s_no_light; /* asked once, and nothing on this board answered */

/*
 * Set the panel's brightness.  Returns true if anything took the request.
 *
 * Through the device, not through `gfx`.  `gfx->backlight` is in the ABI and
 * looks like the obvious call, but the kernel never fills it in - the entry
 * is NULL on every board, which is why the first version of this dimmed
 * nothing at all and said nothing about it either.  The light belongs to the
 * panel DRIVER, which is loadable, so it is asked the way the kernel's own
 * power path asks it: AG_IOC_DISPLAY_BACKLIGHT at every display in the
 * registry.  A driver whose panel has no controllable light answers
 * -AG_ENOTSUP, and that is an answer, not a failure.
 */
static bool set_backlight(uint8_t percent)
{
    bool taken = false;

    for (uint32_t i = 0;; i++) {
        ag_devinfo_t info;
        if (ag_dev_enumerate(i, AG_DEV_DISPLAY, &info) != AG_OK) {
            break;
        }
        const ag_handle_t h = ag_dev_open(info.name);
        if (h < 0) {
            continue;
        }
        uint8_t arg = percent;
        if (ag_dev_ioctl(h, AG_IOC_DISPLAY_BACKLIGHT, &arg, sizeof(arg)) ==
            AG_OK) {
            taken = true;
        }
        (void)ag_dev_close(h);
    }

    return taken;
}

/*
 * What the band callback needs, because it is handed a strip and nothing
 * else.  Worked out once per repaint rather than once per strip.
 */
static int  s_clock_h, s_clock_m;
static bool s_clock_valid;

static void draw_clock_banded(dsk_rect_t r)
{
    (void)r; /* the painter is already clipped to this strip */
    dsk_clock_draw(s_m.screen, s_clock_h, s_clock_m, s_clock_valid,
                   s_clock_nudge);
}

/* Paint the clock face for the minute `now` names. */
static void draw_clock(void)
{
    ag_datetime_t dt;
    bool          valid = false;

    if (AG_HAS(ag_api()->time, get_datetime) &&
        ag_api()->time->get_datetime != NULL &&
        ag_api()->time->get_datetime(&dt) == AG_OK) {
        /*
         * Before anything sets it the clock is seconds since 1970 plus
         * uptime, so it reads as a January morning in 1970.  That is not a
         * time, and drawing it as one would be a lie a person has to work
         * out for themselves.
         */
        valid = (dt.year >= 2000u);
    }

    /*
     * UTC plus the desk's offset, in minutes, wrapped into a day.  Done
     * here and not by moving the system clock: file timestamps, the network
     * and `date` all want the real thing.
     */
    int mins = 0;
    if (valid) {
        mins = (int)dt.hour * 60 + (int)dt.minute + (int)s_ini.tz_min;
        while (mins < 0) {
            mins += 24 * 60;
        }
        mins %= 24 * 60;
    }

    s_clock_h = mins / 60;
    s_clock_m = mins % 60;
    s_clock_valid = valid;

    dsk_cursor_hide();
    if (dsk_paint_banded()) {
        dsk_paint_region(s_m.screen, draw_clock_banded);
        dsk_paint_frame_done();
    } else {
        dsk_clock_draw(s_m.screen, s_clock_h, s_clock_m, s_clock_valid,
                       s_clock_nudge);
        dsk_flush(s_m.screen);
    }
}

static void enter_saver(uint32_t now, bool armed)
{
    s_screen_state = SCREEN_SAVER;
    s_screen_since = now;
    s_clock_at = now;
    s_saver_armed = armed;
    (void)set_backlight(SAVER_PCT);
    draw_clock();
}

static void enter_dark(uint32_t now)
{
    s_screen_state = SCREEN_DARK;
    s_screen_since = now;
    (void)set_backlight(0);
}

/* Back to the desktop from wherever we were. */
static void wake_screen(void)
{
    if (s_screen_state == SCREEN_LIVE) {
        return;
    }
    s_screen_state = SCREEN_LIVE;
    s_screen_since = ag_millis();
    (void)set_backlight(100);
    /*
     * And paint the whole thing again, because the glass does not hold what
     * was on it.  The clock painted over the desktop; and putting the light
     * out also puts the controller to sleep - that is the larger half of
     * the saving, a panel left scanning keeps its oscillator and charge
     * pumps running for a picture nobody can see - so a panel that has
     * slept wakes with its memory stale and the driver clears it.  Whoever
     * wakes it owes it a repaint.  Without this the desktop came back as an
     * empty screen that filled in wherever the pointer happened to pass.
     */
    s_repaints++;
    repaint_all();
}

/* Milliseconds until the screen state has to change, UINT32_MAX for never. */
static uint32_t screen_due_in(uint32_t now)
{
    if (s_no_light) {
        return UINT32_MAX;
    }

    if (s_screen_state == SCREEN_SAVER) {
        const uint32_t up = now - s_screen_since;
        const uint32_t left = (up >= SAVER_MS) ? 0u : SAVER_MS - up;
        /*
         * And a wake-up on the minute, so the face is never more than that
         * out of date.  Cheap: the saver is up for ten seconds, so this
         * costs at most one repaint of four digits.
         */
        const uint32_t shown = now - s_clock_at;
        const uint32_t tick = (shown >= 60000u) ? 0u : 60000u - shown;
        return (left < tick) ? left : tick;
    }

    if (s_screen_state == SCREEN_DARK || s_ini.dim_s == 0u) {
        return UINT32_MAX;
    }

    const uint32_t after = (uint32_t)s_ini.dim_s * 1000u;
    const uint32_t idle = now - s_dim_since;
    return (idle >= after) ? 0u : after - idle;
}

/*
 * An event has arrived: the board is not idle any more.
 *
 * Returns true when this one only moved the screen along and must go no
 * further.  That is the whole point of the two steps: a stylus aimed at a
 * dark screen is aimed at nothing, so the first touch buys the clock and
 * the second buys the desktop, and neither presses what happened to be
 * underneath.
 */
static bool wake_on_input(const ag_event_t *ev, uint32_t now)
{
    switch (ev->type) {
    case AG_EV_POINTER_MOVE:
    case AG_EV_POINTER_DOWN:
    case AG_EV_POINTER_UP:
    case AG_EV_WHEEL:
    case AG_EV_KEY_DOWN:
    case AG_EV_KEY_UP:
    case AG_EV_CHAR:
        break;
    default:
        return false;
    }

    s_dim_since = now;
    if (s_screen_state == SCREEN_LIVE) {
        return false;
    }

    /*
     * Only a press moves the screen along - never a move and never a
     * release.  A mouse nudged by the desk it sits on would otherwise light
     * the room, and on a touchscreen the driver reports a move before every
     * press and a release after it, so anything looser turns one tap into
     * two decisions.
     */
    const bool press = (ev->type == AG_EV_POINTER_DOWN) ||
                       (ev->type == AG_EV_KEY_DOWN) ||
                       (ev->type == AG_EV_CHAR) || (ev->type == AG_EV_WHEEL);

    if (s_screen_state == SCREEN_DARK) {
        if (press) {
            enter_saver(now, false); /* armed when this touch ends */
        }
        return true;
    }

    /* SCREEN_SAVER */
    if (!s_saver_armed) {
        if (ev->type == AG_EV_POINTER_UP || ev->type == AG_EV_KEY_UP) {
            s_saver_armed = true;
        }
        return true;
    }
    if (press) {
        wake_screen();
    }
    return true;
}

/* The idle periods the Dim item walks through, in seconds; 0 is never. */
static const uint16_t s_dims[] = { 0u, 60u, 300u, 900u };

static const char *dim_name(uint16_t s)
{
    return (s == 0u)    ? "never"
           : (s < 120u) ? "1 min"
           : (s < 600u) ? "5 min"
                        : "15 min";
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
    /*
     * Greyed out rather than hidden when there is nothing selected, so the
     * menu is the same shape every time it opens and the keys beside the
     * labels can be learnt from it.
     */
    const bool sel = dsk_folder_selected(active, NULL, 0, NULL, 0, NULL);
    const bool in_folder = dsk_folder_path(active) != NULL;
    /*
     * Marks count as a selection for the three that can act on many, and do
     * not for Rename, which cannot: there is no such thing as renaming four
     * files to one name.
     */
    const bool any = sel || dsk_folder_marked(active) > 0;
    set_item(&s_menus[0], "Copy...  F8", ID_COPY, any);
    set_item(&s_menus[0], "Move...  F7", ID_MOVE, any);
    set_item(&s_menus[0], "Rename...  F2", ID_RENAME, sel);
    set_item(&s_menus[0], "Delete  Del", ID_DELETE, any);
    set_separator(&s_menus[0]);
    set_item(&s_menus[0], "Create directory...", ID_MKDIR, in_folder);
    set_item(&s_menus[0], "Properties...", ID_PROPS,
             sel || (!in_folder && s_drive_sel >= 0));
    set_separator(&s_menus[0]);
    set_item(&s_menus[0], "Exit", ID_EXIT, true);

    /*
     * Edit, and why there is one at all.
     *
     * Copy... has been here from the start and it asks where to - which
     * means typing a path, on a board with no keyboard.  Cut, Copy and
     * Paste are the same operations asked for in the other order: pick
     * here, walk there, paste, and never spell anything.  That is the whole
     * of what makes file work possible with a finger.
     *
     * A menu of its own because File is full: sixteen items is the limit,
     * and File is at twelve without any of this.
     */
    s_menus[1].title = "Edit";
    s_menus[1].n = 0;
    set_item(&s_menus[1], "Cut  Ctrl+X", ID_CUT, any);
    set_item(&s_menus[1], "Copy  Ctrl+C", ID_CLIPCOPY, any);
    set_item(&s_menus[1], "Paste  Ctrl+V", ID_PASTE,
             in_folder && s_clip.n > 0);
    set_separator(&s_menus[1]);
    set_item(&s_menus[1], "Select all  Ctrl+A", ID_MARK_ALL, in_folder);
    set_item(&s_menus[1], "Clear marks", ID_MARK_NONE,
             dsk_folder_marked(active) > 0);

    s_menus[2].title = "Window";
    s_menus[2].n = 0;
    set_item(&s_menus[2], "Cascade", ID_CASCADE, n > 0);
    set_item(&s_menus[2], "Tile", ID_TILE, n > 0);
    set_separator(&s_menus[2]);
    set_item(&s_menus[2], "Close", ID_CLOSE, active != NULL);
    set_item(&s_menus[2], "Close all", ID_CLOSE_ALL, n > 0);
    set_separator(&s_menus[2]);
    set_item(&s_menus[2], "Arrange icons", ID_ARRANGE, s_ndrives > 0);
    set_separator(&s_menus[2]);
    /*
     * Under Window rather than under File: it opens a window onto something
     * the machine already has, which is what every other item here does.  What
     * it shows - and what it deliberately does not do yet - is in dsk_term.h.
     */
    set_item(&s_menus[2], "System console", ID_CONSOLE, true);
    if (n > 0) {
        set_separator(&s_menus[2]);
    }
    /* Topmost first, which is the order somebody looking at the screen sees. */
    for (int i = n - 1; i >= 0; i--) {
        dsk_win_t *w = dsk_wm_at(i);
        char      *label = s_win_labels[i];
        ag_strlcpy(label, w->title, DSK_TITLE_MAX + 4);
        set_item(&s_menus[2], label, (uint16_t)(ID_WINDOW_FIRST + i), true);
        s_menus[2].items[s_menus[2].n - 1].checked = (w == active);
    }

    s_menus[3].title = "Options";
    s_menus[3].n = 0;
    set_item(&s_menus[3], "Large font (8x16)", ID_FONT_LARGE, true);
    set_item(&s_menus[3], "Small font (8x8)", ID_FONT_SMALL, true);
    s_menus[3].items[0].checked = !s_ini.small_font;
    s_menus[3].items[1].checked = s_ini.small_font;
    set_separator(&s_menus[3]);
    set_item(&s_menus[3], "Teal desk", ID_BG_TEAL, true);
    set_item(&s_menus[3], "Navy desk", ID_BG_NAVY, true);
    set_item(&s_menus[3], "Green desk", ID_BG_GREEN, true);
    set_item(&s_menus[3], "Black desk", ID_BG_BLACK, true);
    s_menus[3].items[3].checked = (s_ini.background == DSK_TEAL);
    s_menus[3].items[4].checked = (s_ini.background == DSK_NAVY);
    s_menus[3].items[5].checked = (s_ini.background == DSK_GREEN);
    s_menus[3].items[6].checked = (s_ini.background == DSK_BLACK);
    /*
     * One item that cycles rather than five that do not fit: a menu holds
     * sixteen and this one is at fifteen.  The label says where it is now,
     * which is what a tick would have said.
     */
    ag_strlcpy(s_pattern_label, "Pattern: ", sizeof(s_pattern_label));
    ag_strlcat(s_pattern_label, dsk_pattern_name((int)s_ini.pattern),
               sizeof(s_pattern_label));
    set_item(&s_menus[3], s_pattern_label, ID_PATTERN, true);
    set_separator(&s_menus[3]);
    /*
     * Three settings, three items, not nine.  Each of these used to be a
     * row of alternatives with a tick beside the live one, which is the
     * Windows form and reads better - but the menu holds sixteen and those
     * nine filled it, so there was no room left for the fourth setting.
     * A cycling item says its value in its own label, which is what the
     * tick was for.
     */
    ag_strlcpy(s_kbd_label, "Keyboard: ", sizeof(s_kbd_label));
    ag_strlcat(s_kbd_label,
               (s_ini.keyboard == 1)   ? "always"
               : (s_ini.keyboard == 2) ? "never"
                                       : "automatic",
               sizeof(s_kbd_label));
    set_item(&s_menus[3], s_kbd_label, ID_KBD_CYCLE, true);

    ag_strlcpy(s_dbl_label, "Double click: ", sizeof(s_dbl_label));
    ag_strlcat(s_dbl_label,
               (s_ini.dblclick_ms >= 600u)  ? "slow"
               : (s_ini.dblclick_ms > 300u) ? "normal"
                                            : "fast",
               sizeof(s_dbl_label));
    set_item(&s_menus[3], s_dbl_label, ID_DBL_CYCLE, true);

    ag_strlcpy(s_dim_label, "Dim: ", sizeof(s_dim_label));
    ag_strlcat(s_dim_label, dim_name(s_ini.dim_s), sizeof(s_dim_label));
    set_item(&s_menus[3], s_dim_label, ID_DIM, true);

    s_menus[4].title = "Help";
    s_menus[4].n = 0;
    set_item(&s_menus[4], "About...", ID_ABOUT, true);

    /*
     * The fifth is the context menu: filled when somebody asks for it and
     * never drawn on the bar (dsk_menu_t::hidden).
     */
    s_menus[5].hidden = true;
    dsk_menu_set(s_menus, 6);
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
    case ID_COPY:
        ask_copy(false);
        break;
    case ID_MOVE:
        ask_copy(true);
        break;
    case ID_RENAME:
        ask_rename();
        break;
    case ID_DELETE:
        ask_delete();
        break;
    case ID_OPEN:
        (void)dsk_folder_open_sel(dsk_wm_active());
        break;
    case ID_MKDIR:
        ask_mkdir();
        break;
    case ID_PROPS:
        /*
         * Whichever thing is picked.  A folder window's selection wins over a
         * drive icon, because the window is in front of the icon and that is
         * what "the thing I am looking at" means.
         */
        if (dsk_folder_path(dsk_wm_active()) != NULL) {
            show_file_props();
        } else {
            show_drive_props(s_drive_sel);
        }
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
    case ID_ARRANGE:
        arrange_icons();
        break;
    case ID_CONSOLE:
        if (dsk_term_open(&s_m) == NULL) {
            s_note = "no room for another window";
            damage(s_m.statusbar);
        } else {
            /*
             * Said on the console, which is also what the window now shows -
             * so the line appears inside it, which is a demonstration as well
             * as a trace.  A script driving this shell cannot read pixels, and
             * "the window opened" is otherwise unobservable from outside.
             */
            ag_printf("desktop: console window opened\n");
        }
        break;
    case ID_CUT:
        clip_take(true);
        break;
    case ID_CLIPCOPY:
        clip_take(false);
        break;
    case ID_PASTE:
        clip_paste();
        break;
    case ID_MARK:
        dsk_folder_mark(dsk_wm_active(), -1, DSK_MARK_TOGGLE);
        break;
    case ID_MARK_ALL:
        dsk_folder_mark(dsk_wm_active(), -1, DSK_MARK_ALL);
        break;
    case ID_MARK_NONE:
        dsk_folder_mark(dsk_wm_active(), -1, DSK_MARK_NONE);
        break;
    case ID_PATTERN:
        s_ini.pattern = (uint8_t)((s_ini.pattern + 1u) % DSK_PATTERNS);
        repaint_all();
        save_arrangement();
        ag_printf("desktop: desk pattern %u (%s)\n",
                  (unsigned)s_ini.pattern,
                  dsk_pattern_name((int)s_ini.pattern));
        break;
    case ID_KBD_CYCLE:
        s_ini.keyboard = (uint8_t)((s_ini.keyboard + 1u) % 3u);
        apply_keyboard();
        save_arrangement();
        break;
    case ID_DIM: {
        unsigned i = 0;
        while (i + 1u < sizeof(s_dims) / sizeof(s_dims[0]) &&
               s_dims[i] != s_ini.dim_s) {
            i++;
        }
        s_ini.dim_s = s_dims[(i + 1u) % (sizeof(s_dims) / sizeof(s_dims[0]))];
        s_dim_since = ag_millis();
        wake_screen();
        s_no_light = false; /* a fresh ask: the answer may have changed */
        save_arrangement();
        ag_printf("desktop: dim after %s\n", dim_name(s_ini.dim_s));
        break;
    }
    case ID_FONT_LARGE:
    case ID_FONT_SMALL: {
        const bool small = (id == ID_FONT_SMALL);
        if (small != s_ini.small_font) {
            s_ini.small_font = small;
            apply_font();
        }
        break;
    }
    case ID_BG_TEAL:
    case ID_BG_NAVY:
    case ID_BG_GREEN:
    case ID_BG_BLACK: {
        const uint32_t bg = (id == ID_BG_TEAL)    ? DSK_TEAL
                            : (id == ID_BG_NAVY)  ? DSK_NAVY
                            : (id == ID_BG_GREEN) ? DSK_GREEN
                                                  : DSK_BLACK;
        if (bg != s_ini.background) {
            s_ini.background = bg;
            repaint_all();
            save_arrangement();
        }
        break;
    }
    case ID_DBL_CYCLE:
        s_ini.dblclick_ms = (s_ini.dblclick_ms >= 600u)  ? 400u
                            : (s_ini.dblclick_ms > 300u) ? 250u
                                                         : 700u;
        save_arrangement();
        break;
    case ID_ABOUT: {
        /*
         * The version of the SYSTEM, not of this shell: a shell that reports
         * its own number tells you nothing you cannot see, and the number
         * anyone actually needs when something is wrong is the one the kernel
         * was built from.
         */
        ag_sysinfo_t si;
        ag_sysinfo_get(&si);

        char rows[4][64];
        const char *lines[4] = {rows[0], rows[1], rows[2], rows[3]};

        ag_strlcpy(rows[0], "ArgonOS desktop", sizeof(rows[0]));

        ag_strlcpy(rows[1], "System ", sizeof(rows[1]));
        ag_strlcat(rows[1], si.os_version, sizeof(rows[1]));
        ag_strlcat(rows[1], " (", sizeof(rows[1]));
        ag_strlcat(rows[1], si.build, sizeof(rows[1]));
        ag_strlcat(rows[1], ")", sizeof(rows[1]));

        char num[24];
        ag_strlcpy(rows[2], si.chip, sizeof(rows[2]));
        ag_strlcat(rows[2], ", ABI ", sizeof(rows[2]));
        ag_strlcat(rows[2], ag_utoa(si.abi_major, num, sizeof(num), 0, false),
                   sizeof(rows[2]));
        ag_strlcat(rows[2], ".", sizeof(rows[2]));
        ag_strlcat(rows[2], ag_utoa(si.abi_minor, num, sizeof(num), 0, false),
                   sizeof(rows[2]));

        ag_meminfo_t mem;
        ag_meminfo(&mem);
        ag_strlcpy(rows[3], "Arena ", sizeof(rows[3]));
        ag_strlcat(rows[3],
                   ag_utoa(mem.arena_free / 1024u, num, sizeof(num), 0, true),
                   sizeof(rows[3]));
        ag_strlcat(rows[3], " KB free of ", sizeof(rows[3]));
        ag_strlcat(rows[3],
                   ag_utoa(mem.arena_total / 1024u, num, sizeof(num), 0, true),
                   sizeof(rows[3]));
        ag_strlcat(rows[3], " KB", sizeof(rows[3]));

        (void)dsk_dlg_lines("About", lines, 4, about_done, NULL);
        break;
    }
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

    /*
     * With -lag the two numbers come first and the rest of the line goes.
     *
     * Forty columns is the whole of this screen, and a diagnostic pushed off
     * the right-hand edge is not a diagnostic: Maxim could not read the
     * numbers he was asked to read.
     */
    if (s_lag) {
        ag_strlcpy(line, "p", sizeof(line));
        ag_strlcat(line, ag_utoa((uint64_t)s_worst_paint, num, sizeof(num), 0,
                                 false),
                   sizeof(line));
        ag_strlcat(line, "ms g", sizeof(line));
        ag_strlcat(line, ag_utoa((uint64_t)s_worst_gap, num, sizeof(num), 0,
                                 false),
                   sizeof(line));
        ag_strlcat(line, "ms  ", sizeof(line));
        ag_strlcat(line, ag_utoa((uint64_t)s_ptr_events, num, sizeof(num), 0,
                                 false),
                   sizeof(line));
        ag_strlcat(line, " ev", sizeof(line));
        dsk_text_small((int16_t)(s_m.statusbar.x + 4), ty, line, DSK_BLACK,
                       DSK_LGRAY);
        return;
    }

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
    draw_desk();
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
/*
 * How long the slowest frame took, and how long the slowest flush inside it.
 *
 * The worst one, not the average, and that is the whole point: an average
 * frame rate on a screen driven over SPI says nothing about what a person
 * sees, because what they see is the frame that stuttered.  Kept in
 * microseconds because on this board the interesting ones are tens of
 * thousands of them.
 */
static uint32_t s_worst_frame_us;
static uint32_t s_worst_flush_us;
static uint32_t s_frames;
static uint64_t s_frame_total_us;

/*
 * One strip, complete: everything the shell has, in the order it stacks.
 *
 * The pointer and the drag outline read the background, so in band mode they
 * have to be drawn here - while the strip they read from is still the strip
 * being built - rather than saved and restored around the frame the way the
 * surface path does it.
 */
static void draw_banded(dsk_rect_t r)
{
    draw_region(r);
    dsk_wm_outline_paint();
    dsk_cursor_paint();
}

static void commit(void)
{
    if (s_damage.n == 0) {
        return;
    }
    const uint32_t t0 = (uint32_t)ag_micros();
    dsk_damage_clip(&s_damage, s_m.screen);

    uint32_t t_flush;
    if (dsk_paint_banded()) {
        /*
         * No frame, so nothing is taken off and put back: each strip is drawn
         * complete - furniture, outline, pointer - and goes to the panel as
         * it is finished.  The flush stopwatch starts with the first strip
         * because in this mode drawing and sending are not separable.
         */
        t_flush = (uint32_t)ag_micros();
        for (uint8_t i = 0; i < s_damage.n; i++) {
            dsk_paint_region(s_damage.r[i], draw_banded);
        }
        /* All of it is one frame, and saying so is worth a lot: see
         * dsk_bander_t::frame_done. */
        dsk_paint_frame_done();
    } else {
        /*
         * Pointer off, then outline off, then paint, then both back on in the
         * other order - because the pointer is above the outline and each of
         * them holds a copy of what it covered.
         */
        dsk_cursor_hide();
        dsk_wm_outline_off();
        for (uint8_t i = 0; i < s_damage.n; i++) {
            draw_region(s_damage.r[i]);
        }
        dsk_wm_outline_on();
        dsk_cursor_show();

        /* The pointer's square needs a flush though it needed no repaint. */
        dsk_damage_add(&s_damage, dsk_cursor_rect());
        t_flush = (uint32_t)ag_micros();
        for (uint8_t i = 0; i < s_damage.n; i++) {
            dsk_flush(s_damage.r[i]);
        }
    }
    const uint32_t now = (uint32_t)ag_micros();

    const uint32_t flush_us = now - t_flush;
    const uint32_t frame_us = now - t0;
    if (flush_us > s_worst_flush_us) {
        s_worst_flush_us = flush_us;
    }
    if (frame_us > s_worst_frame_us) {
        s_worst_frame_us = frame_us;
    }
    s_frames++;
    s_frame_total_us += frame_us;

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

/*
 * -ptr: every pointer event on the console, as it arrives.
 *
 * A board is where the pointer actually is, and a board is exactly where
 * there is no way to watch one: the touchscreen cannot be scripted and the
 * screen cannot be photographed.  Two lines of print are the difference
 * between knowing what the finger sent and guessing from what did not happen.
 */
static bool s_ptr_log;

static void on_pointer(dsk_ptr_t type, int16_t x, int16_t y, uint8_t buttons,
                       uint32_t now)
{
    s_ptr_events++;
    if (s_ptr_log) {
        ag_printf("ptr %s %d,%d b%u -> drive %d\n",
                  (type == DSK_PTR_DOWN)   ? "down"
                  : (type == DSK_PTR_UP)   ? "up"
                                           : "move",
                  (int)x, (int)y, (unsigned)buttons, drive_at(x, y));
    }
    if (s_last_ptr != 0u) {
        const uint32_t gap = now - s_last_ptr;
        if (gap > s_worst_gap) {
            s_worst_gap = gap;
        }
    }
    s_last_ptr = now;

    s_buttons = buttons;
    if (dsk_paint_banded()) {
        damage(dsk_cursor_place_moved(x, y));
    } else {
        dsk_cursor_move(x, y);
    }
    s_status_dirty = true;

    const bool dbl = (type == DSK_PTR_DOWN) ? is_double(now, x, y) : false;

    /*
     * A drag in progress comes before everything, including the menu.
     *
     * Not tidiness: an icon dragged across a window would otherwise have its
     * movements taken by that window - which would scroll a list while
     * somebody is moving an icon over it, and then drop the icon back where it
     * started because the up-click never arrived here either.
     */
    /*
     * Anything but the press itself ends the hold, and this is asked before
     * the drag branch below rather than after it: that branch takes every
     * move while an icon is being dragged and returns, so a hold judged
     * after it never saw the hand move.  The context menu duly popped up in
     * the middle of dragging an icon, which is the one moment it must not.
     */
    if (type != DSK_PTR_DOWN || (buttons & 1u) == 0) {
        s_press.armed = false;
    }
    if (s_drag.which >= 0) {
        if (type == DSK_PTR_MOVE && (buttons & 1u) != 0) {
            drag_icon_to(x, y);
            return;
        }
        if (type == DSK_PTR_UP || (buttons & 1u) == 0) {
            if (s_ptr_log) {
                ag_printf("ptr    ended an icon drag\n");
            }
            drag_icon_end();
            return;
        }
    }

    /*
     * A rubber band owns the pointer while it is being drawn, and for the
     * same reason the icon drag above does: the manager routes by what is
     * under the pointer, so a hand that wanders off the window - up over
     * the caption, sideways onto the desk - would stop the band dead with
     * its marks half made, and the release would land somewhere else
     * entirely and never end it.  Maxim found it by dragging one way and
     * then back: the highlight would not come off.
     */
    if (type != DSK_PTR_DOWN) {
        dsk_win_t *bw = dsk_folder_banding();
        if (bw != NULL) {
            (void)dsk_folder_band_event(bw, type, x, y, buttons);
            if (s_ptr_log) {
                ag_printf("ptr    taken by a rubber band\n");
            }
            return;
        }
    }

    /* The menu is above everything else, so it is asked next. */
    if (dsk_menu_pointer(type, x, y)) {
        if (s_ptr_log) {
            ag_printf("ptr    taken by the menu\n");
        }
        return;
    }
    /*
     * A drag out of a folder window is watched here, before the windows see
     * the event: the release of one is the drop and belongs to nobody else,
     * while everything up to that point is passed on so that a press which
     * turns out to be a click still reaches the row it was on.
     */
    if (fdrag_pointer(type, x, y, buttons)) {
        if (s_ptr_log) {
            ag_printf("ptr    taken by a file drag\n");
        }
        return;
    }
    if (type == DSK_PTR_DOWN && (buttons & 2u) != 0) {
        (void)dsk_wm_pointer(type, x, y, buttons, false); /* pick what is under it */
        context_menu_at(x, y);
        return;
    }
    if (dsk_wm_pointer(type, x, y, buttons, dbl)) {
        if (s_ptr_log) {
            ag_printf("ptr    taken by a window\n");
        }
        if (type == DSK_PTR_DOWN && !dbl) {
            fdrag_arm(x, y);
            s_press.armed = true;
            s_press.x = x;
            s_press.y = y;
            s_press.at = now;
        }
        return;
    }
    if (type == DSK_PTR_DOWN && !dbl) {
        /* On the desk, where no window took it: the hold still counts. */
        s_press.armed = true;
        s_press.x = x;
        s_press.y = y;
        s_press.at = now;
    }
    /* Nothing wanted it: the click was on the desktop itself. */
    if (type == DSK_PTR_UP) {
        if (s_ptr_log) {
            ag_printf("ptr    nobody wanted the release\n");
        }
        return;
    }
    if (type != DSK_PTR_DOWN) {
        return;
    }
    if (s_ptr_log) {
        ag_printf("ptr    fell through to the desk\n");
    }
    const int d = drive_at(x, y);
    if (dbl) {
        if (d >= 0 && dsk_folder_open(s_drives[d].path) == NULL) {
            s_note = "no room for another window";
            damage(s_m.statusbar);
        }
        return;
    }

    /* A single click picks a drive, or unpicks whatever was picked. */
    if (d != s_drive_sel) {
        const int was = s_drive_sel;
        s_drive_sel = d;
        if (was >= 0) {
            damage(drive_rect(was));
        }
        if (d >= 0) {
            damage(drive_rect(d));
        }
    }

    /* And arms a drag, which the next movement with the button down starts. */
    if (d >= 0) {
        s_drag.which = d;
        s_drag.dx = (int16_t)(x - drive_rect(d).x);
        s_drag.dy = (int16_t)(y - drive_rect(d).y);
        s_drag.moved = false;
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

    /*
     * The file operation keys, and they are Windows 3.11's File Manager ones:
     * F7 moves, F8 copies, Delete deletes.  F5 is already refresh, there and
     * here.  Rename had no key there and F2 is what every manager since has
     * used, so it is F2.
     *
     * Ahead of the windows rather than behind them, because a folder window
     * would otherwise have to know about operations to pass them on - and
     * these apply to whatever is selected in the active window, which is a
     * question about the shell and not about one window.
     */
    /*
     * The clipboard chords, ahead of the windows for the same reason the
     * function keys are: they are about the shell's clipboard and the
     * active window, not about one window's own keys.  Ctrl+A is NOT here -
     * that one is a folder window's business, because only the window knows
     * which rows it has.
     */
    if (!dsk_dlg_up() && !dsk_menu_open() && (mods & DSK_MOD_CTRL) != 0) {
        if (unicode == 'x' || unicode == 'X') {
            clip_take(true);
            return;
        }
        if (unicode == 'c' || unicode == 'C') {
            clip_take(false);
            return;
        }
        if (unicode == 'v' || unicode == 'V') {
            clip_paste();
            return;
        }
    }

    if (!dsk_dlg_up() && !dsk_menu_open()) {
        switch (keycode) {
        case AG_KEY_F7:
            ask_copy(true);
            return;
        case AG_KEY_F8:
            ask_copy(false);
            return;
        case AG_KEY_F2:
            ask_rename();
            return;
        case AG_KEY_DELETE:
            ask_delete();
            return;
        default:
            break;
        }
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

/*
 * The board measurement, with nobody at the desk.
 *
 * On a board there is no way to script a finger: the pointer is a resistive
 * panel and the virtual input drivers need the radio, which on this board
 * costs the UART buffers.  So the shell drives itself through the three
 * things worth timing and prints what they cost.
 *
 * It is NOT a substitute for a finger and does not pretend to be: what it
 * measures is what the SHELL costs - opening a directory, moving a window,
 * repainting everything - with the touch latency left out, because that
 * belongs to the panel and not to this.  A number that includes both would
 * hide which half it came from.
 */
static void bench(void)
{
    char     line[96];
    char     num[24];
    uint32_t t0;

    ag_printf("desktop: bench on a %dx%d surface\n", (int)s_m.screen_w,
              (int)s_m.screen_h);

    /* 1. A window onto C:\ - the read, the layout and the first paint. */
    t0 = (uint32_t)ag_micros();
    dsk_win_t *w = dsk_folder_open("c:\\");
    const uint32_t open_us = (uint32_t)ag_micros() - t0;
    if (w == NULL) {
        ag_printf("desktop: bench: no window\n");
        return;
    }
    t0 = (uint32_t)ag_micros();
    commit();
    const uint32_t first_paint_us = (uint32_t)ag_micros() - t0;

    /* 2. The window moved across the work area, one step at a time, painting
     *    every step - which is what a drag costs once the outline is off. */
    s_worst_frame_us = 0;
    s_worst_flush_us = 0;
    s_frames = 0;
    s_frame_total_us = 0;

    const dsk_rect_t start = w->frame;
    const int16_t    span = (int16_t)(s_m.work.w - start.w);
    for (int i = 1; i <= 16; i++) {
        dsk_rect_t f = start;
        f.x = (int16_t)(s_m.work.x + (span > 0 ? span * i / 16 : 0));
        dsk_wm_move(w, f);
        commit();
    }
    const uint32_t drag_worst = s_worst_frame_us;
    const uint32_t drag_flush = s_worst_flush_us;
    const uint32_t drag_mean =
        (s_frames > 0) ? (uint32_t)(s_frame_total_us / s_frames) : 0;

    /* 3. Everything, once - what F5 costs, and the floor for any full repaint. */
    s_worst_frame_us = 0;
    damage(s_m.screen);
    commit();
    const uint32_t full_us = s_worst_frame_us;

    ag_strlcpy(line, "desktop: open c: ", sizeof(line));
    ag_strlcat(line, ag_utoa(open_us / 1000u, num, sizeof(num), 0, false),
               sizeof(line));
    ag_strlcat(line, " ms, first paint ", sizeof(line));
    ag_strlcat(line, ag_utoa(first_paint_us / 1000u, num, sizeof(num), 0, false),
               sizeof(line));
    ag_strlcat(line, " ms, full repaint ", sizeof(line));
    ag_strlcat(line, ag_utoa(full_us / 1000u, num, sizeof(num), 0, false),
               sizeof(line));
    ag_strlcat(line, " ms", sizeof(line));
    ag_printf("%s\n", line);

    ag_strlcpy(line, "desktop: drag worst ", sizeof(line));
    ag_strlcat(line, ag_utoa(drag_worst / 1000u, num, sizeof(num), 0, false),
               sizeof(line));
    ag_strlcat(line, " ms, mean ", sizeof(line));
    ag_strlcat(line, ag_utoa(drag_mean / 1000u, num, sizeof(num), 0, false),
               sizeof(line));
    ag_strlcat(line, " ms, worst flush ", sizeof(line));
    ag_strlcat(line, ag_utoa(drag_flush / 1000u, num, sizeof(num), 0, false),
               sizeof(line));
    ag_strlcat(line, " ms over 16 steps", sizeof(line));
    ag_printf("%s\n", line);

    dsk_wm_close(w);
    damage(s_m.screen);
    commit();
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
    /*
     * No system surface: the pixels are the shell's own and go out in bands
     * through gfx->present (dsk_paint_band.c, docs/plans/desktop.md §9.2).
     * This is how a 320x240 screen happens on a board that cannot hold a
     * frame of it - the CYD, which is also the only board with a touchscreen.
     */
    const bool banded = (info.fb == NULL);

    s_double_buf = banded ? false : info.double_buf;

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
    /*
     * One grammar for both backends, with the mode as the third field:
     * "single", "double" or "bands".  A script that watches for this line -
     * apps/desktop/check.ps1 does, and it is the first checkable thing in the
     * whole transcript - then needs no second rule to recognise the other
     * kind of run.
     */
    ag_printf("desktop: surface %ux%u %s, focus %s, font 8x%d\n",
              (unsigned)info.width, (unsigned)info.height,
              banded ? "bands" : (info.double_buf ? "double" : "single"),
              ag_focused() ? "yes" : "no", (int)dsk_ui_h());

    dsk_metrics_init(&s_m, (int16_t)info.width, (int16_t)info.height);
    if (banded) {
        dsk_paint_bind_band(s_m.screen_w, s_m.screen_h);
    } else {
        dsk_paint_bind_surface(info.fb, info.stride, s_m.screen_w,
                               s_m.screen_h);
    }
    dsk_cursor_init(s_m.screen_w, s_m.screen_h, damage);
    dsk_wm_init(&s_m, damage);
    dsk_menu_init(&s_m, damage, menu_chose);
    dsk_dlg_init(&s_m);
    apply_keyboard();
    dsk_folder_init(&s_m, open_from_folder);
    dsk_run_init(repaint_all);
    dsk_ops_init(&s_m, repaint_all);

    /*
     * The arrangement is read BEFORE the drives are found and the windows
     * opened: the icon positions decide where find_drives puts them, and the
     * background colour decides what the first paint paints.
     */
    dsk_ini_defaults(&s_ini);
    dsk_ini_load(&s_ini);

    /*
     * Which font the shell draws in: 8x16 unless DESKTOP.INI asks for 8x8.
     *
     * It was chosen by screen size for a day, small below four hundred
     * pixels, and that was wrong in the way only the person looking can
     * settle: on the CYD the small font is readable and the big one is
     * nicer, so twice the lines is a thing to ask for rather than to be
     * given.  `font = small` in [desktop] asks.
     *
     * The layout is rebuilt because every height in it is one line of this
     * font plus a pixel each side; s_m is filled in place, and the window
     * manager and the menus hold a pointer to it rather than a copy.
     */
    if (s_ini.small_font) {
        dsk_ui_font_set(DSK_UI_FONT_SMALL);
        dsk_metrics_init(&s_m, (int16_t)info.width, (int16_t)info.height);
    }
    find_drives();
    restore_windows();
    rebuild_menus();

    /* First paint: everything, once. */
    dsk_damage_clear(&s_damage);
    damage(s_m.screen);
    commit();

    for (int i = 1; i < argc; i++) {
        if (ag_stricmp(argv[i], "-ptr") == 0) {
            s_ptr_log = true;
        }
        if (ag_stricmp(argv[i], "-lag") == 0) {
            s_lag = true;
        }
    }
    for (int i = 1; i < argc; i++) {
        if (ag_stricmp(argv[i], "-bench") == 0) {
            bench();
        }
    }

    const uint32_t deadline_s = parse_seconds(argc, argv);
    const uint32_t started = ag_millis();

    s_status_at = started;
    s_dim_since = started;
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
        /*
         * The console window polls, because the console changes without
         * telling anybody: a driver logs, a background process prints.  With
         * the window shut this asks for nothing and the wait stays as long as
         * it was.
         */
        wait = soonest(wait, dsk_term_due_in(now));
        wait = soonest(wait, press_due_in(now));
        wait = soonest(wait, screen_due_in(now));
        if (screen_due_in(now) == 0u) {
            if (s_screen_state == SCREEN_SAVER) {
                if (now - s_screen_since >= SAVER_MS) {
                    enter_dark(now);
                } else {
                    s_clock_at = now;
                    s_clock_nudge++;
                    draw_clock();
                }
            } else {
                /*
                 * Only start this if something can actually be switched
                 * off at the end of it.  A panel whose driver takes no
                 * brightness would get the clock and keep it for ever, and
                 * the next touch would be eaten waking a screen that was
                 * never out.  Ask once, say so once, and leave it alone.
                 */
                if (set_backlight(SAVER_PCT)) {
                    /*
                     * Nobody is touching anything - this one came from the
                     * idle timer, so the next press is a real answer.
                     */
                    enter_saver(now, true);
                } else {
                    s_no_light = true;
                    ag_printf("desktop: this panel has no backlight to dim\n");
                }
            }
        }
        ag_event_t ev;

        if (deadline_s != 0u) {
            const uint32_t elapsed = now - started;
            const uint32_t total = deadline_s * 1000u;
            wait = soonest(wait, (elapsed >= total) ? 0u : total - elapsed);
        }

        if (next_event(&ev, s_have_held ? 0u : wait)) {
            now = ag_millis();
            if (wake_on_input(&ev, now)) {
                continue;
            }
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
        /*
         * Nothing is painted while the clock is up or the light is out.
         *
         * Not an optimisation - a correctness rule.  The desktop paints
         * through a damage list, and a caret blinking or a status strip
         * ticking behind the clock would put desktop pixels on a screen
         * that is showing something else.  The damage keeps accumulating
         * and is paid off in one repaint when we come back.
         */
        if (s_screen_state != SCREEN_LIVE) {
            continue;
        }

        dsk_dlg_tick(now);
        status_settle(now);
        press_settle(now);
        if (dsk_term_due_in(now) == 0u) {
            dsk_term_tick();
        }
        const uint32_t paint_at = ag_millis();
        commit();
        const uint32_t took = ag_millis() - paint_at;
        if (took > s_worst_paint) {
            s_worst_paint = took;
        }
        if (paint_at - s_lag_since >= 3000u) {
            s_lag_since = paint_at;
            s_worst_paint = 0;
            s_worst_gap = 0;
        }
        if (s_lag) {
            s_status_dirty = true; /* the numbers are only worth a live strip */
        }
    }

    dsk_cursor_hide();
    /* Never hand the screen back dark: the next program cannot light it. */
    wake_screen();
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

    /*
     * Last, and after the counters have been printed: the counters are what a
     * scripted run reads, and a write to flash that goes wrong must not be
     * what stops them appearing.
     */
    save_arrangement();
    return 0;
}
