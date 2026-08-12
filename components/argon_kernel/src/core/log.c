/*
 * ArgonOS - logging.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/log.h>

#include <stdio.h>
#include <string.h>

#include <argon/console.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*
 * Static storage, in internal RAM, available before any allocator has run.  4 KB
 * is roughly two hundred lines: enough to hold a whole boot plus recent history,
 * which is what the journal is for.  It is charged against the kernel's memory
 * budget deliberately - see docs/00-architecture.md section 10.
 */
#define AG_JOURNAL_BYTES 4096

static char         s_storage[AG_JOURNAL_BYTES];
static ag_journal_t s_journal;
static bool         s_echo = true;
static bool         s_ready;

/*
 * One line is several writes to the journal - the head, then the body, then the
 * newline - so without this a task preempted between them has its line spliced
 * into somebody else's.  That was possible on one core already; with an
 * application running on the other one it stops being theoretical.
 *
 * Recursive, because a line is written while it is being echoed to the console,
 * and anything on that path that logs would otherwise deadlock against itself.
 * Taken only when it exists: the first messages of the boot arrive before any
 * allocator has run, and losing those is not an option.
 */
static SemaphoreHandle_t s_lock;

static void log_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}

static void log_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

const ag_journal_t *ag_log_journal(void) { return &s_journal; }

void ag_log_clear(void) { ag_journal_clear(&s_journal); }

void ag_log_set_echo(bool on) { s_echo = on; }
bool ag_log_echo(void) { return s_echo; }

/*
 * Where ESP-IDF's log output goes.  Note the order: the journal first, always,
 * and the console only if there is one and it is wanted.  Reversing that is how
 * a message about a failure gets lost in the failure.
 */
static int log_vprintf(const char *fmt, va_list ap)
{
    char      line[AG_JOURNAL_LINE_MAX + 8];
    const int n = vsnprintf(line, sizeof(line), fmt, ap);

    if (n <= 0) {
        return n;
    }
    const size_t len = ((size_t)n < sizeof(line)) ? (size_t)n
                                                  : sizeof(line) - 1;

    log_lock();
    ag_journal_write(&s_journal, line, len);

    if (s_echo && ag_console_ready()) {
        ag_console_write_log(line, len);
    } else if (!ag_console_ready()) {
        /* Before the console exists the raw port is the only way out. */
        (void)fwrite(line, 1, len, stdout);
        (void)fflush(stdout);
    }
    log_unlock();
    return n;
}

ag_err_t ag_log_init(void)
{
    const ag_err_t err = ag_journal_init(&s_journal, s_storage,
                                         sizeof(s_storage));
    if (err != AG_OK) {
        return err;
    }

    s_lock = xSemaphoreCreateRecursiveMutex();
    if (s_lock == NULL) {
        return -AG_ENOMEM;
    }

    s_ready = true;
    esp_log_set_vprintf(log_vprintf);
    return AG_OK;
}

void ag_vlog(ag_log_level_t level, const char *tag, const char *fmt,
             va_list ap)
{
    static const char k_mark[] = {'E', 'W', 'I', 'D', 'V'};

    if (!s_ready) {
        return;
    }

    const char mark = (level < sizeof(k_mark)) ? k_mark[level] : '?';
    const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* One line, whoever else is logging at the same time. */
    log_lock();

    /* Same shape as ESP-IDF's own lines, so a mixed journal reads as one thing. */
    char head[48];
    const int hn = snprintf(head, sizeof(head), "%c (%u) %s: ", mark,
                            (unsigned)ms, (tag != NULL) ? tag : "argon");
    if (hn > 0) {
        ag_journal_write(&s_journal, head, (size_t)hn);
    }

    char body[AG_JOURNAL_LINE_MAX];
    const int bn = vsnprintf(body, sizeof(body), fmt, ap);
    if (bn > 0) {
        const size_t len = ((size_t)bn < sizeof(body)) ? (size_t)bn
                                                      : sizeof(body) - 1;
        ag_journal_write(&s_journal, body, len);
        ag_journal_puts(&s_journal, "\n");
    }

    /*
     * Echo as a single write so a live prompt is broken once, not once per
     * fragment of the line.
     */
    if (s_echo && ag_console_ready() && hn > 0) {
        char line[sizeof(head) + AG_JOURNAL_LINE_MAX + 1];
        size_t off = 0;
        const size_t hlen = (size_t)hn;
        if (hlen < sizeof(line)) {
            memcpy(line, head, hlen);
            off = hlen;
        }
        if (bn > 0) {
            const size_t blen = ((size_t)bn < sizeof(body)) ? (size_t)bn
                                                            : sizeof(body) - 1;
            const size_t room = sizeof(line) - off - 1;
            const size_t take = (blen < room) ? blen : room;
            memcpy(line + off, body, take);
            off += take;
        }
        if (off + 1 < sizeof(line)) {
            line[off++] = '\n';
        }
        ag_console_write_log(line, off);
    }
    log_unlock();
}

void ag_log(ag_log_level_t level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ag_vlog(level, tag, fmt, ap);
    va_end(ap);
}
