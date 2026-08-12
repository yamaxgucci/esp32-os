/*
 * ArgonOS - VT100 renderer tests.
 *
 * Expectations are written with ESC shown as "^[", which is how the escaped()
 * helper renders both sides of a comparison.  Without that, a failure prints
 * raw escape sequences and reprograms the terminal running the tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/vtout.h>

#include <argon/codepage.h>

#include "test.h"

static uint32_t g_mem[512];
static uint32_t g_mem2[512];

static char   g_cap[8192];
static size_t g_cap_len;

static void cap_sink(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    for (size_t i = 0; i < len && g_cap_len + 1 < sizeof(g_cap); i++) {
        g_cap[g_cap_len++] = data[i];
    }
    g_cap[g_cap_len] = '\0';
}

static void cap_reset(void)
{
    g_cap_len = 0;
    g_cap[0] = '\0';
}

static const char *escaped(const char *s)
{
    static char buf[16384];
    size_t      n = 0;
    for (; *s != '\0' && n + 3 < sizeof(buf); s++) {
        if (*s == 0x1b) {
            buf[n++] = '^';
            buf[n++] = '[';
        } else {
            buf[n++] = *s;
        }
    }
    buf[n] = '\0';
    return buf;
}

static const char *captured(void) { return escaped(g_cap); }

static ag_screen_t g_screen;
static ag_vtout_t  g_out;

static ag_screen_t *screen8x2(void)
{
    AG_CHECK_INT(ag_screen_init(&g_screen, g_mem, sizeof(g_mem), 8, 2), AG_OK);
    return &g_screen;
}

static void test_first_flush_paints_everything(void)
{
    ag_screen_t *s = screen8x2();
    ag_vtout_init(&g_out);
    ag_vtout_mark_all(&g_out);

    ag_screen_puts(s, "Hi");

    cap_reset();
    ag_vtout_flush(&g_out, s, cap_sink, NULL);

    /*
     * Hide the cursor for the repaint, paint both rows, put the cursor where
     * the screen says it is, show it again.
     */
    AG_CHECK_STR(captured(),
                 "^[[?25l^[[0m"
                 "^[[1;1HHi^[[K"
                 "^[[2;1H^[[K"
                 "^[[1;3H"
                 "^[[?25h");
    AG_CHECK(!ag_vtout_pending(&g_out));
}

static void test_incremental_update(void)
{
    ag_screen_t *s = screen8x2();
    ag_vtout_init(&g_out);
    ag_vtout_mark_all(&g_out);
    ag_screen_puts(s, "Hi");

    cap_reset();
    ag_vtout_flush(&g_out, s, cap_sink, NULL);
    ag_screen_clear_dirty(s);

    /* One more character must not repaint the second row. */
    ag_screen_puts(s, "!");
    ag_vtout_take_dirty(&g_out, s);

    cap_reset();
    ag_vtout_flush(&g_out, s, cap_sink, NULL);
    AG_CHECK_STR(captured(), "^[[1;1HHi!^[[K^[[1;4H");
}

static void test_nothing_to_do(void)
{
    ag_screen_t *s = screen8x2();
    ag_vtout_init(&g_out);
    ag_vtout_mark_all(&g_out);
    ag_screen_puts(s, "Hi");
    ag_vtout_flush(&g_out, s, cap_sink, NULL);
    ag_screen_clear_dirty(s);

    /* A flush with no changes and no cursor movement emits nothing at all. */
    cap_reset();
    ag_vtout_take_dirty(&g_out, s);
    ag_vtout_flush(&g_out, s, cap_sink, NULL);
    AG_CHECK_STR(captured(), "");

    /* Moving only the cursor emits only the cursor move. */
    ag_screen_gotoxy(s, 5, 1);
    ag_vtout_take_dirty(&g_out, s);
    cap_reset();
    ag_vtout_flush(&g_out, s, cap_sink, NULL);
    AG_CHECK_STR(captured(), "^[[2;6H");
}

static void test_attributes(void)
{
    ag_screen_t *s = screen8x2();
    ag_vtout_init(&g_out);
    ag_vtout_mark_all(&g_out);

    ag_screen_set_attr(s, AG_ATTR(AG_RED, AG_BLACK));
    ag_screen_puts(s, "R");

    cap_reset();
    ag_vtout_flush(&g_out, s, cap_sink, NULL);
    /* Red on default background: no background code needed after the reset. */
    AG_CHECK_STR(captured(),
                 "^[[?25l^[[0m"
                 "^[[1;1H^[[0;31mR^[[0;37m^[[K"
                 "^[[2;1H^[[K"
                 "^[[1;2H"
                 "^[[?25h");

    /* Bright foreground uses the 90 range, background the 40 range. */
    ag_screen_t *s2 = screen8x2();
    ag_vtout_init(&g_out);
    ag_vtout_mark_all(&g_out);
    ag_screen_set_attr(s2, AG_ATTR(AG_LGREEN, AG_BLUE));
    ag_screen_puts(s2, "G");

    cap_reset();
    ag_vtout_flush(&g_out, s2, cap_sink, NULL);
    AG_CHECK(strstr(g_cap, "\x1b[0;92;44mG") != NULL);

    /* A bright background uses the 100 range. */
    ag_screen_t *s3 = screen8x2();
    ag_vtout_init(&g_out);
    ag_vtout_mark_all(&g_out);
    ag_screen_set_attr(s3, AG_ATTR(AG_BLACK, AG_DGRAY));
    ag_screen_puts(s3, "K");
    cap_reset();
    ag_vtout_flush(&g_out, s3, cap_sink, NULL);
    AG_CHECK(strstr(g_cap, "\x1b[0;30;100mK") != NULL);
}

static void test_erase_resets_colour_first(void)
{
    ag_screen_t *s = screen8x2();
    ag_vtout_init(&g_out);
    ag_vtout_mark_all(&g_out);

    /*
     * Erase-to-end-of-line paints with the current background, so a coloured
     * run must be reset before it or the colour smears across the row.
     */
    ag_screen_set_attr(s, AG_ATTR(AG_WHITE, AG_RED));
    ag_screen_puts(s, "X");

    cap_reset();
    ag_vtout_flush(&g_out, s, cap_sink, NULL);
    AG_CHECK(strstr(g_cap, "X\x1b[0;37m\x1b[K") != NULL);
}

static void test_cursor_visibility(void)
{
    ag_screen_t *s = screen8x2();
    ag_vtout_init(&g_out);
    ag_vtout_mark_all(&g_out);
    ag_vtout_flush(&g_out, s, cap_sink, NULL);
    ag_screen_clear_dirty(s);

    ag_screen_set_cursor(s, false);
    cap_reset();
    ag_vtout_take_dirty(&g_out, s);
    ag_vtout_flush(&g_out, s, cap_sink, NULL);
    AG_CHECK_STR(captured(), "^[[?25l");

    ag_screen_set_cursor(s, true);
    cap_reset();
    ag_vtout_flush(&g_out, s, cap_sink, NULL);
    AG_CHECK_STR(captured(), "^[[?25h");
}

static void test_endpoints_are_independent(void)
{
    ag_screen_t *s = screen8x2();

    ag_vtout_t uart;
    ag_vtout_t telnet;
    ag_vtout_init(&uart);
    ag_vtout_init(&telnet);
    ag_vtout_mark_all(&uart);
    ag_vtout_mark_all(&telnet);

    ag_screen_puts(s, "A");
    ag_vtout_take_dirty(&uart, s);
    ag_vtout_take_dirty(&telnet, s);
    ag_screen_clear_dirty(s);

    /* The fast endpoint drains; the slow one still owes the same rows. */
    cap_reset();
    ag_vtout_flush(&uart, s, cap_sink, NULL);
    AG_CHECK(!ag_vtout_pending(&uart));
    AG_CHECK(ag_vtout_pending(&telnet));

    cap_reset();
    ag_vtout_flush(&telnet, s, cap_sink, NULL);
    AG_CHECK(!ag_vtout_pending(&telnet));
    AG_CHECK(strstr(g_cap, "A") != NULL);
}

static void test_terminal_smaller_than_screen(void)
{
    ag_screen_t s;
    AG_CHECK_INT(ag_screen_init(&s, g_mem2, sizeof(g_mem2), 20, 5), AG_OK);
    ag_screen_puts(&s, "0123456789ABCDEF\nrow1\nrow2\nrow3\nrow4");

    ag_vtout_t o;
    ag_vtout_init(&o);
    o.cols = 8;
    o.rows = 2;
    ag_vtout_mark_all(&o);

    cap_reset();
    ag_vtout_flush(&o, &s, cap_sink, NULL);

    /* Only the visible window is sent, and the rest is not left pending. */
    AG_CHECK(strstr(g_cap, "01234567") != NULL);
    AG_CHECK(strstr(g_cap, "89AB") == NULL);
    AG_CHECK(strstr(g_cap, "row1") != NULL);
    AG_CHECK(strstr(g_cap, "row2") == NULL);
    AG_CHECK(!ag_vtout_pending(&o));
}

static void test_hello_and_goodbye(void)
{
    ag_screen_t *s = screen8x2();
    ag_screen_puts(s, "Hi");
    ag_screen_clear_dirty(s);

    ag_vtout_t o;
    cap_reset();
    ag_vtout_hello(&o, cap_sink, NULL);
    AG_CHECK_STR(captured(), "^[[0m^[[?7h^[[2J^[[H");

    /* A terminal that just attached owes the whole screen. */
    AG_CHECK(ag_vtout_pending(&o));
    cap_reset();
    ag_vtout_flush(&o, s, cap_sink, NULL);
    AG_CHECK(strstr(g_cap, "Hi") != NULL);

    cap_reset();
    ag_vtout_goodbye(cap_sink, NULL);
    AG_CHECK_STR(captured(), "^[[0m^[[?25h\r\n");
}

static void test_control_characters_are_not_forwarded(void)
{
    ag_screen_t *s = screen8x2();
    ag_vtout_init(&g_out);
    ag_vtout_mark_all(&g_out);

    /* A raw poke can put anything in a cell; it must not reach the terminal. */
    ag_screen_poke(s, 0, 0, '\x1b', AG_ATTR_DEFAULT);
    ag_screen_poke(s, 1, 0, '\x07', AG_ATTR_DEFAULT);
    ag_screen_poke(s, 2, 0, 'Z', AG_ATTR_DEFAULT);

    cap_reset();
    ag_vtout_flush(&g_out, s, cap_sink, NULL);
    AG_CHECK(strstr(g_cap, "\x1b[1;1H  Z") != NULL);
    AG_CHECK(strchr(g_cap, '\x07') == NULL);
}

/*
 * The screen holds one code page byte per cell; a terminal speaks UTF-8.  The
 * renderer is the place that converts, and the column arithmetic must keep
 * counting cells while the byte count grows - a Cyrillic letter is two bytes on
 * the wire and one column on the glass.
 */
static void test_high_bytes_go_out_as_utf8(void)
{
    ag_screen_t *s = screen8x2();
    ag_vtout_init(&g_out);
    ag_vtout_mark_all(&g_out);

    ag_cp_set_active(AG_CP_866);
    ag_screen_poke(s, 0, 0, (char)0xa4, AG_ATTR_DEFAULT); /* д */
    ag_screen_poke(s, 1, 0, (char)0xa0, AG_ATTR_DEFAULT); /* а */
    ag_screen_poke(s, 2, 0, (char)0xc4, AG_ATTR_DEFAULT); /* ─ */

    cap_reset();
    ag_vtout_flush(&g_out, s, cap_sink, NULL);

    /* d0 b4, d0 b0, e2 94 80 - and then the row ends after three columns. */
    AG_CHECK(strstr(g_cap, "\x1b[1;1H\xd0\xb4\xd0\xb0\xe2\x94\x80\x1b[K") !=
             NULL);

    /* The same bytes in CP437 are other characters entirely, which is the point
     * of having a page at all: nothing was rewritten, only reinterpreted. */
    ag_cp_set_active(AG_CP_437);
    ag_vtout_mark_all(&g_out);
    cap_reset();
    ag_vtout_flush(&g_out, s, cap_sink, NULL);
    AG_CHECK(strstr(g_cap, "\xd0\xb4") == NULL);
    /* 0xc4 in CP437 is the same box character, so that one survives. */
    AG_CHECK(strstr(g_cap, "\xe2\x94\x80") != NULL);
}

/* CP1251 leaves 0x98 undefined; an undefined byte must not become U+0000. */
static void test_undefined_byte_renders_as_space(void)
{
    ag_screen_t *s = screen8x2();
    ag_vtout_init(&g_out);
    ag_vtout_mark_all(&g_out);

    ag_cp_set_active(AG_CP_1251);
    ag_screen_poke(s, 0, 0, (char)0x98, AG_ATTR_DEFAULT);
    ag_screen_poke(s, 1, 0, 'Z', AG_ATTR_DEFAULT);

    cap_reset();
    ag_vtout_flush(&g_out, s, cap_sink, NULL);
    AG_CHECK(strstr(g_cap, "\x1b[1;1H Z") != NULL);
    AG_CHECK(strchr(g_cap, '\0') == g_cap + g_cap_len);

    ag_cp_set_active(AG_CP_437);
}

void run_vtout_tests(void)
{
    test_high_bytes_go_out_as_utf8();
    test_undefined_byte_renders_as_space();
    test_first_flush_paints_everything();
    test_incremental_update();
    test_nothing_to_do();
    test_attributes();
    test_erase_resets_colour_first();
    test_cursor_visibility();
    test_endpoints_are_independent();
    test_terminal_smaller_than_screen();
    test_hello_and_goodbye();
    test_control_characters_are_not_forwarded();
}
