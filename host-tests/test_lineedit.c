/*
 * ArgonOS - command line editing tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/lineedit.h>

#include <argon/codepage.h>

#include "test.h"

static ag_lineedit_t g_le;

static ag_line_result_t key(uint16_t keycode, uint32_t unicode, uint16_t mods)
{
    ag_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = AG_EV_KEY_DOWN;
    ev.key.keycode = keycode;
    ev.key.unicode = unicode;
    ev.key.mods = mods;
    return ag_lineedit_key(&g_le, &ev);
}

/* Types printable ASCII, the way the input decoder would deliver it. */
static void type(const char *s)
{
    for (; *s != '\0'; s++) {
        key(AG_KEY_NONE, (uint32_t)(unsigned char)*s, 0);
    }
}

static void fresh(const char *initial)
{
    ag_lineedit_init(&g_le);
    if (initial != NULL) {
        type(initial);
    }
}

static void test_typing(void)
{
    fresh(NULL);
    AG_CHECK_INT(key(AG_KEY_A, 'a', 0), AG_LINE_CHANGED);
    type("bc");
    AG_CHECK_STR(g_le.buf, "abc");
    AG_CHECK_INT(g_le.len, 3);
    AG_CHECK_INT(g_le.cursor, 3);

    /* Insertion happens at the cursor, not at the end. */
    key(AG_KEY_LEFT, 0, 0);
    type("X");
    AG_CHECK_STR(g_le.buf, "abXc");
    AG_CHECK_INT(g_le.cursor, 3);
}

static void test_erasing(void)
{
    fresh("hello");
    AG_CHECK_INT(key(AG_KEY_BACKSPACE, 0, 0), AG_LINE_CHANGED);
    AG_CHECK_STR(g_le.buf, "hell");

    /* Backspace at the start of the line does nothing. */
    key(AG_KEY_HOME, 0, 0);
    AG_CHECK_INT(key(AG_KEY_BACKSPACE, 0, 0), AG_LINE_IDLE);
    AG_CHECK_STR(g_le.buf, "hell");

    /* Delete removes forwards. */
    AG_CHECK_INT(key(AG_KEY_DELETE, 0, 0), AG_LINE_CHANGED);
    AG_CHECK_STR(g_le.buf, "ell");
    key(AG_KEY_END, 0, 0);
    AG_CHECK_INT(key(AG_KEY_DELETE, 0, 0), AG_LINE_IDLE);
}

static void test_cursor_movement(void)
{
    fresh("abcdef");
    AG_CHECK_INT(g_le.cursor, 6);

    key(AG_KEY_HOME, 0, 0);
    AG_CHECK_INT(g_le.cursor, 0);
    AG_CHECK_INT(key(AG_KEY_HOME, 0, 0), AG_LINE_IDLE);

    key(AG_KEY_RIGHT, 0, 0);
    key(AG_KEY_RIGHT, 0, 0);
    AG_CHECK_INT(g_le.cursor, 2);

    key(AG_KEY_END, 0, 0);
    AG_CHECK_INT(g_le.cursor, 6);
    AG_CHECK_INT(key(AG_KEY_RIGHT, 0, 0), AG_LINE_IDLE);

    /* Ctrl+A and Ctrl+E, for the fingers that expect them. */
    key(AG_KEY_A, 0, AG_MOD_CTRL);
    AG_CHECK_INT(g_le.cursor, 0);
    key(AG_KEY_E, 0, AG_MOD_CTRL);
    AG_CHECK_INT(g_le.cursor, 6);
}

static void test_word_operations(void)
{
    fresh("copy one two");

    /* Ctrl+W removes the word before the cursor. */
    AG_CHECK_INT(key(AG_KEY_W, 0, AG_MOD_CTRL), AG_LINE_CHANGED);
    AG_CHECK_STR(g_le.buf, "copy one ");
    key(AG_KEY_W, 0, AG_MOD_CTRL);
    AG_CHECK_STR(g_le.buf, "copy ");

    /* Ctrl+Left and Ctrl+Right move by word. */
    fresh("copy one two");
    key(AG_KEY_LEFT, 0, AG_MOD_CTRL);
    AG_CHECK_INT(g_le.cursor, 9);
    key(AG_KEY_LEFT, 0, AG_MOD_CTRL);
    AG_CHECK_INT(g_le.cursor, 5);
    key(AG_KEY_RIGHT, 0, AG_MOD_CTRL);
    AG_CHECK_INT(g_le.cursor, 9);
}

static void test_kill_line(void)
{
    fresh("some text");
    AG_CHECK_INT(key(AG_KEY_U, 0, AG_MOD_CTRL), AG_LINE_CHANGED);
    AG_CHECK_STR(g_le.buf, "");
    AG_CHECK_INT(g_le.cursor, 0);
    AG_CHECK_INT(key(AG_KEY_U, 0, AG_MOD_CTRL), AG_LINE_IDLE);

    fresh("some text");
    key(AG_KEY_HOME, 0, 0);
    key(AG_KEY_RIGHT, 0, 0);
    key(AG_KEY_RIGHT, 0, 0);
    key(AG_KEY_RIGHT, 0, 0);
    key(AG_KEY_RIGHT, 0, 0);
    AG_CHECK_INT(key(AG_KEY_K, 0, AG_MOD_CTRL), AG_LINE_CHANGED);
    AG_CHECK_STR(g_le.buf, "some");
}

static void test_results(void)
{
    fresh("run");
    AG_CHECK_INT(key(AG_KEY_ENTER, '\r', 0), AG_LINE_DONE);
    AG_CHECK_STR(g_le.buf, "run");

    AG_CHECK_INT(key(AG_KEY_C, 0, AG_MOD_CTRL), AG_LINE_CANCEL);
    AG_CHECK_INT(key(AG_KEY_TAB, '\t', 0), AG_LINE_COMPLETE);

    /* Ctrl+D ends input only on an empty line. */
    fresh(NULL);
    AG_CHECK_INT(key(AG_KEY_D, 0, AG_MOD_CTRL), AG_LINE_EOF);
    fresh("x");
    key(AG_KEY_HOME, 0, 0);
    AG_CHECK_INT(key(AG_KEY_D, 0, AG_MOD_CTRL), AG_LINE_CHANGED);
    AG_CHECK_STR(g_le.buf, "");
}

static void test_history(void)
{
    fresh(NULL);
    ag_lineedit_remember(&g_le, "first");
    ag_lineedit_remember(&g_le, "second");

    /* Up walks back through history, most recent first. */
    AG_CHECK_INT(key(AG_KEY_UP, 0, 0), AG_LINE_CHANGED);
    AG_CHECK_STR(g_le.buf, "second");
    AG_CHECK_INT(g_le.cursor, 6);
    key(AG_KEY_UP, 0, 0);
    AG_CHECK_STR(g_le.buf, "first");

    /* And stops at the oldest entry rather than wrapping. */
    AG_CHECK_INT(key(AG_KEY_UP, 0, 0), AG_LINE_IDLE);
    AG_CHECK_STR(g_le.buf, "first");

    /* Down comes back. */
    key(AG_KEY_DOWN, 0, 0);
    AG_CHECK_STR(g_le.buf, "second");
    key(AG_KEY_DOWN, 0, 0);
    AG_CHECK_STR(g_le.buf, "");
    AG_CHECK_INT(key(AG_KEY_DOWN, 0, 0), AG_LINE_IDLE);
}

static void test_history_parks_the_current_line(void)
{
    fresh("half-typed");
    ag_lineedit_remember(&g_le, "old");

    key(AG_KEY_UP, 0, 0);
    AG_CHECK_STR(g_le.buf, "old");

    /* Coming back must restore what was being typed, not an empty line. */
    key(AG_KEY_DOWN, 0, 0);
    AG_CHECK_STR(g_le.buf, "half-typed");
}

static void test_history_dedup_and_depth(void)
{
    fresh(NULL);
    ag_lineedit_remember(&g_le, "dir");
    ag_lineedit_remember(&g_le, "dir");
    ag_lineedit_remember(&g_le, "");
    AG_CHECK_INT(g_le.history_count, 1);

    /* The oldest entries fall off the end. */
    ag_lineedit_init(&g_le);
    for (int i = 0; i < AG_HISTORY_DEPTH + 4; i++) {
        char cmd[16];
        snprintf(cmd, sizeof(cmd), "cmd%d", i);
        ag_lineedit_remember(&g_le, cmd);
    }
    AG_CHECK_INT(g_le.history_count, AG_HISTORY_DEPTH);
    AG_CHECK_STR(g_le.history[0], "cmd11");
    AG_CHECK_STR(g_le.history[AG_HISTORY_DEPTH - 1], "cmd4");
}

/*
 * A typed code point becomes one byte of the active code page - the same byte the
 * screen will hold and the same byte the command will receive.  The editor is
 * where that conversion happens, because it is the edge.
 */
static void test_typing_cyrillic_in_866(void)
{
    ag_cp_set_active(AG_CP_866);

    fresh(NULL);
    key(AG_KEY_NONE, 0x0434, 0); /* д */
    key(AG_KEY_NONE, 0x0430, 0); /* а */

    /* Two characters, two bytes, two columns. */
    AG_CHECK_INT(g_le.len, 2);
    AG_CHECK_INT(g_le.cursor, 2);
    AG_CHECK_INT((unsigned char)g_le.buf[0], 0xa4);
    AG_CHECK_INT((unsigned char)g_le.buf[1], 0xa0);

    key(AG_KEY_LEFT, 0, 0);
    AG_CHECK_INT(g_le.cursor, 1);
    key(AG_KEY_END, 0, 0);
    key(AG_KEY_BACKSPACE, 0, 0);
    AG_CHECK_INT(g_le.len, 1);
    AG_CHECK_INT((unsigned char)g_le.buf[0], 0xa4);

    ag_cp_set_active(AG_CP_437);
}

/*
 * And in a page that has no such letter the key does nothing at all.  The
 * alternative - taking it as some other byte, or as two - would put a character
 * on the screen that the user did not type.
 */
static void test_typing_cyrillic_in_437(void)
{
    ag_cp_set_active(AG_CP_437);

    fresh(NULL);
    key(AG_KEY_NONE, 'a', 0);
    key(AG_KEY_NONE, 0x0434, 0); /* д, absent from CP437 */
    key(AG_KEY_NONE, 'b', 0);

    AG_CHECK_STR(g_le.buf, "ab");
    AG_CHECK_INT(g_le.cursor, 2);
}

/* A box drawing character types as one byte in both PC pages. */
static void test_typing_box_drawing(void)
{
    ag_cp_set_active(AG_CP_437);
    fresh(NULL);
    key(AG_KEY_NONE, 0x2500, 0); /* ─ */
    AG_CHECK_INT(g_le.len, 1);
    AG_CHECK_INT((unsigned char)g_le.buf[0], 0xc4);

    ag_cp_set_active(AG_CP_866);
    fresh(NULL);
    key(AG_KEY_NONE, 0x2500, 0);
    AG_CHECK_INT(g_le.len, 1);
    AG_CHECK_INT((unsigned char)g_le.buf[0], 0xc4);

    ag_cp_set_active(AG_CP_437);
}

static void test_buffer_full(void)
{
    fresh(NULL);
    for (int i = 0; i < AG_LINE_MAX + 20; i++) {
        key(AG_KEY_X, 'x', 0);
    }
    /* Room is always kept for the terminator. */
    AG_CHECK_INT(g_le.len, AG_LINE_MAX - 1);
    AG_CHECK_INT(g_le.buf[AG_LINE_MAX - 1], 0);
    AG_CHECK_INT(key(AG_KEY_X, 'x', 0), AG_LINE_IDLE);
}

static void test_non_text_keys_are_ignored(void)
{
    fresh("abc");

    /* Modified keys are commands, not characters. */
    AG_CHECK_INT(key(AG_KEY_F5, 0, 0), AG_LINE_IDLE);
    AG_CHECK_INT(key(AG_KEY_X, 'x', AG_MOD_ALT), AG_LINE_IDLE);
    AG_CHECK_STR(g_le.buf, "abc");

    /* Events that are not key presses do nothing at all. */
    ag_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = AG_EV_POINTER_MOVE;
    AG_CHECK_INT(ag_lineedit_key(&g_le, &ev), AG_LINE_IDLE);
    AG_CHECK_INT(ag_lineedit_key(&g_le, NULL), AG_LINE_IDLE);
}

void run_lineedit_tests(void)
{
    test_typing();
    test_erasing();
    test_cursor_movement();
    test_word_operations();
    test_kill_line();
    test_results();
    test_history();
    test_history_parks_the_current_line();
    test_history_dedup_and_depth();
    test_typing_cyrillic_in_866();
    test_typing_cyrillic_in_437();
    test_typing_box_drawing();
    test_buffer_full();
    test_non_text_keys_are_ignored();
}
