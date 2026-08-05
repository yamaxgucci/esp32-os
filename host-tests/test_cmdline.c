/*
 * ArgonOS - command line splitting tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/cmdline.h>

#include "test.h"

static char  *g_argv[AG_ARGV_MAX];
static char   g_line[256];

static int split(const char *text)
{
    strncpy(g_line, text, sizeof(g_line) - 1);
    g_line[sizeof(g_line) - 1] = '\0';
    return ag_cmdline_split(g_line, g_argv, AG_ARGV_MAX);
}

static void test_basic(void)
{
    AG_CHECK_INT(split("dir"), 1);
    AG_CHECK_STR(g_argv[0], "dir");

    AG_CHECK_INT(split("copy a.txt b.txt"), 3);
    AG_CHECK_STR(g_argv[0], "copy");
    AG_CHECK_STR(g_argv[1], "a.txt");
    AG_CHECK_STR(g_argv[2], "b.txt");

    /* Runs of whitespace, and leading and trailing space. */
    AG_CHECK_INT(split("   echo    hello   world   "), 3);
    AG_CHECK_STR(g_argv[0], "echo");
    AG_CHECK_STR(g_argv[1], "hello");
    AG_CHECK_STR(g_argv[2], "world");

    AG_CHECK_INT(split("a\tb"), 2);
    AG_CHECK_STR(g_argv[1], "b");
}

static void test_empty(void)
{
    AG_CHECK_INT(split(""), 0);
    AG_CHECK_INT(split("   "), 0);
    AG_CHECK_INT(split("\t\r\n"), 0);
    AG_CHECK_INT(ag_cmdline_split(NULL, g_argv, AG_ARGV_MAX), 0);
    AG_CHECK_INT(ag_cmdline_split(g_line, NULL, AG_ARGV_MAX), 0);
    AG_CHECK_INT(ag_cmdline_split(g_line, g_argv, 0), 0);
}

static void test_quotes(void)
{
    AG_CHECK_INT(split("type \"my file.txt\""), 2);
    AG_CHECK_STR(g_argv[1], "my file.txt");

    /* Quotes may open and close mid-argument. */
    AG_CHECK_INT(split("a\"b c\"d e"), 2);
    AG_CHECK_STR(g_argv[0], "ab cd");
    AG_CHECK_STR(g_argv[1], "e");

    /* An empty quoted argument is still an argument. */
    AG_CHECK_INT(split("echo \"\""), 2);
    AG_CHECK_STR(g_argv[1], "");

    /*
     * An unclosed quote runs to the end of the line.  Refusing to act on a
     * missing quote is more annoying than guessing.
     */
    AG_CHECK_INT(split("echo \"unterminated here"), 2);
    AG_CHECK_STR(g_argv[1], "unterminated here");
}

static void test_backslash_is_not_an_escape(void)
{
    /* Paths are written A:\APPS\X.AXE; escaping them would be absurd. */
    AG_CHECK_INT(split("run A:\\APPS\\HELLO.AXE"), 2);
    AG_CHECK_STR(g_argv[1], "A:\\APPS\\HELLO.AXE");

    AG_CHECK_INT(split("echo C:\\"), 2);
    AG_CHECK_STR(g_argv[1], "C:\\");
}

static void test_argv_limit(void)
{
    /* More arguments than fit are dropped, not written past the array. */
    char many[256] = "cmd";
    for (int i = 0; i < AG_ARGV_MAX + 5; i++) {
        strncat(many, " x", sizeof(many) - strlen(many) - 1);
    }
    AG_CHECK_INT(split(many), AG_ARGV_MAX);
    AG_CHECK_STR(g_argv[0], "cmd");
    AG_CHECK_STR(g_argv[AG_ARGV_MAX - 1], "x");
}

void run_cmdline_tests(void)
{
    test_basic();
    test_empty();
    test_quotes();
    test_backslash_is_not_an_escape();
    test_argv_limit();
}
