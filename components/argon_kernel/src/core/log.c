/*
 * ArgonOS - logging.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/log.h>

#include <stdio.h>
#include <string.h>

#include <argon/console.h>

#include <argon/port/log.h>
#include <argon/port/time.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>

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
static ag_port_mutex_t s_lock;

static void log_lock(void)
{
    if (s_lock != NULL) {
        ag_port_mutex_take_recursive(s_lock, AG_PORT_FOREVER);
    }
}

static void log_unlock(void)
{
    if (s_lock != NULL) {
        ag_port_mutex_give_recursive(s_lock);
    }
}

const ag_journal_t *ag_log_journal(void) { return &s_journal; }

void ag_log_clear(void) { ag_journal_clear(&s_journal); }

void ag_log_set_echo(bool on) { s_echo = on; }
bool ag_log_echo(void) { return s_echo; }

/*
 * One incomplete line per live pid.  Four processes is the hard system limit,
 * so a fixed table is enough and needs no allocator.
 */
#define AG_APP_LOG_SLOTS 4
#define AG_APP_LOG_BODY  (AG_JOURNAL_LINE_MAX - 48)

typedef struct {
    ag_pid_t pid;
    size_t   len;
    char     body[AG_APP_LOG_BODY];
} ag_app_log_slot_t;

static ag_app_log_slot_t s_app_slots[AG_APP_LOG_SLOTS];

static ag_app_log_slot_t *app_slot_for(ag_pid_t pid)
{
    ag_app_log_slot_t *free_slot = NULL;
    for (int i = 0; i < AG_APP_LOG_SLOTS; i++) {
        if (s_app_slots[i].pid == pid) {
            return &s_app_slots[i];
        }
        if (s_app_slots[i].len == 0 && free_slot == NULL) {
            free_slot = &s_app_slots[i];
        }
    }
    if (free_slot != NULL) {
        free_slot->pid = pid;
        free_slot->len = 0;
        return free_slot;
    }
    /* Steal the first slot rather than drop the write silently. */
    s_app_slots[0].pid = pid;
    s_app_slots[0].len = 0;
    return &s_app_slots[0];
}

static void app_flush_line(ag_pid_t pid, const char *name, const char *body,
                           size_t body_len)
{
    char line[AG_JOURNAL_LINE_MAX];
    const char *tag = (name != NULL && name[0] != '\0') ? name : "app";
    const uint32_t ms = (uint32_t)(ag_port_us() / 1000);
    int n = snprintf(line, sizeof(line), "I (%u) app/%s:%u: ", (unsigned)ms,
                     tag, (unsigned)pid);
    if (n < 0) {
        n = 0;
    }
    size_t off = ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1;
    const size_t room = sizeof(line) - off - 1;
    const size_t take = (body_len < room) ? body_len : room;
    if (take > 0) {
        memcpy(line + off, body, take);
        off += take;
    }
    if (off + 1 < sizeof(line)) {
        line[off++] = '\n';
    }

    ag_journal_write(&s_journal, line, off);
    if (s_echo && ag_console_ready()) {
        ag_console_write_log(line, off);
    }
}

void ag_log_app_write(ag_pid_t pid, const char *name, const char *data,
                      size_t len)
{
    if (!s_ready || data == NULL || len == 0) {
        return;
    }

    log_lock();
    ag_app_log_slot_t *slot = app_slot_for(pid);
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            app_flush_line(pid, name, slot->body, slot->len);
            slot->len = 0;
            continue;
        }
        if (slot->len + 1 >= sizeof(slot->body)) {
            app_flush_line(pid, name, slot->body, slot->len);
            slot->len = 0;
        }
        slot->body[slot->len++] = c;
    }
    log_unlock();
}

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

    s_lock = ag_port_mutex_new_recursive();
    if (s_lock == NULL) {
        return -AG_ENOMEM;
    }

    s_ready = true;
    ag_port_log_redirect(log_vprintf);
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
    const uint32_t ms = (uint32_t)(ag_port_us() / 1000);

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
