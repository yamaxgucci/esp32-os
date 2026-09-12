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
    /*
     * Scrolled back by hand, and therefore not following the cursor.
     *
     * A terminal that always shows the newest rows cannot be read back, and
     * on a panel there is no scrollbar worth hitting with a thumb - so the
     * text itself is dragged.  Dragged to the bottom it follows the prompt
     * again, which is what a person means by getting back to the bottom.
     */
    bool          pinned;
    int16_t       drag_y;      /* where the finger went down */
    uint16_t      drag_first;  /* and which row was at the top then */
    bool          dragging;
    bool          moved;       /* a drag, so the release is not a tap */
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
     * Only the window in front has a caret.
     *
     * That is what a caret says - "the keys are coming here" - and with two
     * text windows on the desk a caret in both says nothing at all.  It also
     * keeps a still picture still: a blinking caret in a window nobody is
     * typing into is a screen that never stops changing, and the shell's own
     * proof that a repaint put back exactly what was there cannot tell that
     * apart from a repaint that lost something.
     */
    if (dsk_wm_active() != w) {
        return;
    }

    /*
     * Whose cursor, and where.
     *
     * coninfo answers about THIS task's console, so a prompt window asks the
     * system about the slot it is showing (ABI 0.46).  Without that it drew
     * no caret at all, and a prompt with nothing blinking in it does not
     * look like a prompt.
     */
    uint16_t cur_x = 0, cur_y = 0;
    if (t->slot < 0) {
        ag_coninfo_t info;
        ag_coninfo(&info);
        cur_x = info.cur_x;
        cur_y = info.cur_y;
    } else if (!ag_con_cursor_slot(t->slot, &cur_x, &cur_y)) {
        return;
    }

    /*
     * Blinking, because that is what says "this one is taking what you
     * type": with two windows on the desk showing text, a still block says
     * nothing about which of them the keys are going to.  Half a second on,
     * half a second off, off the same clock for both windows.
     */
    if (((ag_millis() / 500u) & 1u) != 0u) {
        return;
    }

    if (cur_y >= t->first_row && cur_y < t->first_row + t->shown_rows &&
        cur_x < t->shown_cols) {
        const dsk_rect_t caret =
            dsk_rect((int16_t)(client.x + cur_x * dsk_ui_w()),
                     (int16_t)(client.y +
                               (cur_y - t->first_row) * dsk_ui_h() +
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

/*
 * A tap landed on a prompt's text, and nobody has taken it yet.
 *
 * The desk polls this to raise its keyboard.  It is a TAP, not a press: a
 * drag is how the text is scrolled, and a keyboard that came up on every
 * press covered the very rows the person was dragging into view.  Nor is it
 * the frame - dragging a window by its caption is not asking to type.
 */
static bool s_tapped;

bool dsk_term_take_tap(void)
{
    const bool was = s_tapped;
    s_tapped = false;
    return was;
}

/* How many rows the text can be scrolled back through. */
static uint16_t rows_total(const term_t *t)
{
    ag_coninfo_t info;
    ag_coninfo(&info);
    const uint16_t rows = (info.rows > 0) ? info.rows : t->shown_rows;
    return (rows > t->shown_rows) ? rows : t->shown_rows;
}

static bool term_pointer(dsk_win_t *w, dsk_hit_t where, int16_t x, int16_t y,
                         uint8_t buttons, dsk_ptr_t type, bool dbl)
{
    (void)x;
    (void)buttons;
    (void)dbl;
    term_t *t = term_of(w);
    if (t == NULL) {
        return false;
    }

    if (type == DSK_PTR_DOWN) {
        if (where != DSK_HIT_CLIENT) {
            return false; /* the frame is the window manager's business */
        }
        t->dragging = true;
        t->moved = false;
        t->drag_y = y;
        t->drag_first = t->first_row;
        return true;
    }
    if (!t->dragging) {
        return false;
    }

    const uint16_t total = rows_total(t);
    const int16_t  dy = (int16_t)(y - t->drag_y);
    const int16_t  rows = (int16_t)(dy / (int16_t)dsk_ui_h());
    if (rows != 0) {
        t->moved = true;
        /*
         * Dragged DOWN means "show me what came before", the way a sheet of
         * paper moves under a finger rather than the view moving over it.
         */
        int32_t want = (int32_t)t->drag_first - rows;
        const int32_t most = (int32_t)total - (int32_t)t->shown_rows;
        if (want < 0) {
            want = 0;
        }
        if (want > most) {
            want = most;
        }
        if ((uint16_t)want != t->first_row) {
            t->first_row = (uint16_t)want;
            t->polled = false; /* every row on screen is a different row now */
            dsk_wm_damage_rect(dsk_wm_client(w));
        }
        /* At the bottom it follows the prompt again; above it, it stays. */
        t->pinned = ((int32_t)t->first_row < most);
    }

    if (type == DSK_PTR_UP) {
        t->dragging = false;
        if (!t->moved && t->slot >= 0) {
            s_tapped = true; /* a tap on a prompt's text asks for the keys */
        }
    }
    return true;
}

static const dsk_win_ops_t k_term_ops = {
    .draw = term_draw,
    .key = term_key,
    .pointer = term_pointer,
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

    t->win = dsk_wm_open((slot < 0) ? "System console" : "Console",
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
    uint16_t first = t->first_row;
    if (!t->pinned) {
        /*
         * Follow the cursor, the way a terminal does - unless somebody has
         * scrolled back, in which case the rows they are reading stay put
         * while the prompt goes on printing underneath.
         */
        uint16_t cur_y = 0;
        if (t->slot < 0) {
            cur_y = info.cur_y;
        } else {
            uint16_t cur_x = 0;
            (void)ag_con_cursor_slot(t->slot, &cur_x, &cur_y);
        }
        first = 0;
        if (info.rows > t->shown_rows && cur_y >= t->shown_rows) {
            first = (uint16_t)(cur_y - t->shown_rows + 1u);
        }
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
    if (dsk_wm_active() != t->win) {
        /*
         * No caret in a window that is not taking the keys - and the row it
         * was last drawn on has to be repainted once, or the caret stays
         * behind on a window that has just lost the focus.
         */
        if (t->caret_y != 0xFFFFu) {
            if (t->caret_y >= t->first_row &&
                t->caret_y < t->first_row + t->shown_rows) {
                dsk_wm_damage_rect(
                    row_rect(t, client, (uint16_t)(t->caret_y - t->first_row)));
            }
            t->caret_x = 0xFFFFu;
            t->caret_y = 0xFFFFu;
        }
        return;
    }

    uint16_t cx = info.cur_x, cy = info.cur_y;
    if (t->slot >= 0) {
        (void)ag_con_cursor_slot(t->slot, &cx, &cy);
    }
    /*
     * The caret blinks, so its row is repainted on every poll rather than
     * only when it moves: a hundred milliseconds of a window the size of a
     * line is nothing, and it is the only thing that says where the typing
     * is going.
     */
    for (int pass = 0; pass < 2; pass++) {
        const uint16_t row = (pass == 0) ? t->caret_y : cy;
        if (row >= t->first_row && row < t->first_row + t->shown_rows) {
            dsk_wm_damage_rect(
                row_rect(t, client, (uint16_t)(row - t->first_row)));
        }
    }
    t->caret_x = cx;
    t->caret_y = cy;
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

bool dsk_term_is_prompt(const dsk_win_t *w)
{
    const term_t *t = term_of(w);
    return (t != NULL) && (t->slot >= 0);
}
