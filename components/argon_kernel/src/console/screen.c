/*
 * ArgonOS - virtual text screen and ANSI/VT100 parser.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/screen.h>

#include <string.h>

#define AG_TAB_WIDTH 8

/*
 * ANSI orders its colours red-first, the PC attribute byte orders them
 * blue-first.  Everything else about the two is compatible.
 */
static const uint8_t k_ansi_to_cga[8] = {
    AG_BLACK, AG_RED,     AG_GREEN, AG_BROWN,
    AG_BLUE,  AG_MAGENTA, AG_CYAN,  AG_LGRAY,
};

static inline size_t dirty_words(uint16_t rows)
{
    return ((size_t)rows + 31u) / 32u;
}

size_t ag_screen_memsize(uint16_t cols, uint16_t rows)
{
    return dirty_words(rows) * sizeof(uint32_t) +
           (size_t)cols * (size_t)rows * sizeof(ag_cell_t);
}

static inline ag_cell_t *cell_at(const ag_screen_t *s, uint16_t x, uint16_t y)
{
    return &s->cells[(size_t)y * s->cols + x];
}

void ag_screen_mark_row_dirty(ag_screen_t *s, uint16_t y)
{
    if (y < s->rows) {
        s->dirty[y / 32u] |= (uint32_t)1u << (y % 32u);
        s->generation++;
    }
}

void ag_screen_mark_all_dirty(ag_screen_t *s)
{
    const size_t n = dirty_words(s->rows);
    for (size_t i = 0; i < n; i++) {
        s->dirty[i] = 0xffffffffu;
    }
    s->generation++;
}

void ag_screen_clear_dirty(ag_screen_t *s)
{
    memset(s->dirty, 0, dirty_words(s->rows) * sizeof(uint32_t));
}

bool ag_screen_row_dirty(const ag_screen_t *s, uint16_t y)
{
    if (y >= s->rows) {
        return false;
    }
    return (s->dirty[y / 32u] & ((uint32_t)1u << (y % 32u))) != 0;
}

bool ag_screen_any_dirty(const ag_screen_t *s)
{
    const size_t n = dirty_words(s->rows);
    for (size_t i = 0; i < n; i++) {
        if (s->dirty[i] != 0) {
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------- */

static void fill_row(ag_screen_t *s, uint16_t y, uint16_t x0, uint16_t x1,
                     char ch, uint8_t attr)
{
    if (y >= s->rows) {
        return;
    }
    if (x1 > s->cols) {
        x1 = s->cols;
    }
    for (uint16_t x = x0; x < x1; x++) {
        ag_cell_t *c = cell_at(s, x, y);
        c->ch = ch;
        c->attr = attr;
    }
    if (x1 > x0) {
        ag_screen_mark_row_dirty(s, y);
    }
}

ag_err_t ag_screen_init(ag_screen_t *s, void *mem, size_t memsize,
                        uint16_t cols, uint16_t rows)
{
    if (s == NULL || mem == NULL) {
        return -AG_EINVAL;
    }
    if (cols == 0 || rows == 0 || cols > AG_SCREEN_MAX_COLS ||
        rows > AG_SCREEN_MAX_ROWS) {
        return -AG_EINVAL;
    }
    if (memsize < ag_screen_memsize(cols, rows)) {
        return -AG_ERANGE;
    }
    if (((uintptr_t)mem & 3u) != 0) {
        return -AG_EINVAL; /* the dirty bitmap needs 32-bit alignment */
    }

    memset(s, 0, sizeof(*s));
    s->dirty = (uint32_t *)mem;
    s->cells = (ag_cell_t *)((uint8_t *)mem +
                             dirty_words(rows) * sizeof(uint32_t));
    s->cols = cols;
    s->rows = rows;
    s->attr = AG_ATTR_DEFAULT;
    s->cursor_visible = true;
    s->saved_attr = AG_ATTR_DEFAULT;

    ag_screen_cls(s);
    return AG_OK;
}

void ag_screen_cls(ag_screen_t *s)
{
    for (uint16_t y = 0; y < s->rows; y++) {
        fill_row(s, y, 0, s->cols, ' ', s->attr);
    }
    s->cur_x = 0;
    s->cur_y = 0;
    s->pending_wrap = false;
    ag_screen_mark_all_dirty(s);
}

void ag_screen_scroll_up(ag_screen_t *s, uint16_t lines)
{
    if (lines == 0) {
        return;
    }
    if (lines >= s->rows) {
        for (uint16_t y = 0; y < s->rows; y++) {
            fill_row(s, y, 0, s->cols, ' ', s->attr);
        }
        ag_screen_mark_all_dirty(s);
        return;
    }

    const size_t moved = (size_t)(s->rows - lines) * s->cols;
    memmove(s->cells, s->cells + (size_t)lines * s->cols,
            moved * sizeof(ag_cell_t));
    for (uint16_t y = (uint16_t)(s->rows - lines); y < s->rows; y++) {
        fill_row(s, y, 0, s->cols, ' ', s->attr);
    }
    ag_screen_mark_all_dirty(s);
}

static void scroll_down(ag_screen_t *s, uint16_t lines)
{
    if (lines == 0) {
        return;
    }
    if (lines >= s->rows) {
        ag_screen_scroll_up(s, s->rows);
        return;
    }

    const size_t moved = (size_t)(s->rows - lines) * s->cols;
    memmove(s->cells + (size_t)lines * s->cols, s->cells,
            moved * sizeof(ag_cell_t));
    for (uint16_t y = 0; y < lines; y++) {
        fill_row(s, y, 0, s->cols, ' ', s->attr);
    }
    ag_screen_mark_all_dirty(s);
}

static void newline(ag_screen_t *s)
{
    s->cur_x = 0;
    s->pending_wrap = false;
    if (s->cur_y + 1 >= s->rows) {
        ag_screen_scroll_up(s, 1);
        s->cur_y = (uint16_t)(s->rows - 1);
    } else {
        s->cur_y++;
    }
}

void ag_screen_putc_raw(ag_screen_t *s, char ch)
{
    if (s->pending_wrap) {
        newline(s);
    }

    ag_cell_t *c = cell_at(s, s->cur_x, s->cur_y);
    if (c->ch != ch || c->attr != s->attr) {
        c->ch = ch;
        c->attr = s->attr;
        ag_screen_mark_row_dirty(s, s->cur_y);
    }

    if (s->cur_x + 1 >= s->cols) {
        s->pending_wrap = true; /* stay put until the next character */
    } else {
        s->cur_x++;
    }
}

void ag_screen_gotoxy(ag_screen_t *s, uint16_t x, uint16_t y)
{
    s->cur_x = (x < s->cols) ? x : (uint16_t)(s->cols - 1);
    s->cur_y = (y < s->rows) ? y : (uint16_t)(s->rows - 1);
    s->pending_wrap = false;
}

void ag_screen_set_attr(ag_screen_t *s, uint8_t attr) { s->attr = attr; }

void ag_screen_set_cursor(ag_screen_t *s, bool visible)
{
    if (s->cursor_visible != visible) {
        s->cursor_visible = visible;
        s->generation++;
    }
}

void ag_screen_poke(ag_screen_t *s, uint16_t x, uint16_t y, char ch,
                    uint8_t attr)
{
    if (x >= s->cols || y >= s->rows) {
        return;
    }
    ag_cell_t *c = cell_at(s, x, y);
    if (c->ch != ch || c->attr != attr) {
        c->ch = ch;
        c->attr = attr;
        ag_screen_mark_row_dirty(s, y);
    }
}

void ag_screen_fill(ag_screen_t *s, uint16_t x, uint16_t y, uint16_t w,
                    uint16_t h, char ch, uint8_t attr)
{
    for (uint16_t row = y; row < y + h && row < s->rows; row++) {
        fill_row(s, row, x, (uint16_t)(x + w), ch, attr);
    }
}

const ag_cell_t *ag_screen_row(const ag_screen_t *s, uint16_t y)
{
    return (y < s->rows) ? cell_at(s, 0, y) : NULL;
}

ag_cell_t ag_screen_at(const ag_screen_t *s, uint16_t x, uint16_t y)
{
    if (x >= s->cols || y >= s->rows) {
        const ag_cell_t empty = {' ', AG_ATTR_DEFAULT};
        return empty;
    }
    return *cell_at(s, x, y);
}

/* ---------------------------------------------------------------------- */
/* Escape sequence handling                                               */
/* ---------------------------------------------------------------------- */

static int32_t pget(const ag_screen_t *s, uint8_t index, int32_t fallback)
{
    if (index >= s->nparams || s->params[index] < 0) {
        return fallback;
    }
    return s->params[index];
}

static void csi_reset(ag_screen_t *s)
{
    for (uint8_t i = 0; i < AG_VT_MAX_PARAMS; i++) {
        s->params[i] = -1;
    }
    s->nparams = 0;
    s->private_seq = false;
}

static void sgr_set_fg(ag_screen_t *s, uint8_t colour)
{
    const uint8_t bright = (uint8_t)(s->attr & 0x08u);
    s->attr = (uint8_t)((s->attr & 0xf0u) | (colour & 0x07u) | bright);
}

static void sgr_set_bg(ag_screen_t *s, uint8_t colour)
{
    s->attr = (uint8_t)((s->attr & 0x0fu) | (uint8_t)((colour & 0x0fu) << 4));
}

static void swap_nibbles(ag_screen_t *s)
{
    s->attr = (uint8_t)(((s->attr & 0x0fu) << 4) | ((s->attr & 0xf0u) >> 4));
}

static void apply_sgr(ag_screen_t *s)
{
    const uint8_t n = (s->nparams == 0) ? 1 : s->nparams;

    for (uint8_t i = 0; i < n; i++) {
        const int32_t p = pget(s, i, 0);

        if (p == 0) {
            s->attr = AG_ATTR_DEFAULT;
            s->reverse = false;
        } else if (p == 1) {
            s->attr |= 0x08u;
        } else if (p == 2 || p == 22) {
            s->attr &= (uint8_t)~0x08u;
        } else if (p == 7) {
            if (!s->reverse) {
                swap_nibbles(s);
                s->reverse = true;
            }
        } else if (p == 27) {
            if (s->reverse) {
                swap_nibbles(s);
                s->reverse = false;
            }
        } else if (p >= 30 && p <= 37) {
            sgr_set_fg(s, k_ansi_to_cga[p - 30]);
        } else if (p == 39) {
            sgr_set_fg(s, AG_LGRAY);
        } else if (p >= 40 && p <= 47) {
            sgr_set_bg(s, k_ansi_to_cga[p - 40]);
        } else if (p == 49) {
            sgr_set_bg(s, AG_BLACK);
        } else if (p >= 90 && p <= 97) {
            s->attr = (uint8_t)((s->attr & 0xf0u) |
                                (k_ansi_to_cga[p - 90] | 0x08u));
        } else if (p >= 100 && p <= 107) {
            sgr_set_bg(s, (uint8_t)(k_ansi_to_cga[p - 100] | 0x08u));
        } else if (p == 38 || p == 48) {
            /*
             * Extended colour.  We only have sixteen, so the value is folded
             * down, but the parameters still have to be consumed or the rest
             * of the sequence would be read as separate attributes.
             */
            const int32_t kind = pget(s, (uint8_t)(i + 1), -1);
            if (kind == 5) {
                const int32_t idx = pget(s, (uint8_t)(i + 2), 7);
                const uint8_t c = (idx >= 0 && idx < 8)
                                      ? k_ansi_to_cga[idx]
                                      : (uint8_t)((idx >= 8 && idx < 16)
                                                      ? (k_ansi_to_cga[idx - 8] |
                                                         0x08u)
                                                      : AG_LGRAY);
                if (p == 38) {
                    sgr_set_fg(s, (uint8_t)(c & 0x07u));
                    s->attr = (uint8_t)((s->attr & ~0x08u) | (c & 0x08u));
                } else {
                    sgr_set_bg(s, c);
                }
                i = (uint8_t)(i + 2);
            } else if (kind == 2) {
                i = (uint8_t)(i + 4); /* 38;2;r;g;b */
            }
        }
    }
}

static void erase_display(ag_screen_t *s, int32_t mode)
{
    if (mode == 0) {
        fill_row(s, s->cur_y, s->cur_x, s->cols, ' ', s->attr);
        for (uint16_t y = (uint16_t)(s->cur_y + 1); y < s->rows; y++) {
            fill_row(s, y, 0, s->cols, ' ', s->attr);
        }
    } else if (mode == 1) {
        for (uint16_t y = 0; y < s->cur_y; y++) {
            fill_row(s, y, 0, s->cols, ' ', s->attr);
        }
        fill_row(s, s->cur_y, 0, (uint16_t)(s->cur_x + 1), ' ', s->attr);
    } else {
        for (uint16_t y = 0; y < s->rows; y++) {
            fill_row(s, y, 0, s->cols, ' ', s->attr);
        }
        ag_screen_mark_all_dirty(s);
    }
}

static void erase_line(ag_screen_t *s, int32_t mode)
{
    if (mode == 0) {
        fill_row(s, s->cur_y, s->cur_x, s->cols, ' ', s->attr);
    } else if (mode == 1) {
        fill_row(s, s->cur_y, 0, (uint16_t)(s->cur_x + 1), ' ', s->attr);
    } else {
        fill_row(s, s->cur_y, 0, s->cols, ' ', s->attr);
    }
}

static void move_cursor(ag_screen_t *s, int dx, int dy)
{
    int nx = (int)s->cur_x + dx;
    int ny = (int)s->cur_y + dy;

    if (nx < 0) {
        nx = 0;
    }
    if (ny < 0) {
        ny = 0;
    }
    if (nx >= (int)s->cols) {
        nx = s->cols - 1;
    }
    if (ny >= (int)s->rows) {
        ny = s->rows - 1;
    }

    s->cur_x = (uint16_t)nx;
    s->cur_y = (uint16_t)ny;
    s->pending_wrap = false;
}

static void csi_dispatch(ag_screen_t *s, char final)
{
    switch (final) {
    case 'A': move_cursor(s, 0, -(int)pget(s, 0, 1)); break;
    case 'B': move_cursor(s, 0, (int)pget(s, 0, 1)); break;
    case 'C': move_cursor(s, (int)pget(s, 0, 1), 0); break;
    case 'D': move_cursor(s, -(int)pget(s, 0, 1), 0); break;

    case 'E': /* next line, column 1 */
        move_cursor(s, 0, (int)pget(s, 0, 1));
        s->cur_x = 0;
        break;
    case 'F': /* previous line, column 1 */
        move_cursor(s, 0, -(int)pget(s, 0, 1));
        s->cur_x = 0;
        break;

    case 'G': /* absolute column */
        ag_screen_gotoxy(s, (uint16_t)(pget(s, 0, 1) - 1), s->cur_y);
        break;
    case 'd': /* absolute row */
        ag_screen_gotoxy(s, s->cur_x, (uint16_t)(pget(s, 0, 1) - 1));
        break;

    case 'H':
    case 'f':
        ag_screen_gotoxy(s, (uint16_t)(pget(s, 1, 1) - 1),
                         (uint16_t)(pget(s, 0, 1) - 1));
        break;

    case 'J': erase_display(s, pget(s, 0, 0)); break;
    case 'K': erase_line(s, pget(s, 0, 0)); break;

    case 'S': ag_screen_scroll_up(s, (uint16_t)pget(s, 0, 1)); break;
    case 'T': scroll_down(s, (uint16_t)pget(s, 0, 1)); break;

    case 'm': apply_sgr(s); break;

    case 'h':
    case 'l':
        if (s->private_seq && pget(s, 0, 0) == 25) {
            ag_screen_set_cursor(s, final == 'h');
        }
        break;

    case 's':
        s->saved_x = s->cur_x;
        s->saved_y = s->cur_y;
        s->saved_attr = s->attr;
        break;
    case 'u':
        s->cur_x = s->saved_x;
        s->cur_y = s->saved_y;
        s->attr = s->saved_attr;
        s->pending_wrap = false;
        break;

    default:
        /* Unknown sequences are dropped rather than printed as garbage. */
        break;
    }
}

static void ground_char(ag_screen_t *s, char ch)
{
    switch (ch) {
    case '\n':
        /*
         * The screen is the terminal, and a bare LF from an application is
         * always meant as "start of the next line" - the same thing the DOS
         * CON device did.
         */
        newline(s);
        break;

    case '\r':
        s->cur_x = 0;
        s->pending_wrap = false;
        break;

    case '\t': {
        uint16_t next = (uint16_t)((s->cur_x / AG_TAB_WIDTH + 1) *
                                   AG_TAB_WIDTH);
        if (next >= s->cols) {
            next = (uint16_t)(s->cols - 1);
        }
        s->cur_x = next;
        s->pending_wrap = false;
        break;
    }

    case '\b':
        if (s->cur_x > 0) {
            s->cur_x--;
        }
        s->pending_wrap = false;
        break;

    case '\a':
        break; /* BEL: the console driver turns this into a beep event */

    case 0x1b:
        s->state = AG_VT_ESC;
        break;

    default:
        if ((unsigned char)ch >= 0x80) {
            /*
             * A cell holds one byte, so a multi-byte code point cannot be
             * stored as itself yet.  Storing one placeholder per code point
             * at least keeps the column count honest, which storing each
             * byte in its own cell would not.
             *
             * The real fix is a single-byte code page for the screen, CP437
             * by default and CP866 for Cyrillic, the way DOS did it; see
             * docs/04-roadmap.md.  Continuation bytes are skipped here, which
             * also makes a code point split across two writes work.
             */
            if (((unsigned char)ch & 0xc0u) != 0x80u) {
                ag_screen_putc_raw(s, '?');
            }
        } else if ((unsigned char)ch >= 0x20) {
            ag_screen_putc_raw(s, ch);
        }
        break;
    }
}

void ag_screen_write(ag_screen_t *s, const char *buf, size_t len)
{
    if (s == NULL || buf == NULL) {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        const char ch = buf[i];

        switch (s->state) {
        case AG_VT_GROUND:
            ground_char(s, ch);
            break;

        case AG_VT_ESC:
            switch (ch) {
            case '[':
                csi_reset(s);
                s->state = AG_VT_CSI;
                break;
            case ']':
                s->state = AG_VT_OSC;
                break;
            case '(':
            case ')':
            case '#':
                s->state = AG_VT_ESC_SKIP1;
                break;
            case '7':
                s->saved_x = s->cur_x;
                s->saved_y = s->cur_y;
                s->saved_attr = s->attr;
                s->state = AG_VT_GROUND;
                break;
            case '8':
                s->cur_x = s->saved_x;
                s->cur_y = s->saved_y;
                s->attr = s->saved_attr;
                s->pending_wrap = false;
                s->state = AG_VT_GROUND;
                break;
            case 'c':
                s->attr = AG_ATTR_DEFAULT;
                s->reverse = false;
                ag_screen_cls(s);
                s->state = AG_VT_GROUND;
                break;
            case 'M': /* reverse index */
                if (s->cur_y == 0) {
                    scroll_down(s, 1);
                } else {
                    s->cur_y--;
                }
                s->pending_wrap = false;
                s->state = AG_VT_GROUND;
                break;
            default:
                s->state = AG_VT_GROUND;
                break;
            }
            break;

        case AG_VT_ESC_SKIP1:
            s->state = AG_VT_GROUND;
            break;

        case AG_VT_CSI:
            if (ch >= '0' && ch <= '9') {
                if (s->nparams == 0) {
                    s->nparams = 1;
                }
                int32_t *p = &s->params[s->nparams - 1];
                const int32_t base = (*p < 0) ? 0 : *p;
                if (base < 100000) { /* clamp instead of overflowing */
                    *p = base * 10 + (ch - '0');
                }
            } else if (ch == ';') {
                if (s->nparams == 0) {
                    s->nparams = 1;
                }
                if (s->nparams < AG_VT_MAX_PARAMS) {
                    s->nparams++;
                }
            } else if (ch == '?' || ch == '<' || ch == '>' || ch == '=') {
                s->private_seq = true;
            } else if ((unsigned char)ch >= 0x40 && (unsigned char)ch <= 0x7e) {
                csi_dispatch(s, ch);
                s->state = AG_VT_GROUND;
            } else if ((unsigned char)ch < 0x20) {
                /* Control characters still act while a sequence is open. */
                ground_char(s, ch);
            }
            break;

        case AG_VT_OSC:
            /*
             * Terminated by BEL or by ESC \.  Treating a bare ESC as the end
             * costs nothing and stops a truncated sequence from swallowing
             * the rest of the output.
             */
            if (ch == '\a' || ch == 0x1b) {
                s->state = AG_VT_GROUND;
            }
            break;
        }
    }
}

void ag_screen_puts(ag_screen_t *s, const char *str)
{
    if (str != NULL) {
        ag_screen_write(s, str, strlen(str));
    }
}
