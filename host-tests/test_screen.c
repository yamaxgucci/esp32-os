/*
 * ArgonOS - virtual text screen tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/screen.h>

#include "test.h"

/* uint32_t storage guarantees the alignment ag_screen_init() insists on. */
static uint32_t g_small_mem[256];  /* enough for 20x5  */
static uint32_t g_big_mem[1200];   /* enough for 80x25 */

static ag_screen_t g_s;

static ag_screen_t *small_screen(void)
{
    AG_CHECK_INT(ag_screen_init(&g_s, g_small_mem, sizeof(g_small_mem), 20, 5),
                 AG_OK);
    return &g_s;
}

static ag_screen_t *big_screen(void)
{
    AG_CHECK_INT(ag_screen_init(&g_s, g_big_mem, sizeof(g_big_mem), 80, 25),
                 AG_OK);
    return &g_s;
}

/* Row contents with trailing blanks removed, so expectations stay readable. */
static const char *row_text(const ag_screen_t *s, uint16_t y)
{
    static char buf[AG_SCREEN_MAX_COLS + 1];
    const ag_cell_t *row = ag_screen_row(s, y);
    if (row == NULL) {
        return "<no such row>";
    }
    for (uint16_t x = 0; x < s->cols; x++) {
        buf[x] = row[x].ch;
    }
    buf[s->cols] = '\0';
    for (int x = (int)s->cols - 1; x >= 0 && buf[x] == ' '; x--) {
        buf[x] = '\0';
    }
    return buf;
}

static void test_init(void)
{
    /* One dirty word for 25 rows, plus two bytes per cell. */
    AG_CHECK_INT(ag_screen_memsize(80, 25), 4 + 80 * 25 * 2);
    AG_CHECK_INT(ag_screen_memsize(20, 5), 4 + 20 * 5 * 2);
    AG_CHECK_INT(ag_screen_memsize(80, 64), 8 + 80 * 64 * 2);

    ag_screen_t s;
    AG_CHECK_INT(ag_screen_init(&s, g_small_mem, 8, 20, 5), -AG_ERANGE);
    AG_CHECK_INT(ag_screen_init(&s, g_small_mem, sizeof(g_small_mem), 0, 5),
                 -AG_EINVAL);
    AG_CHECK_INT(ag_screen_init(&s, NULL, 1024, 20, 5), -AG_EINVAL);
    AG_CHECK_INT(ag_screen_init(&s, (uint8_t *)g_small_mem + 1,
                                sizeof(g_small_mem) - 1, 20, 5),
                 -AG_EINVAL);

    ag_screen_t *ok = small_screen();
    AG_CHECK_INT(ok->cols, 20);
    AG_CHECK_INT(ok->rows, 5);
    AG_CHECK_INT(ok->attr, AG_ATTR_DEFAULT);
    AG_CHECK_STR(row_text(ok, 0), "");
    AG_CHECK(ok->cursor_visible);
}

static void test_plain_text(void)
{
    ag_screen_t *s = small_screen();

    ag_screen_puts(s, "Hello");
    AG_CHECK_STR(row_text(s, 0), "Hello");
    AG_CHECK_INT(s->cur_x, 5);
    AG_CHECK_INT(s->cur_y, 0);

    ag_screen_puts(s, "\nworld");
    AG_CHECK_STR(row_text(s, 0), "Hello");
    AG_CHECK_STR(row_text(s, 1), "world");
    AG_CHECK_INT(s->cur_x, 5);
    AG_CHECK_INT(s->cur_y, 1);
}

static void test_control_chars(void)
{
    ag_screen_t *s = small_screen();

    /* CR returns to column zero without erasing. */
    ag_screen_puts(s, "abc\rX");
    AG_CHECK_STR(row_text(s, 0), "Xbc");

    s = small_screen();
    ag_screen_puts(s, "a\tb");
    AG_CHECK_INT(ag_screen_at(s, 8, 0).ch, 'b');
    AG_CHECK_INT(ag_screen_at(s, 1, 0).ch, ' ');

    /* Backspace moves the cursor but does not erase, like a real terminal. */
    s = small_screen();
    ag_screen_puts(s, "ab\bc");
    AG_CHECK_STR(row_text(s, 0), "ac");

    /* Backspace at the left margin stays put. */
    s = small_screen();
    ag_screen_puts(s, "\b\bx");
    AG_CHECK_STR(row_text(s, 0), "x");

    /* BEL and other C0 characters produce nothing visible. */
    s = small_screen();
    /* Split so that "\x01" does not swallow the following 'c' as a hex digit. */
    ag_screen_puts(s, "a\ab\x01" "c");
    AG_CHECK_STR(row_text(s, 0), "abc");
}

static void test_deferred_wrap(void)
{
    ag_screen_t *s = small_screen();

    /* Exactly one screen width must not spill onto the next line. */
    ag_screen_puts(s, "12345678901234567890");
    AG_CHECK_STR(row_text(s, 0), "12345678901234567890");
    AG_CHECK_STR(row_text(s, 1), "");
    AG_CHECK_INT(s->cur_y, 0);
    AG_CHECK_INT(s->cur_x, 19);
    AG_CHECK(s->pending_wrap);

    /* The next character is what actually wraps. */
    ag_screen_puts(s, "A");
    AG_CHECK_STR(row_text(s, 1), "A");
    AG_CHECK_INT(s->cur_y, 1);
    AG_CHECK_INT(s->cur_x, 1);
    AG_CHECK(!s->pending_wrap);

    /* A cursor move disarms the pending wrap. */
    s = small_screen();
    ag_screen_puts(s, "12345678901234567890\rZ");
    AG_CHECK_STR(row_text(s, 0), "Z2345678901234567890");
    AG_CHECK_STR(row_text(s, 1), "");
}

static void test_scroll(void)
{
    ag_screen_t *s = small_screen();

    ag_screen_puts(s, "r0\nr1\nr2\nr3\nr4");
    AG_CHECK_STR(row_text(s, 0), "r0");
    AG_CHECK_STR(row_text(s, 4), "r4");
    AG_CHECK_INT(s->cur_y, 4);

    /* The sixth line pushes the first one off the top. */
    ag_screen_puts(s, "\nr5");
    AG_CHECK_STR(row_text(s, 0), "r1");
    AG_CHECK_STR(row_text(s, 4), "r5");
    AG_CHECK_INT(s->cur_y, 4);

    /* Scrolling by more than the height just blanks everything. */
    ag_screen_scroll_up(s, 99);
    AG_CHECK_STR(row_text(s, 0), "");
    AG_CHECK_STR(row_text(s, 4), "");
}

static void test_sgr_colours(void)
{
    ag_screen_t *s = small_screen();

    ag_screen_puts(s, "\x1b[31mR");
    AG_CHECK_INT(ag_screen_at(s, 0, 0).attr, AG_ATTR(AG_RED, AG_BLACK));

    /* Bold turns into the bright bit of the PC attribute byte. */
    ag_screen_puts(s, "\x1b[1;32mG");
    AG_CHECK_INT(ag_screen_at(s, 1, 0).attr, AG_ATTR(AG_LGREEN, AG_BLACK));

    ag_screen_puts(s, "\x1b[44mB");
    AG_CHECK_INT(ag_screen_at(s, 2, 0).attr, AG_ATTR(AG_LGREEN, AG_BLUE));

    ag_screen_puts(s, "\x1b[0mD");
    AG_CHECK_INT(ag_screen_at(s, 3, 0).attr, AG_ATTR_DEFAULT);

    /* Bright foreground via the 90-97 range. */
    ag_screen_puts(s, "\x1b[93mY");
    AG_CHECK_INT(ag_screen_at(s, 4, 0).attr, AG_ATTR(AG_YELLOW, AG_BLACK));

    /* A bare ESC[m is a reset. */
    ag_screen_puts(s, "\x1b[mZ");
    AG_CHECK_INT(ag_screen_at(s, 5, 0).attr, AG_ATTR_DEFAULT);
}

static void test_sgr_reverse(void)
{
    ag_screen_t *s = small_screen();

    ag_screen_puts(s, "\x1b[31;47m");
    const uint8_t normal = AG_ATTR(AG_RED, AG_LGRAY);
    AG_CHECK_INT(s->attr, normal);

    ag_screen_puts(s, "\x1b[7mX");
    AG_CHECK_INT(ag_screen_at(s, 0, 0).attr, AG_ATTR(AG_LGRAY, AG_RED));

    /* Reverse must not toggle twice for two SGR 7 in a row. */
    ag_screen_puts(s, "\x1b[7mY");
    AG_CHECK_INT(ag_screen_at(s, 1, 0).attr, AG_ATTR(AG_LGRAY, AG_RED));

    ag_screen_puts(s, "\x1b[27mZ");
    AG_CHECK_INT(ag_screen_at(s, 2, 0).attr, normal);
}

static void test_sgr_extended_is_consumed(void)
{
    ag_screen_t *s = small_screen();

    /*
     * We only have sixteen colours, but the parameters of a 256-colour or
     * truecolour sequence still have to be eaten, or the trailing values
     * would be read as separate attributes.
     */
    ag_screen_puts(s, "\x1b[38;5;9;44mX");
    AG_CHECK_INT(ag_screen_at(s, 0, 0).attr & 0xf0u,
                 AG_ATTR(AG_BLACK, AG_BLUE));

    ag_screen_puts(s, "\x1b[0m\x1b[38;2;255;0;0;42mY");
    AG_CHECK_INT(ag_screen_at(s, 1, 0).attr & 0xf0u,
                 AG_ATTR(AG_BLACK, AG_GREEN));
}

static void test_cursor_sequences(void)
{
    ag_screen_t *s = big_screen();

    ag_screen_puts(s, "\x1b[5;10H");
    AG_CHECK_INT(s->cur_x, 9);
    AG_CHECK_INT(s->cur_y, 4);

    ag_screen_puts(s, "\x1b[H");
    AG_CHECK_INT(s->cur_x, 0);
    AG_CHECK_INT(s->cur_y, 0);

    ag_screen_puts(s, "\x1b[3B\x1b[5C");
    AG_CHECK_INT(s->cur_x, 5);
    AG_CHECK_INT(s->cur_y, 3);

    ag_screen_puts(s, "\x1b[2A\x1b[2D");
    AG_CHECK_INT(s->cur_x, 3);
    AG_CHECK_INT(s->cur_y, 1);

    /* Movement clamps at the edges instead of wrapping or going negative. */
    ag_screen_puts(s, "\x1b[99A\x1b[99D");
    AG_CHECK_INT(s->cur_x, 0);
    AG_CHECK_INT(s->cur_y, 0);
    ag_screen_puts(s, "\x1b[999B\x1b[999C");
    AG_CHECK_INT(s->cur_x, 79);
    AG_CHECK_INT(s->cur_y, 24);

    /* Absolute column and row. */
    ag_screen_puts(s, "\x1b[7G\x1b[3d");
    AG_CHECK_INT(s->cur_x, 6);
    AG_CHECK_INT(s->cur_y, 2);

    /* Save and restore, both the CSI and the ESC 7/8 spelling. */
    ag_screen_puts(s, "\x1b[s\x1b[1;1H\x1b[u");
    AG_CHECK_INT(s->cur_x, 6);
    AG_CHECK_INT(s->cur_y, 2);
    /* Split so the digit is not absorbed into the hex escape. */
    ag_screen_puts(s, "\x1b" "7" "\x1b[20;20H" "\x1b" "8");
    AG_CHECK_INT(s->cur_x, 6);
    AG_CHECK_INT(s->cur_y, 2);

    /* DECTCEM. */
    ag_screen_puts(s, "\x1b[?25l");
    AG_CHECK(!s->cursor_visible);
    ag_screen_puts(s, "\x1b[?25h");
    AG_CHECK(s->cursor_visible);
}

static void test_erase_sequences(void)
{
    ag_screen_t *s = small_screen();

    ag_screen_puts(s, "abcdef\x1b[1;4H\x1b[K");
    AG_CHECK_STR(row_text(s, 0), "abc");

    s = small_screen();
    ag_screen_puts(s, "abcdef\x1b[1;4H\x1b[1K");
    AG_CHECK_STR(row_text(s, 0), "    ef");

    s = small_screen();
    ag_screen_puts(s, "r0\nr1\nr2\x1b[2;2H\x1b[J");
    AG_CHECK_STR(row_text(s, 0), "r0");
    AG_CHECK_STR(row_text(s, 1), "r");
    AG_CHECK_STR(row_text(s, 2), "");

    /* ED 2 clears everything and, unlike DOS CLS, leaves the cursor alone. */
    s = small_screen();
    ag_screen_puts(s, "r0\nr1\x1b[2;2H\x1b[2J");
    AG_CHECK_STR(row_text(s, 0), "");
    AG_CHECK_STR(row_text(s, 1), "");
    AG_CHECK_INT(s->cur_x, 1);
    AG_CHECK_INT(s->cur_y, 1);
}

static void test_malformed_sequences(void)
{
    ag_screen_t *s = small_screen();

    /* An unknown final byte drops the sequence rather than printing it. */
    ag_screen_puts(s, "a\x1b[999zb");
    AG_CHECK_STR(row_text(s, 0), "ab");

    /* An OSC string is swallowed up to its terminator. */
    s = small_screen();
    ag_screen_puts(s, "a\x1b]0;window title\ab");
    AG_CHECK_STR(row_text(s, 0), "ab");

    /* A truncated OSC must not eat the rest of the output forever. */
    s = small_screen();
    ag_screen_puts(s, "a\x1b]0;no terminator\x1b[31mb");
    AG_CHECK(s->state == AG_VT_GROUND || s->state == AG_VT_CSI);

    /* Charset selection takes one byte with it. */
    s = small_screen();
    ag_screen_puts(s, "a\x1b(Bb");
    AG_CHECK_STR(row_text(s, 0), "ab");

    /* More parameters than we store must not corrupt anything. */
    s = small_screen();
    ag_screen_puts(s, "\x1b[1;2;3;4;5;6;7;8;9;10;11mX");
    AG_CHECK_STR(row_text(s, 0), "X");

    /* A sequence split across two writes still works. */
    s = small_screen();
    ag_screen_write(s, "\x1b[3", 3);
    ag_screen_write(s, "1mR", 3);
    AG_CHECK_INT(ag_screen_at(s, 0, 0).attr, AG_ATTR(AG_RED, AG_BLACK));
}

static void test_dirty_tracking(void)
{
    ag_screen_t *s = small_screen();

    /* A fresh screen is entirely dirty: nothing has been drawn yet. */
    AG_CHECK(ag_screen_any_dirty(s));

    ag_screen_clear_dirty(s);
    AG_CHECK(!ag_screen_any_dirty(s));

    ag_screen_gotoxy(s, 0, 3);
    ag_screen_puts(s, "x");
    AG_CHECK(ag_screen_row_dirty(s, 3));
    AG_CHECK(!ag_screen_row_dirty(s, 0));
    AG_CHECK(!ag_screen_row_dirty(s, 4));

    /* Writing the same character again changes nothing, so nothing is dirty. */
    ag_screen_clear_dirty(s);
    ag_screen_gotoxy(s, 0, 3);
    ag_screen_puts(s, "x");
    AG_CHECK(!ag_screen_any_dirty(s));

    /* Scrolling moves every row. */
    ag_screen_clear_dirty(s);
    ag_screen_scroll_up(s, 1);
    for (uint16_t y = 0; y < s->rows; y++) {
        AG_CHECK(ag_screen_row_dirty(s, y));
    }

    /* Rows past 32 need the second word of the bitmap. */
    ag_screen_t big;
    AG_CHECK_INT(ag_screen_init(&big, g_big_mem, sizeof(g_big_mem), 40, 40),
                 AG_OK);
    ag_screen_clear_dirty(&big);
    ag_screen_gotoxy(&big, 0, 35);
    ag_screen_puts(&big, "y");
    AG_CHECK(ag_screen_row_dirty(&big, 35));
    AG_CHECK(!ag_screen_row_dirty(&big, 3));
}

static void test_poke_and_fill(void)
{
    ag_screen_t *s = small_screen();

    ag_screen_poke(s, 2, 1, 'Q', AG_ATTR(AG_WHITE, AG_RED));
    AG_CHECK_INT(ag_screen_at(s, 2, 1).ch, 'Q');
    AG_CHECK_INT(ag_screen_at(s, 2, 1).attr, AG_ATTR(AG_WHITE, AG_RED));

    /* Out of range writes are ignored, not clamped into a neighbour. */
    ag_screen_poke(s, 999, 1, 'Z', 0);
    ag_screen_poke(s, 2, 999, 'Z', 0);
    AG_CHECK_INT(ag_screen_at(s, 2, 1).ch, 'Q');

    ag_screen_fill(s, 0, 2, 5, 2, '#', AG_ATTR_DEFAULT);
    AG_CHECK_STR(row_text(s, 2), "#####");
    AG_CHECK_STR(row_text(s, 3), "#####");
    AG_CHECK_STR(row_text(s, 4), "");

    /* A fill wider than the screen is clipped. */
    ag_screen_fill(s, 18, 4, 99, 1, '*', AG_ATTR_DEFAULT);
    AG_CHECK_STR(row_text(s, 4), "                  **");
}

void run_screen_tests(void)
{
    test_init();
    test_plain_text();
    test_control_chars();
    test_deferred_wrap();
    test_scroll();
    test_sgr_colours();
    test_sgr_reverse();
    test_sgr_extended_is_consumed();
    test_cursor_sequences();
    test_erase_sequences();
    test_malformed_sequences();
    test_dirty_tracking();
    test_poke_and_fill();
}
