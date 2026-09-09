/*
 * ArgonOS DESKTOP - the console, in a window.  See dsk_term.h for why.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_term.h"

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

static const dsk_metrics_t *s_m;
static dsk_win_t           *s_win;
static ag_textcell_t        s_shadow[TERM_ROWS_MAX][TERM_COLS_MAX];
static uint16_t             s_shown_rows;  /* rows the window has space for  */
static uint16_t             s_shown_cols;
static uint16_t             s_first_row;   /* which console row is at the top */
static uint32_t             s_polled_at;
static bool                 s_polled;

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

static dsk_rect_t row_rect(dsk_rect_t client, uint16_t row)
{
    return dsk_rect(client.x, (int16_t)(client.y + row * dsk_ui_h()),
                    (int16_t)(s_shown_cols * dsk_ui_w()), dsk_ui_h());
}

/*
 * One row of cells, as runs of one attribute.
 *
 * Run by run rather than cell by cell: a line of console text is usually one
 * attribute from end to end, and handing sixty glyphs to the painter in one
 * call instead of sixty is the difference between a window that repaints in
 * one millisecond and one that does not.
 */
static void draw_row(dsk_rect_t client, uint16_t row)
{
    const ag_textcell_t *cells = s_shadow[row];
    const int16_t        y = (int16_t)(client.y + row * dsk_ui_h());
    uint16_t             at = 0;

    while (at < s_shown_cols) {
        const uint8_t attr = cells[at].attr;
        uint16_t      end = at;
        char          run[TERM_COLS_MAX + 1];

        while (end < s_shown_cols && cells[end].attr == attr) {
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
    (void)w;

    /* The margins the cells do not cover, so the window has no stale edges. */
    const int16_t used_w = (int16_t)(s_shown_cols * dsk_ui_w());
    const int16_t used_h = (int16_t)(s_shown_rows * dsk_ui_h());
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

    for (uint16_t row = 0; row < s_shown_rows; row++) {
        if (!dsk_visible(row_rect(client, row))) {
            continue;
        }
        draw_row(client, row);
    }

    /*
     * The caret, as the console's own block rather than a shell decoration:
     * this window is a view of a text screen, and where that screen's cursor
     * is is part of what it says.
     */
    ag_coninfo_t info;
    ag_coninfo(&info);
    if (info.cur_y >= s_first_row && info.cur_y < s_first_row + s_shown_rows &&
        info.cur_x < s_shown_cols) {
        const dsk_rect_t caret =
            dsk_rect((int16_t)(client.x + info.cur_x * dsk_ui_w()),
                     (int16_t)(client.y +
                               (info.cur_y - s_first_row) * dsk_ui_h() +
                               dsk_ui_h() - 2),
                     dsk_ui_w(), 2);
        if (dsk_visible(caret)) {
            dsk_fill(caret, DSK_LGRAY);
        }
    }
}

static void term_closed(dsk_win_t *w)
{
    if (s_win == w) {
        s_win = NULL;
    }
}

static const dsk_win_ops_t k_term_ops = {
    .draw = term_draw,
    .key = NULL, /* a view, not a prompt - see dsk_term.h */
    .pointer = NULL,
    .closed = term_closed,
};

dsk_win_t *dsk_term_open(const dsk_metrics_t *m)
{
    s_m = m;
    if (s_win != NULL) {
        dsk_wm_activate(s_win);
        return s_win;
    }

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
    s_shown_cols = (cols < (uint16_t)max_cols) ? cols : (uint16_t)max_cols;
    s_shown_rows = (rows < (uint16_t)max_rows) ? rows : (uint16_t)max_rows;

    const dsk_rect_t frame =
        dsk_rect((int16_t)(m->work.x + 8), (int16_t)(m->work.y + 8),
                 (int16_t)(s_shown_cols * dsk_ui_w() + chrome_w),
                 (int16_t)(s_shown_rows * dsk_ui_h() + chrome_h));

    for (uint16_t r = 0; r < TERM_ROWS_MAX; r++) {
        for (uint16_t c = 0; c < TERM_COLS_MAX; c++) {
            s_shadow[r][c].ch = ' ';
            s_shadow[r][c].attr = AG_ATTR(AG_LGRAY, AG_BLACK);
        }
    }
    s_polled = false;
    s_first_row = 0;

    s_win = dsk_wm_open("System console", frame, &k_term_ops, NULL);
    return s_win;
}

uint32_t dsk_term_due_in(uint32_t now)
{
    if (s_win == NULL) {
        return 0u; /* nothing to poll: no deadline of its own */
    }
    const uint32_t since = now - s_polled_at;
    return (since >= TERM_POLL_MS) ? 0u : (TERM_POLL_MS - since);
}

void dsk_term_tick(void)
{
    if (s_win == NULL || s_m == NULL) {
        return;
    }
    s_polled_at = ag_millis();

    ag_coninfo_t info;
    ag_coninfo(&info);

    /*
     * Follow the cursor, the way a terminal does: when the console is taller
     * than the window, the rows worth showing are the ones being written, not
     * the ones at the top that have scrolled out of interest.
     */
    uint16_t first = 0;
    if (info.rows > s_shown_rows && info.cur_y >= s_shown_rows) {
        first = (uint16_t)(info.cur_y - s_shown_rows + 1u);
    }
    const bool scrolled = (first != s_first_row) || !s_polled;
    s_first_row = first;
    s_polled = true;

    const dsk_rect_t client = dsk_wm_client(s_win);

    for (uint16_t row = 0; row < s_shown_rows; row++) {
        ag_textcell_t fresh[TERM_COLS_MAX];
        const int32_t n = ag_con_peek_row((uint16_t)(s_first_row + row), fresh,
                                          s_shown_cols);
        if (n <= 0) {
            continue; /* not this row's fault; the console said no */
        }
        bool changed = scrolled;
        for (int32_t i = 0; !changed && i < n; i++) {
            changed = (fresh[i].ch != s_shadow[row][i].ch) ||
                      (fresh[i].attr != s_shadow[row][i].attr);
        }
        if (!changed) {
            continue;
        }
        for (int32_t i = 0; i < n; i++) {
            s_shadow[row][i] = fresh[i];
        }
        dsk_wm_damage_rect(row_rect(client, row));
    }

    /*
     * The caret moves without any cell changing - typing a character changes
     * one cell and moves the caret off another - so its row is repainted
     * whenever it has moved.
     */
    static uint16_t s_caret_x = 0xFFFFu, s_caret_y = 0xFFFFu;
    if (info.cur_x != s_caret_x || info.cur_y != s_caret_y) {
        for (int pass = 0; pass < 2; pass++) {
            const uint16_t cy = (pass == 0) ? s_caret_y : info.cur_y;
            if (cy >= s_first_row && cy < s_first_row + s_shown_rows) {
                dsk_wm_damage_rect(
                    row_rect(client, (uint16_t)(cy - s_first_row)));
            }
        }
        s_caret_x = info.cur_x;
        s_caret_y = info.cur_y;
    }
}
