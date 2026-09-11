/*
 * ArgonOS DESKTOP - the console, in a window.  See dsk_term.h for why.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_term.h"

#include <argon/libc.h>

#include <argon/argon.h>

#include "dsk_paint.h"

/*
 * What the view can hold.
 *
 * Eighty by thirty covers both consoles this system has: 80x25 on a 640x400
 * surface and 40x30 on the CYD's glass.  A wider console than this is shown
 * from its left edge rather than refused - a view that says "too wide" is
 * worth less than a view of most of it.
 *
 * The shadow copy is what makes the redraw small: a console changes one line
 * at a time, and comparing against what was drawn is the only way to know
 * which line that was, because the kernel publishes no generation count.
 * 4.8 KB of bss buys repainting one row instead of a window.
 */
#define TERM_COLS_MAX 80
#define TERM_ROWS_MAX 30

/* Ten times a second: faster than anyone reads, slower than anything costs. */
#define TERM_POLL_MS 100u

/*
 * One of these per open window, because there is more than one kind.
 *
 * The system console is a VIEW: it shows what the machine said and takes no
 * keys, because there is one keyboard and this desktop is holding it.  A
 * prompt is somebody's own shell in a session slot, and every key it is
 * given is passed straight through to that slot.  They are different
 * windows and they can be open at once, which is why none of this is a
 * file-wide static any more: it was, and opening the prompt silently took
 * over the console's window instead of opening its own.
 */
typedef struct {
    dsk_win_t    *win;
    int           slot;        /* below zero: the system console, as a view */
    ag_textcell_t shadow[TERM_ROWS_MAX][TERM_COLS_MAX];
    uint16_t      shown_rows;  /* rows the window has space for  */
    uint16_t      shown_cols;
    uint16_t      first_row;   /* which console row is at the top */
    uint32_t      polled_at;
    bool          polled;
    uint16_t      caret_x;
    uint16_t      caret_y;
} term_t;

#define TERM_MAX 2 /* the console view and one prompt; 4.8 KB each */

static const dsk_metrics_t *s_m;
static term_t               s_terms[TERM_MAX];

/* Keys handed to a prompt, and keys the slot took: pixels otherwise. */
static uint32_t             s_fwd;
static uint32_t             s_fwd_ok;

static term_t *term_of(const dsk_win_t *w)
{
    for (int i = 0; i < TERM_MAX; i++) {
        if (s_terms[i].win == w) {
            return &s_terms[i];
        }
    }
    return NULL;
}

/*
 * The CGA sixteen, as this shell's palette.
 *
 * The console's attribute byte is bg << 4 | fg, and those numbers mean the
 * BIOS colours - so a console window that invented its own would not look
 * like the console it is showing.  Mapped to the desktop's own constants
 * where they are the same colour, which they mostly are.
 */
static const uint32_t k_cga[16] = {
    DSK_BLACK, DSK_NAVY,  DSK_GREEN,   DSK_TEAL,
    DSK_MAROON, DSK_PURPLE, DSK_OLIVE, DSK_LGRAY,
    DSK_DGRAY, DSK_BLUE,  DSK_LIME,    DSK_CYAN,
    DSK_RED,   DSK_MAGENTA, DSK_YELLOW, DSK_WHITE,
};

static dsk_rect_t row_rect(const term_t *t, dsk_rect_t client, uint16_t row)
{
    return dsk_rect(client.x, (int16_t)(client.y + row * dsk_ui_h()),
                    (int16_t)(t->shown_cols * dsk_ui_w()), dsk_ui_h());
}

/*
 * One row of cells, as runs of one attribute.
 *
 * Run by run rather than cell by cell: a line of console text is usually one
 * attribute from end to end, and handing sixty glyphs to the painter in one
 * call instead of sixty is the difference between a window that repaints in
 * one millisecond and one that does not.
 */
static void draw_row(const term_t *t, dsk_rect_t client, uint16_t row)
{
    const ag_textcell_t *cells = t->shadow[row];
    const int16_t        y = (int16_t)(client.y + row * dsk_ui_h());
    uint16_t             at = 0;

    while (at < t->shown_cols) {
        const uint8_t attr = cells[at].attr;
        uint16_t      end = at;
        char          run[TERM_COLS_MAX + 1];

        while (end < t->shown_cols && cells[end].attr == attr) {
            /*
             * A cell holds a code page byte, and a zero is what an untouched
             * cell holds; the font has a glyph at zero and it is not a space.
             */
            run[end - at] = (cells[end].ch == 0) ? ' ' : (char)cells[end].ch;
            end++;
        }
        run[end - at] = '\0';

        dsk_text((int16_t)(client.x + at * dsk_ui_w()), y, run,
                 k_cga[attr & 0x0Fu], k_cga[(attr >> 4) & 0x0Fu]);
        at = end;
    }
}

static void term_draw(dsk_win_t *w, dsk_rect_t client)
{
    const term_t *t = term_of(w);
    if (t == NULL) {
        return;
    }

    /* The margins the cells do not cover, so the window has no stale edges. */
    const int16_t used_w = (int16_t)(t->shown_cols * dsk_ui_w());
    const int16_t used_h = (int16_t)(t->shown_rows * dsk_ui_h());
    if (client.w > used_w) {
        dsk_fill(dsk_rect((int16_t)(client.x + used_w), client.y,
                          (int16_t)(client.w - used_w), client.h),
                 DSK_BLACK);
    }
    if (client.h > used_h) {
        dsk_fill(dsk_rect(client.x, (int16_t)(client.y + used_h), client.w,
                          (int16_t)(client.h - used_h)),
                 DSK_BLACK);
    }

    for (uint16_t row = 0; row < t->shown_rows; row++) {
        if (!dsk_visible(row_rect(t, client, row))) {
            continue;
        }
        draw_row(t, client, row);
    }

    /*
     * The caret, as the console's own block rather than a shell decoration:
     * this window is a view of a text screen, and where that screen's cursor
     * is is part of what it says.
     */
    /*
     * Only for the view.  ag_coninfo answers about THIS task's console, and
     * a prompt window is showing another slot's screen: there is no way to
     * ask where that screen's cursor is, so drawing one here would be
     * drawing our own cursor on somebody else's text.
     */
    if (t->slot >= 0) {
        return;
    }
    ag_coninfo_t info;
    ag_coninfo(&info);
    if (info.cur_y >= t->first_row && info.cur_y < t->first_row + t->shown_rows &&
        info.cur_x < t->shown_cols) {
        const dsk_rect_t caret =
            dsk_rect((int16_t)(client.x + info.cur_x * dsk_ui_w()),
                     (int16_t)(client.y +
                               (info.cur_y - t->first_row) * dsk_ui_h() +
                               dsk_ui_h() - 2),
                     dsk_ui_w(), 2);
        if (dsk_visible(caret)) {
            dsk_fill(caret, DSK_LGRAY);
        }
    }
}

static void term_closed(dsk_win_t *w)
{
    term_t *t = term_of(w);
    if (t != NULL) {
        t->win = NULL;
    }
}

/* Defined below, next to the window it belongs to. */
static bool term_key(dsk_win_t *w, uint16_t keycode, uint32_t unicode,
                     uint16_t mods);

static const dsk_win_ops_t k_term_ops = {
    .draw = term_draw,
    .key = term_key,
    .pointer = NULL,
    .closed = term_closed,
};

/*
 * Which slot this window shows, or below zero for the system console.
 *
 * A console window is a VIEW: it shows what the machine said and takes no
 * keys.  A prompt window is somebody's prompt - a slot with a shell of
 * its own - and every key it is given is passed straight through, because
 * the keyboard belongs to whatever is in front and that is this desktop.
 */
static int32_t peek(const term_t *t, uint16_t row, ag_textcell_t *cells,
                    uint16_t max)
{
    if (t->slot < 0) {
        return ag_con_peek_row(row, cells, max);
    }
    return ag_con_peek_row_slot(t->slot, row, cells, max);
}

static bool term_key(dsk_win_t *w, uint16_t keycode, uint32_t unicode,
                     uint16_t mods)
{
    term_t *t = term_of(w);
    if (t == NULL || t->slot < 0) {
        return false; /* a view, not a prompt - see dsk_term.h */
    }

    /*
     * F10 stays the desktop's.
     *
     * Everything else goes to the prompt, which is the point of the window -
     * but the menu bar is how a machine with no mouse is driven, and a
     * window that swallowed F10 would be a window you cannot get out of.
     * The scripted run found it the blunt way: after this window opened,
     * two hundred keystrokes went to a shell that had not been asked for
     * them and the desktop never saw a menu again.
     */
    if (keycode == DSK_KEY_F10) {
        return false;
    }

    /*
     * Rebuilt rather than forwarded, because what arrives here has already
     * been taken apart by the shell above: this is the event the prompt on
     * the other side would have read if the keyboard had been its own.
     */
    ag_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = AG_EV_KEY_DOWN;
    ev.key.keycode = keycode;
    ev.key.unicode = unicode;
    ev.key.mods = mods;
    s_fwd++;
    const bool took = ag_post_to_slot(t->slot, &ev);
    if (took) {
        s_fwd_ok++;
    }
    return took;
}

dsk_win_t *dsk_term_open_slot(const dsk_metrics_t *m, int slot)
{
    s_m = m;

    /*
     * The same slot twice is the same window, brought to the front - asking
     * for the console view while it is open should not open a second view of
     * the same text.  A different slot is a different window.
     */
    term_t *t = NULL;
    for (int i = 0; i < TERM_MAX; i++) {
        if (s_terms[i].win != NULL && s_terms[i].slot == slot) {
            dsk_wm_activate(s_terms[i].win);
            return s_terms[i].win;
        }
    }
    int index = 0;
    for (int i = 0; i < TERM_MAX; i++) {
        if (s_terms[i].win == NULL) {
            t = &s_terms[i];
            index = i;
            break;
        }
    }
    if (t == NULL) {
        return NULL; /* both windows are already somebody's */
    }
    t->slot = slot;

    ag_coninfo_t info;
    ag_coninfo(&info);
    uint16_t cols = (info.cols > 0) ? info.cols : 80u;
    if (cols > TERM_COLS_MAX) {
        cols = TERM_COLS_MAX;
    }
    uint16_t rows = (info.rows > 0) ? info.rows : 25u;
    if (rows > TERM_ROWS_MAX) {
        rows = TERM_ROWS_MAX;
    }

    /*
     * As many cells as the desk can hold, and never more than the console has.
     * The frame is the client area plus the furniture, so the arithmetic is
     * the layout's rather than guessed at.
     */
    const int16_t chrome_w = (int16_t)(2 * m->border);
    const int16_t chrome_h = (int16_t)(2 * m->border + m->title_h);
    int16_t max_cols = (int16_t)((m->work.w - chrome_w) / dsk_ui_w());
    int16_t max_rows = (int16_t)((m->work.h - chrome_h) / dsk_ui_h());
    if (max_cols < 20) {
        max_cols = 20;
    }
    if (max_rows < 4) {
        max_rows = 4;
    }
    t->shown_cols = (cols < (uint16_t)max_cols) ? cols : (uint16_t)max_cols;
    t->shown_rows = (rows < (uint16_t)max_rows) ? rows : (uint16_t)max_rows;

    /* Stepped, so the second one does not land exactly on the first. */
    const int16_t step = (int16_t)(8 + index * 16);
    const dsk_rect_t frame =
        dsk_rect((int16_t)(m->work.x + step), (int16_t)(m->work.y + step),
                 (int16_t)(t->shown_cols * dsk_ui_w() + chrome_w),
                 (int16_t)(t->shown_rows * dsk_ui_h() + chrome_h));

    for (uint16_t r = 0; r < TERM_ROWS_MAX; r++) {
        for (uint16_t c = 0; c < TERM_COLS_MAX; c++) {
            t->shadow[r][c].ch = ' ';
            t->shadow[r][c].attr = AG_ATTR(AG_LGRAY, AG_BLACK);
        }
    }
    t->polled = false;
    t->first_row = 0;
    t->caret_x = 0xFFFFu;
    t->caret_y = 0xFFFFu;

    t->win = dsk_wm_open((slot < 0) ? "System console" : "MS-DOS Prompt",
                         frame, &k_term_ops, NULL);
    return t->win;
}

static uint32_t due_in_one(const term_t *t, uint32_t now)
{
    if (t->win == NULL) {
        /*
         * No deadline, which in this loop is UINT32_MAX and not zero: zero
         * means "already due".  It said zero, so with the console window shut
         * - which is nearly always - the shell's wait was nought and the main
         * loop spun instead of sleeping.  A shell showing a still picture is
         * supposed to cost nothing.
         */
        return UINT32_MAX;
    }
    const uint32_t since = now - t->polled_at;
    return (since >= TERM_POLL_MS) ? 0u : (TERM_POLL_MS - since);
}

uint32_t dsk_term_due_in(uint32_t now)
{
    uint32_t soonest = UINT32_MAX;
    for (int i = 0; i < TERM_MAX; i++) {
        const uint32_t d = due_in_one(&s_terms[i], now);
        if (d < soonest) {
            soonest = d;
        }
    }
    return soonest;
}

static void tick_one(term_t *t)
{
    if (t->win == NULL || s_m == NULL) {
        return;
    }
    t->polled_at = ag_millis();

    ag_coninfo_t info;
    ag_coninfo(&info);

    /*
     * Follow the cursor, the way a terminal does: when the console is taller
     * than the window, the rows worth showing are the ones being written, not
     * the ones at the top that have scrolled out of interest.
     */
    uint16_t first = 0;
    if (t->slot < 0 && info.rows > t->shown_rows &&
        info.cur_y >= t->shown_rows) {
        first = (uint16_t)(info.cur_y - t->shown_rows + 1u);
    }
    const bool scrolled = (first != t->first_row) || !t->polled;
    t->first_row = first;
    t->polled = true;

    const dsk_rect_t client = dsk_wm_client(t->win);

    for (uint16_t row = 0; row < t->shown_rows; row++) {
        ag_textcell_t fresh[TERM_COLS_MAX];
        const int32_t n = peek(t, (uint16_t)(t->first_row + row), fresh,
                               t->shown_cols);
        if (n <= 0) {
            continue; /* not this row's fault; the console said no */
        }
        bool changed = scrolled;
        for (int32_t i = 0; !changed && i < n; i++) {
            changed = (fresh[i].ch != t->shadow[row][i].ch) ||
                      (fresh[i].attr != t->shadow[row][i].attr);
        }
        if (!changed) {
            continue;
        }
        for (int32_t i = 0; i < n; i++) {
            t->shadow[row][i] = fresh[i];
        }
        dsk_wm_damage_rect(row_rect(t, client, row));
    }

    /*
     * The caret moves without any cell changing - typing a character changes
     * one cell and moves the caret off another - so its row is repainted
     * whenever it has moved.
     */
    if (t->slot < 0 && (info.cur_x != t->caret_x || info.cur_y != t->caret_y)) {
        for (int pass = 0; pass < 2; pass++) {
            const uint16_t cy = (pass == 0) ? t->caret_y : info.cur_y;
            if (cy >= t->first_row && cy < t->first_row + t->shown_rows) {
                dsk_wm_damage_rect(
                    row_rect(t, client, (uint16_t)(cy - t->first_row)));
            }
        }
        t->caret_x = info.cur_x;
        t->caret_y = info.cur_y;
    }
}

void dsk_term_tick(void)
{
    for (int i = 0; i < TERM_MAX; i++) {
        tick_one(&s_terms[i]);
    }
}

dsk_win_t *dsk_term_open(const dsk_metrics_t *m)
{
    return dsk_term_open_slot(m, -1);
}

void dsk_term_fwd_stats(uint32_t *sent, uint32_t *taken)
{
    if (sent != NULL) {
        *sent = s_fwd;
    }
    if (taken != NULL) {
        *taken = s_fwd_ok;
    }
}
