/*
 * ArgonOS DESKTOP - the keyboard drawn on the glass.  See dsk_kbd.h.
 *
 * Five rows, ten keys each, laid out on the box it is given rather than on a
 * grid of fixed pixels: the same table has to work on a 320-pixel panel and
 * on 640, and a key that is 32 pixels wide on one and 32 on the other wastes
 * half the second.
 *
 * The layout is not a QWERTY keyboard's, quite.  What is typed into these
 * boxes is file names and paths, so the row a hand reaches most easily
 * carries the letters, and the punctuation that a path needs - backslash,
 * colon, dot, underscore - is on the keyboard rather than behind a shift
 * that would have to be invented for it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_kbd.h"

#include "dsk_paint.h"

#define ROWS 5
#define COLS 10

/*
 * Row 4 is the one with wide keys, and it is spelled here as ten cells so
 * that hit-testing stays "which cell of the grid" for every row.  Space
 * covers four of them and says so by repeating.
 */
static const char k_rows[ROWS][COLS + 1] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl.",
    "zxcvbnm-_:",
    /* Shift, backslash, a space bar four cells wide, slash, the two
     * wildcards a Run box wants, and the one that rubs out. */
    "^\\    /*?#",
};

/* What shift makes of a key.  Only where it differs from upper case. */
static char shifted(char c)
{
    switch (c) {
    case '1': return '!';
    case '2': return '@';
    case '3': return '#';
    case '4': return '$';
    case '5': return '%';
    case '6': return '^';
    case '7': return '&';
    case '8': return '*';
    case '9': return '(';
    case '0': return ')';
    case '-': return '+';
    case '.': return ',';
    case '_': return '=';
    case ':': return ';';
    case '/': return '?';
    default:
        if (c >= 'a' && c <= 'z') {
            return (char)(c - 'a' + 'A');
        }
        return c;
    }
}

static bool s_shift;
static int  s_hi = -1;

void dsk_kbd_reset(void)
{
    s_shift = false;
    s_hi = -1;
}

void dsk_kbd_hilite(int key) { s_hi = key; }

/*
 * A key is as tall as the box allows, and the box is what the caller had
 * left over.
 *
 * It used to be a line of the chosen font plus room for a fingertip, which
 * is the right size and the wrong question: on 320x240 the box that wanted
 * to hold it was taller than the work area, the window manager cut it down,
 * and the keyboard - anchored to the bottom - climbed over the very text
 * field it exists to fill.  Maxim could hit the keys and could not see what
 * he was typing.
 *
 * So the height comes from the rectangle, both here and in the hit test,
 * and the two cannot disagree about where a key is.
 */
#define KEY_MIN 12

static int16_t key_h_of(dsk_rect_t r)
{
    const int16_t h = (int16_t)((r.h - 2) / ROWS);
    return (h < KEY_MIN) ? KEY_MIN : h;
}

int16_t dsk_kbd_height(int16_t w)
{
    return dsk_kbd_height_for(w, 32767);
}

int16_t dsk_kbd_height_for(int16_t w, int16_t avail)
{
    if (w < COLS * KEY_MIN || avail < ROWS * KEY_MIN + 2) {
        return 0; /* below this the keys are smaller than what presses them */
    }
    int16_t want = (int16_t)(dsk_ui_h() + 6);
    if (want < KEY_MIN) {
        want = KEY_MIN;
    }
    int16_t h = (int16_t)(ROWS * want + 2);
    if (h > avail) {
        h = avail;
    }
    return h;
}

static dsk_rect_t cell_rect(dsk_rect_t r, int row, int col)
{
    const int16_t kw = (int16_t)(r.w / COLS);
    const int16_t kh = key_h_of(r);
    return dsk_rect((int16_t)(r.x + col * kw), (int16_t)(r.y + 1 + row * kh),
                    kw, kh);
}

/* The label a cell shows: two of them are words rather than characters. */
static void cell_label(int row, int col, char *out, int cap)
{
    const char c = k_rows[row][col];
    out[0] = '\0';
    if (cap < 6) {
        return;
    }
    if (row == ROWS - 1 && col == 0) {
        out[0] = (char)(s_shift ? 'A' : 'a');
        out[1] = '\0';
        return;
    }
    if (row == ROWS - 1 && col == COLS - 1) {
        out[0] = '<';
        out[1] = '-';
        out[2] = '\0';
        return;
    }
    if (c == ' ') {
        return; /* the space bar carries no letter */
    }
    out[0] = s_shift ? shifted(c) : c;
    out[1] = '\0';
}

void dsk_kbd_draw(dsk_rect_t r)
{
    const int16_t kh = key_h_of(r);
    const int16_t kw = (int16_t)(r.w / COLS);

    dsk_fill(r, DSK_LGRAY);
    for (int row = 0; row < ROWS; row++) {
        for (int col = 0; col < COLS; col++) {
            const char c = k_rows[row][col];
            /*
             * The space bar is four cells and is drawn once, by its first:
             * four keys touching each other look like four keys.
             */
            if (c == ' ' && !(row == ROWS - 1 && col == 2)) {
                continue;
            }
            dsk_rect_t b = cell_rect(r, row, col);
            if (c == ' ') {
                b.w = (int16_t)(kw * 4);
            }
            const int  id = row * COLS + col;
            const bool down = (id == s_hi);
            dsk_panel(dsk_rect_inset(b, 1), !down, DSK_LGRAY);

            char label[8];
            cell_label(row, col, label, sizeof(label));
            if (label[0] != '\0') {
                int16_t n = 0;
                while (label[n] != '\0') {
                    n++;
                }
                dsk_text((int16_t)(b.x + (b.w - n * dsk_ui_w()) / 2),
                         (int16_t)(b.y + (kh - dsk_ui_h()) / 2), label,
                         DSK_BLACK, DSK_LGRAY);
            }
        }
    }
}

int dsk_kbd_key_at(dsk_rect_t r, int16_t x, int16_t y)
{
    if (!dsk_rect_has(r, x, y)) {
        return -1;
    }
    const int16_t kw = (int16_t)(r.w / COLS);
    const int16_t kh = key_h_of(r);
    const int     col = (x - r.x) / (kw > 0 ? kw : 1);
    const int     row = (y - (r.y + 1)) / (kh > 0 ? kh : 1);
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS) {
        return -1;
    }
    return row * COLS + col;
}

int dsk_kbd_press(dsk_rect_t r, int16_t x, int16_t y)
{
    const int id = dsk_kbd_key_at(r, x, y);
    if (id < 0) {
        return DSK_KBD_NONE;
    }
    const int  row = id / COLS;
    const int  col = id % COLS;
    const char c = k_rows[row][col];

    if (row == ROWS - 1 && col == 0) {
        s_shift = !s_shift;
        return DSK_KBD_SHIFT;
    }
    if (c == ' ') {
        return ' ';
    }
    if (c == '#' && col == COLS - 1) {
        return DSK_KBD_BACKSPACE; /* the last cell is the rubbing-out one */
    }

    const char out = s_shift ? shifted(c) : c;
    /*
     * Shift is for one key, as it is on a keyboard where somebody is holding
     * it down: a person typing "C:" presses shift, presses the colon, and
     * does not expect the next letter to be shouted.
     */
    s_shift = false;
    return (int)(unsigned char)out;
}
