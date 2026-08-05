/*
 * ArgonOS - log journal tests.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/journal.h>

#include "test.h"

static ag_journal_t g_j;
static char         g_storage[512];

static void fresh(void)
{
    AG_CHECK_INT(ag_journal_init(&g_j, g_storage, sizeof(g_storage)), AG_OK);
}

/* Joins every line the journal holds with '/', for compact expectations. */
static const char *dump(void)
{
    static char out[2048];
    out[0] = '\0';

    ag_journal_iter_t it;
    ag_journal_begin(&g_j, &it);

    char line[AG_JOURNAL_LINE_MAX];
    bool first = true;
    while (ag_journal_next(&g_j, &it, line, sizeof(line))) {
        if (!first) {
            strncat(out, "/", sizeof(out) - strlen(out) - 1);
        }
        strncat(out, line, sizeof(out) - strlen(out) - 1);
        first = false;
    }
    return out;
}

static void test_init(void)
{
    ag_journal_t j;
    AG_CHECK_INT(ag_journal_init(&j, NULL, 512), -AG_EINVAL);
    AG_CHECK_INT(ag_journal_init(NULL, g_storage, 512), -AG_EINVAL);
    /* Too small to hold one line is not a journal. */
    AG_CHECK_INT(ag_journal_init(&j, g_storage, 8), -AG_EINVAL);

    fresh();
    AG_CHECK_INT(ag_journal_count(&g_j), 0);
    AG_CHECK_STR(dump(), "");
}

static void test_lines(void)
{
    fresh();
    ag_journal_puts(&g_j, "first\nsecond\n");
    AG_CHECK_STR(dump(), "first/second");
    AG_CHECK_INT(g_j.lines_written, 2);
    AG_CHECK_INT(g_j.lines_dropped, 0);
    AG_CHECK_INT(ag_journal_count(&g_j), 2);

    /* A line still being written is readable, without waiting for its newline. */
    ag_journal_puts(&g_j, "third");
    AG_CHECK_STR(dump(), "first/second/third");
    AG_CHECK_INT(g_j.lines_written, 2);

    /* And a printf split across calls becomes one line, not three. */
    fresh();
    ag_journal_puts(&g_j, "one ");
    ag_journal_puts(&g_j, "two ");
    ag_journal_puts(&g_j, "three\n");
    AG_CHECK_STR(dump(), "one two three");
}

static void test_escape_sequences_are_stripped(void)
{
    fresh();

    /* ESP-IDF colours its log lines; a journal read as text must not keep them. */
    ag_journal_puts(&g_j, "\x1b[0;32mI (123) tag: hello\x1b[0m\n");
    AG_CHECK_STR(dump(), "I (123) tag: hello");

    /* Cursor positioning too, and a sequence split across two writes. */
    fresh();
    ag_journal_puts(&g_j, "a\x1b[12;34Hb\n");
    AG_CHECK_STR(dump(), "ab");

    fresh();
    ag_journal_puts(&g_j, "x\x1b[1");
    ag_journal_puts(&g_j, ";2Hy\n");
    AG_CHECK_STR(dump(), "xy");

    /* Two-byte escapes end after one byte. */
    fresh();
    ag_journal_puts(&g_j, "p\x1b" "7q\n");
    AG_CHECK_STR(dump(), "pq");

    /* Carriage returns and stray control bytes are dropped, tabs kept. */
    fresh();
    ag_journal_puts(&g_j, "a\r\nb\x01" "c\td\n");
    AG_CHECK_STR(dump(), "a/bc\td");
}

static void test_wrap_drops_oldest(void)
{
    fresh();

    /* Fill well past the buffer with numbered lines. */
    for (int i = 0; i < 200; i++) {
        char line[32];
        snprintf(line, sizeof(line), "line %03d\n", i);
        ag_journal_puts(&g_j, line);
    }

    AG_CHECK(g_j.wrapped);
    AG_CHECK_INT(g_j.lines_written, 200);
    AG_CHECK(g_j.lines_dropped > 0);

    /* The newest line is always there. */
    const char *all = dump();
    AG_CHECK(strstr(all, "line 199") != NULL);
    /* The oldest is gone, and the journal admits how many went. */
    AG_CHECK(strstr(all, "line 000") == NULL);
    AG_CHECK_INT(ag_journal_lost(&g_j) + ag_journal_count(&g_j),
                 g_j.lines_written);
    /*
     * The cheap counter can undercount by one: a line whose beginning was
     * overwritten is unreadable while its newline is still present.  That is
     * why the derived figure is the one reported.
     */
    AG_CHECK(g_j.lines_dropped <= ag_journal_lost(&g_j));

    /* Reading starts at a line boundary, not in the middle of a word. */
    ag_journal_iter_t it;
    ag_journal_begin(&g_j, &it);
    char first[AG_JOURNAL_LINE_MAX];
    AG_CHECK(ag_journal_next(&g_j, &it, first, sizeof(first)));
    AG_CHECK_INT(strncmp(first, "line ", 5), 0);
}

static void test_long_line_is_truncated(void)
{
    fresh();

    char huge[400];
    memset(huge, 'x', sizeof(huge) - 2);
    huge[sizeof(huge) - 2] = '\n';
    huge[sizeof(huge) - 1] = '\0';
    ag_journal_puts(&g_j, huge);

    ag_journal_iter_t it;
    ag_journal_begin(&g_j, &it);
    char line[64];
    AG_CHECK(ag_journal_next(&g_j, &it, line, sizeof(line)));
    /* Truncated to the caller's buffer, and NUL terminated within it. */
    AG_CHECK_INT(strlen(line), 63);
}

static void test_clear(void)
{
    fresh();
    ag_journal_puts(&g_j, "before\n");
    ag_journal_clear(&g_j);
    AG_CHECK_STR(dump(), "");
    AG_CHECK_INT(g_j.lines_written, 0);

    ag_journal_puts(&g_j, "after\n");
    AG_CHECK_STR(dump(), "after");
}

static void test_repeated_reads_are_stable(void)
{
    fresh();
    ag_journal_puts(&g_j, "a\nb\nc\n");

    /* Reading is not consuming: the journal is a record, not a queue. */
    AG_CHECK_STR(dump(), "a/b/c");
    AG_CHECK_STR(dump(), "a/b/c");
    AG_CHECK_INT(ag_journal_count(&g_j), 3);
    AG_CHECK_INT(ag_journal_count(&g_j), 3);
}

void run_journal_tests(void)
{
    test_init();
    test_lines();
    test_escape_sequences_are_stripped();
    test_wrap_drops_oldest();
    test_long_line_is_truncated();
    test_clear();
    test_repeated_reads_are_stable();
}
