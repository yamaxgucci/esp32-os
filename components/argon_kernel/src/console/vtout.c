/*
 * ArgonOS - VT100 renderer for the virtual text screen.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/vtout.h>

#include <string.h>

/*
 * The PC attribute byte orders colours blue-first, ANSI orders them red-first.
 * The permutation happens to be its own inverse, so the same table converts in
 * both directions.
 */
static const uint8_t k_cga_to_ansi[8] = {0, 4, 2, 6, 1, 5, 3, 7};

/* Cursor column is unknown, so the next row must position absolutely. */
#define AG_VT_X_UNKNOWN 0xffffu

/* ---------------------------------------------------------------------- */
/* Output buffering                                                       */
/* ---------------------------------------------------------------------- */

typedef struct {
    ag_vt_sink_fn sink;
    void         *ctx;
    char          buf[96];
    size_t        len;
} emitter_t;

static void emit_drain(emitter_t *e)
{
    if (e->len > 0) {
        e->sink(e->ctx, e->buf, e->len);
        e->len = 0;
    }
}

static void emit(emitter_t *e, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (e->len == sizeof(e->buf)) {
            emit_drain(e);
        }
        e->buf[e->len++] = data[i];
    }
}

static void emit_str(emitter_t *e, const char *s) { emit(e, s, strlen(s)); }

static void emit_char(emitter_t *e, char c) { emit(e, &c, 1); }

static void emit_uint(emitter_t *e, uint32_t v)
{
    char   tmp[10];
    size_t n = 0;

    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0 && n < sizeof(tmp));

    while (n > 0) {
        emit_char(e, tmp[--n]);
    }
}

static void emit_goto(emitter_t *e, uint16_t x, uint16_t y)
{
    emit_str(e, "\x1b[");
    emit_uint(e, (uint32_t)y + 1u);
    emit_char(e, ';');
    emit_uint(e, (uint32_t)x + 1u);
    emit_char(e, 'H');
}

static void emit_attr(emitter_t *e, uint8_t attr)
{
    const uint8_t fg = (uint8_t)(attr & 0x07u);
    const uint8_t bg = (uint8_t)((attr >> 4) & 0x07u);
    const bool    fg_bright = (attr & 0x08u) != 0;
    const bool    bg_bright = (attr & 0x80u) != 0;

    /* Always start from a reset so no previous state has to be tracked. */
    emit_str(e, "\x1b[0");

    if (fg_bright) {
        emit_str(e, ";9");
    } else {
        emit_str(e, ";3");
    }
    emit_uint(e, k_cga_to_ansi[fg]);

    /* Default background needs nothing: the reset already selected it. */
    if (bg != 0 || bg_bright) {
        emit_str(e, bg_bright ? ";10" : ";4");
        emit_uint(e, k_cga_to_ansi[bg]);
    }

    emit_char(e, 'm');
}

/* ---------------------------------------------------------------------- */

static inline size_t word_count(uint16_t rows)
{
    return ((size_t)rows + 31u) / 32u;
}

void ag_vtout_init(ag_vtout_t *o)
{
    if (o == NULL) {
        return;
    }
    memset(o, 0, sizeof(*o));
    o->last_attr = AG_ATTR_DEFAULT;
    o->last_x = AG_VT_X_UNKNOWN;
    o->cursor_visible = true;
}

void ag_vtout_mark_all(ag_vtout_t *o)
{
    memset(o->dirty, 0xff, sizeof(o->dirty));
    o->synced = false;
}

void ag_vtout_take_dirty(ag_vtout_t *o, const ag_screen_t *s)
{
    const size_t n = word_count(s->rows);
    for (size_t i = 0; i < n && i < sizeof(o->dirty) / sizeof(o->dirty[0]);
         i++) {
        o->dirty[i] |= s->dirty[i];
    }
}

bool ag_vtout_pending(const ag_vtout_t *o)
{
    for (size_t i = 0; i < sizeof(o->dirty) / sizeof(o->dirty[0]); i++) {
        if (o->dirty[i] != 0) {
            return true;
        }
    }
    return false;
}

static bool row_pending(const ag_vtout_t *o, uint16_t y)
{
    return (o->dirty[y / 32u] & ((uint32_t)1u << (y % 32u))) != 0;
}

static void row_done(ag_vtout_t *o, uint16_t y)
{
    o->dirty[y / 32u] &= ~((uint32_t)1u << (y % 32u));
}

/* Number of cells worth sending: trailing default-attribute blanks are not. */
static uint16_t row_extent(const ag_cell_t *row, uint16_t cols)
{
    uint16_t extent = 0;
    for (uint16_t x = 0; x < cols; x++) {
        if (row[x].ch != ' ' || row[x].attr != AG_ATTR_DEFAULT) {
            extent = (uint16_t)(x + 1);
        }
    }
    return extent;
}

static void render_row(ag_vtout_t *o, const ag_screen_t *s, uint16_t y,
                       uint16_t cols, emitter_t *e)
{
    const ag_cell_t *row = ag_screen_row(s, y);
    if (row == NULL) {
        return;
    }

    const uint16_t extent = row_extent(row, cols);

    emit_goto(e, 0, y);
    o->last_y = y;

    for (uint16_t x = 0; x < extent; x++) {
        if (row[x].attr != o->last_attr) {
            emit_attr(e, row[x].attr);
            o->last_attr = row[x].attr;
        }
        const unsigned char ch = (unsigned char)row[x].ch;
        emit_char(e, (ch < 0x20 || ch == 0x7f) ? ' ' : row[x].ch);
    }

    if (extent < cols) {
        /*
         * Erase to end of line paints with the current background, so reset
         * first or a coloured run would smear across the rest of the row.
         */
        if (o->last_attr != AG_ATTR_DEFAULT) {
            emit_attr(e, AG_ATTR_DEFAULT);
            o->last_attr = AG_ATTR_DEFAULT;
        }
        emit_str(e, "\x1b[K");
    }

    /*
     * After writing the last column a terminal may or may not have wrapped,
     * depending on its autowrap setting.  Rather than guess, forget where the
     * cursor is and position absolutely for the next row.
     */
    o->last_x = AG_VT_X_UNKNOWN;
}

void ag_vtout_flush(ag_vtout_t *o, const ag_screen_t *s, ag_vt_sink_fn sink,
                    void *ctx)
{
    if (o == NULL || s == NULL || sink == NULL) {
        return;
    }

    emitter_t e = {.sink = sink, .ctx = ctx, .len = 0};

    const uint16_t cols = (o->cols != 0 && o->cols < s->cols) ? o->cols
                                                              : s->cols;
    const uint16_t rows = (o->rows != 0 && o->rows < s->rows) ? o->rows
                                                              : s->rows;

    if (!o->synced) {
        /* Hide the cursor for the duration of a full repaint. */
        emit_str(&e, "\x1b[?25l\x1b[0m");
        o->last_attr = AG_ATTR_DEFAULT;
        o->cursor_visible = false;
        o->synced = true;
    }

    for (uint16_t y = 0; y < rows; y++) {
        if (row_pending(o, y)) {
            render_row(o, s, y, cols, &e);
            row_done(o, y);
        }
    }

    /* Rows the terminal cannot show are still considered handled. */
    for (uint16_t y = rows; y < AG_SCREEN_MAX_ROWS; y++) {
        row_done(o, y);
    }

    const uint16_t cx = (s->cur_x < cols) ? s->cur_x : (uint16_t)(cols - 1);
    const uint16_t cy = (s->cur_y < rows) ? s->cur_y : (uint16_t)(rows - 1);

    if (o->last_x != cx || o->last_y != cy) {
        emit_goto(&e, cx, cy);
        o->last_x = cx;
        o->last_y = cy;
    }

    if (o->cursor_visible != s->cursor_visible) {
        emit_str(&e, s->cursor_visible ? "\x1b[?25h" : "\x1b[?25l");
        o->cursor_visible = s->cursor_visible;
    }

    emit_drain(&e);
}

void ag_vtout_hello(ag_vtout_t *o, ag_vt_sink_fn sink, void *ctx)
{
    if (o == NULL || sink == NULL) {
        return;
    }

    emitter_t e = {.sink = sink, .ctx = ctx, .len = 0};

    /* Reset attributes, enable autowrap, clear, home. */
    emit_str(&e, "\x1b[0m\x1b[?7h\x1b[2J\x1b[H");
    emit_drain(&e);

    ag_vtout_init(o);
    ag_vtout_mark_all(o);
}

void ag_vtout_goodbye(ag_vt_sink_fn sink, void *ctx)
{
    if (sink == NULL) {
        return;
    }

    emitter_t e = {.sink = sink, .ctx = ctx, .len = 0};
    emit_str(&e, "\x1b[0m\x1b[?25h\r\n");
    emit_drain(&e);
}
