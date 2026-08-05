/*
 * ArgonOS - terminal input decoder tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/vtin.h>

#include "test.h"

static ag_vtin_t g_in;

/*
 * Feeds a byte string and returns the number of events produced, storing them
 * for inspection.  Everything a terminal sends is tested this way, byte by
 * byte, because that is how it actually arrives.
 */
#define MAX_EVENTS 8
static ag_event_t g_events[MAX_EVENTS];
static int        g_nevents;

static int feed(const char *bytes, size_t len)
{
    g_nevents = 0;
    for (size_t i = 0; i < len; i++) {
        ag_event_t ev;
        if (ag_vtin_feed(&g_in, (uint8_t)bytes[i], &ev) &&
            g_nevents < MAX_EVENTS) {
            g_events[g_nevents++] = ev;
        }
    }
    return g_nevents;
}

static int feed_str(const char *s)
{
    ag_vtin_init(&g_in);
    return feed(s, strlen(s));
}

/* Checks that the input produced exactly one key event with these fields. */
static void expect_key(const char *input, size_t len, uint16_t keycode,
                       uint32_t unicode, uint16_t mods)
{
    ag_vtin_init(&g_in);
    const int n = feed(input, len);

    AG_CHECK_INT(n, 1);
    if (n != 1) {
        printf("     (input was %zu bytes starting with 0x%02x)\n", len,
               (unsigned)(unsigned char)input[0]);
        return;
    }
    AG_CHECK_INT(g_events[0].type, AG_EV_KEY_DOWN);
    AG_CHECK_INT(g_events[0].key.keycode, keycode);
    AG_CHECK_INT(g_events[0].key.unicode, unicode);
    AG_CHECK_INT(g_events[0].key.mods, mods);
}

#define EXPECT_KEY(str, kc, uc, md) expect_key(str, sizeof(str) - 1, kc, uc, md)

static void test_plain_characters(void)
{
    EXPECT_KEY("a", AG_KEY_A, 'a', 0);
    EXPECT_KEY("A", AG_KEY_A, 'A', AG_MOD_SHIFT);
    EXPECT_KEY("z", AG_KEY_Z, 'z', 0);
    EXPECT_KEY("0", AG_KEY_0, '0', 0);
    EXPECT_KEY("7", AG_KEY_7, '7', 0);
    EXPECT_KEY(" ", AG_KEY_SPACE, ' ', 0);
    EXPECT_KEY("/", AG_KEY_SLASH, '/', 0);
    EXPECT_KEY("?", AG_KEY_SLASH, '?', AG_MOD_SHIFT);
    EXPECT_KEY("\\", AG_KEY_BACKSLASH, '\\', 0);
    EXPECT_KEY("|", AG_KEY_BACKSLASH, '|', AG_MOD_SHIFT);
    EXPECT_KEY("*", AG_KEY_8, '*', AG_MOD_SHIFT);
}

static void test_control_keys(void)
{
    EXPECT_KEY("\r", AG_KEY_ENTER, '\r', 0);
    /* A terminal in raw mode may send either; both are the Enter key. */
    EXPECT_KEY("\n", AG_KEY_ENTER, '\r', 0);
    EXPECT_KEY("\t", AG_KEY_TAB, '\t', 0);
    /* Most terminals send DEL when you press backspace. */
    EXPECT_KEY("\x7f", AG_KEY_BACKSPACE, 0x08, 0);
    EXPECT_KEY("\x08", AG_KEY_BACKSPACE, 0x08, 0);

    /* Ctrl+letter. Ctrl+C is what interrupts a running application. */
    EXPECT_KEY("\x03", AG_KEY_C, 0, AG_MOD_CTRL);
    EXPECT_KEY("\x01", AG_KEY_A, 0, AG_MOD_CTRL);
    EXPECT_KEY("\x1a", AG_KEY_Z, 0, AG_MOD_CTRL);
    EXPECT_KEY("\x00", AG_KEY_SPACE, 0, AG_MOD_CTRL);

    /*
     * Ctrl+H, Ctrl+I and Ctrl+M are the same bytes as Backspace, Tab and
     * Enter.  Naming the key is more useful than naming the combination.
     */
    AG_CHECK_INT(feed_str("\x08")  > 0 ? g_events[0].key.keycode : 0,
                 AG_KEY_BACKSPACE);
    AG_CHECK_INT(feed_str("\x09")  > 0 ? g_events[0].key.keycode : 0,
                 AG_KEY_TAB);
    AG_CHECK_INT(feed_str("\x0d")  > 0 ? g_events[0].key.keycode : 0,
                 AG_KEY_ENTER);
}

static void test_arrows_and_navigation(void)
{
    EXPECT_KEY("\x1b[A", AG_KEY_UP, 0, 0);
    EXPECT_KEY("\x1b[B", AG_KEY_DOWN, 0, 0);
    EXPECT_KEY("\x1b[C", AG_KEY_RIGHT, 0, 0);
    EXPECT_KEY("\x1b[D", AG_KEY_LEFT, 0, 0);
    EXPECT_KEY("\x1b[H", AG_KEY_HOME, 0, 0);
    EXPECT_KEY("\x1b[F", AG_KEY_END, 0, 0);

    /* Application cursor mode sends SS3 instead of CSI. */
    EXPECT_KEY("\x1bOA", AG_KEY_UP, 0, 0);
    EXPECT_KEY("\x1bOD", AG_KEY_LEFT, 0, 0);

    EXPECT_KEY("\x1b[2~", AG_KEY_INSERT, 0, 0);
    EXPECT_KEY("\x1b[3~", AG_KEY_DELETE, 0, 0);
    EXPECT_KEY("\x1b[5~", AG_KEY_PAGEUP, 0, 0);
    EXPECT_KEY("\x1b[6~", AG_KEY_PAGEDOWN, 0, 0);
    /* Both spellings of Home and End are in the wild. */
    EXPECT_KEY("\x1b[1~", AG_KEY_HOME, 0, 0);
    EXPECT_KEY("\x1b[4~", AG_KEY_END, 0, 0);
}

static void test_function_keys(void)
{
    EXPECT_KEY("\x1bOP", AG_KEY_F1, 0, 0);
    EXPECT_KEY("\x1bOQ", AG_KEY_F2, 0, 0);
    EXPECT_KEY("\x1bOR", AG_KEY_F3, 0, 0);
    EXPECT_KEY("\x1bOS", AG_KEY_F4, 0, 0);
    EXPECT_KEY("\x1b[15~", AG_KEY_F5, 0, 0);
    EXPECT_KEY("\x1b[17~", AG_KEY_F6, 0, 0);
    EXPECT_KEY("\x1b[21~", AG_KEY_F10, 0, 0);
    EXPECT_KEY("\x1b[24~", AG_KEY_F12, 0, 0);
}

static void test_modified_keys(void)
{
    /* xterm encodes modifiers as 1 + bitmask in the second parameter. */
    EXPECT_KEY("\x1b[1;5A", AG_KEY_UP, 0, AG_MOD_CTRL);
    EXPECT_KEY("\x1b[1;2A", AG_KEY_UP, 0, AG_MOD_SHIFT);
    EXPECT_KEY("\x1b[1;3A", AG_KEY_UP, 0, AG_MOD_ALT);
    EXPECT_KEY("\x1b[1;6A", AG_KEY_UP, 0, AG_MOD_SHIFT | AG_MOD_CTRL);
    EXPECT_KEY("\x1b[1;7C", AG_KEY_RIGHT, 0, AG_MOD_ALT | AG_MOD_CTRL);
    EXPECT_KEY("\x1b[3;5~", AG_KEY_DELETE, 0, AG_MOD_CTRL);

    /* Shift+Tab has its own sequence. */
    EXPECT_KEY("\x1b[Z", AG_KEY_TAB, '\t', AG_MOD_SHIFT);

    /* Alt+key is the key prefixed with ESC. */
    EXPECT_KEY("\x1b" "x", AG_KEY_X, 'x', AG_MOD_ALT);
    EXPECT_KEY("\x1b" "1", AG_KEY_1, '1', AG_MOD_ALT);

    /* CSI u, for combinations the legacy scheme cannot express. */
    EXPECT_KEY("\x1b[13;5u", AG_KEY_ENTER, 0, AG_MOD_CTRL);
    EXPECT_KEY("\x1b[97;5u", AG_KEY_A, 0, AG_MOD_CTRL);
}

static void test_lone_escape(void)
{
    ag_event_t ev;

    /* A bare ESC produces nothing until we decide no more bytes are coming. */
    ag_vtin_init(&g_in);
    AG_CHECK(!ag_vtin_feed(&g_in, 0x1b, &ev));
    AG_CHECK(ag_vtin_busy(&g_in));

    AG_CHECK(ag_vtin_idle(&g_in, &ev));
    AG_CHECK_INT(ev.key.keycode, AG_KEY_ESC);
    AG_CHECK(!ag_vtin_busy(&g_in));

    /* Idle with nothing pending reports nothing. */
    AG_CHECK(!ag_vtin_idle(&g_in, &ev));

    /* ESC ESC gives the first escape immediately, the second on idle. */
    ag_vtin_init(&g_in);
    AG_CHECK_INT(feed("\x1b\x1b", 2), 1);
    AG_CHECK_INT(g_events[0].key.keycode, AG_KEY_ESC);
    AG_CHECK(ag_vtin_idle(&g_in, &ev));
    AG_CHECK_INT(ev.key.keycode, AG_KEY_ESC);

    /* A half-finished sequence is discarded rather than guessed at. */
    ag_vtin_init(&g_in);
    feed("\x1b[1;", 4);
    AG_CHECK(ag_vtin_busy(&g_in));
    AG_CHECK(!ag_vtin_idle(&g_in, &ev));
    AG_CHECK(!ag_vtin_busy(&g_in));
}

static void test_split_and_batched_input(void)
{
    ag_event_t ev;

    /* A sequence split across reads must still decode. */
    ag_vtin_init(&g_in);
    AG_CHECK(!ag_vtin_feed(&g_in, 0x1b, &ev));
    AG_CHECK(!ag_vtin_feed(&g_in, '[', &ev));
    AG_CHECK(ag_vtin_feed(&g_in, 'A', &ev));
    AG_CHECK_INT(ev.key.keycode, AG_KEY_UP);

    /* A paste arrives as one burst and must produce one event per character. */
    AG_CHECK_INT(feed_str("hi!"), 3);
    AG_CHECK_INT(g_events[0].key.unicode, 'h');
    AG_CHECK_INT(g_events[1].key.unicode, 'i');
    AG_CHECK_INT(g_events[2].key.unicode, '!');

    /* Text and sequences interleaved. */
    AG_CHECK_INT(feed_str("a\x1b[Ab"), 3);
    AG_CHECK_INT(g_events[0].key.unicode, 'a');
    AG_CHECK_INT(g_events[1].key.keycode, AG_KEY_UP);
    AG_CHECK_INT(g_events[2].key.unicode, 'b');
}

static void test_utf8(void)
{
    /* Two byte: Cyrillic small letter de (U+0434). */
    EXPECT_KEY("\xd0\xb4", AG_KEY_NONE, 0x0434, 0);
    /* Three byte: euro sign (U+20AC). */
    EXPECT_KEY("\xe2\x82\xac", AG_KEY_NONE, 0x20ac, 0);
    /* Four byte: U+1F600. */
    EXPECT_KEY("\xf0\x9f\x98\x80", AG_KEY_NONE, 0x1f600, 0);

    /* A truncated code point followed by ASCII must not eat the ASCII. */
    AG_CHECK_INT(feed_str("\xd0" "a"), 1);
    AG_CHECK_INT(g_events[0].key.unicode, 'a');

    /* A stray continuation byte is dropped silently. */
    AG_CHECK_INT(feed_str("\xb4"), 0);
}

static void test_runaway_sequence(void)
{
    /* A sequence longer than the buffer is abandoned, not overflowed. */
    ag_vtin_init(&g_in);
    char junk[64];
    junk[0] = 0x1b;
    junk[1] = '[';
    for (size_t i = 2; i < sizeof(junk); i++) {
        junk[i] = '1';
    }
    feed(junk, sizeof(junk));
    AG_CHECK(!ag_vtin_busy(&g_in));

    /* And normal input still works afterwards. */
    AG_CHECK_INT(feed_str("q"), 1);
    AG_CHECK_INT(g_events[0].key.keycode, AG_KEY_Q);
}

static void test_mouse(void)
{
    /* SGR mouse: CSI < button ; column ; row M for press, m for release. */
    ag_vtin_init(&g_in);
    AG_CHECK_INT(feed("\x1b[<0;10;5M", 10), 1);
    AG_CHECK_INT(g_events[0].type, AG_EV_POINTER_DOWN);
    AG_CHECK_INT(g_events[0].ptr.x, 9);
    AG_CHECK_INT(g_events[0].ptr.y, 4);
    AG_CHECK_INT(g_events[0].ptr.buttons, 1);

    /* The held button is remembered so a drag reports it. */
    AG_CHECK_INT(feed("\x1b[<32;11;5M", 11), 1);
    AG_CHECK_INT(g_events[0].type, AG_EV_POINTER_MOVE);
    AG_CHECK_INT(g_events[0].ptr.buttons, 1);

    AG_CHECK_INT(feed("\x1b[<0;11;5m", 10), 1);
    AG_CHECK_INT(g_events[0].type, AG_EV_POINTER_UP);
    AG_CHECK_INT(g_events[0].ptr.buttons, 0);

    /* Wheel. */
    AG_CHECK_INT(feed("\x1b[<64;1;1M", 10), 1);
    AG_CHECK_INT(g_events[0].type, AG_EV_WHEEL);
    AG_CHECK_INT(g_events[0].ptr.dy, -1);
    AG_CHECK_INT(feed("\x1b[<65;1;1M", 10), 1);
    AG_CHECK_INT(g_events[0].ptr.dy, 1);
}

static void test_unknown_sequences_are_dropped(void)
{
    /* An unrecognised final byte yields nothing rather than a bogus key. */
    AG_CHECK_INT(feed_str("\x1b[99X"), 0);
    /* A device status report echoed back by the terminal is not a keystroke. */
    AG_CHECK_INT(feed_str("\x1b[24;80R"), 0);
    AG_CHECK(!ag_vtin_busy(&g_in));
}

void run_vtin_tests(void)
{
    test_plain_characters();
    test_control_keys();
    test_arrows_and_navigation();
    test_function_keys();
    test_modified_keys();
    test_lone_escape();
    test_split_and_batched_input();
    test_utf8();
    test_runaway_sequence();
    test_mouse();
    test_unknown_sequences_are_dropped();
}
