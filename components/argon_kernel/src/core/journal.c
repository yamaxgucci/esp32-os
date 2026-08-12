/*
 * ArgonOS - log journal.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/journal.h>

#include <string.h>

/* Escape sequence stripper states. */
enum { ESC_GROUND = 0, ESC_SEEN, ESC_CSI };

ag_err_t ag_journal_init(ag_journal_t *j, char *storage, size_t size)
{
    if (j == NULL || storage == NULL || size < AG_JOURNAL_LINE_MAX) {
        return -AG_EINVAL;
    }

    memset(j, 0, sizeof(*j));
    j->buf = storage;
    j->size = (uint32_t)size;
    return AG_OK;
}

void ag_journal_clear(ag_journal_t *j)
{
    if (j == NULL) {
        return;
    }
    j->head = 0;
    j->wrapped = false;
    j->lines_written = 0;
    j->lines_dropped = 0;
    j->bytes_written = 0;
    j->esc = ESC_GROUND;
}

static void put_byte(ag_journal_t *j, char c)
{
    /*
     * Once the ring has been round, every byte written destroys an old one.
     * If that old byte ended a line, the line is gone, and saying so is the
     * whole point of counting.
     */
    if (j->wrapped && j->buf[j->head] == '\n') {
        j->lines_dropped++;
    }

    j->buf[j->head] = c;
    j->head++;
    if (j->head == j->size) {
        j->head = 0;
        j->wrapped = true;
    }

    j->bytes_written++;
    if (c == '\n') {
        j->lines_written++;
    }
}

void ag_journal_write(ag_journal_t *j, const char *text, size_t len)
{
    if (j == NULL || j->buf == NULL || text == NULL) {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)text[i];

        switch (j->esc) {
        case ESC_SEEN:
            /* CSI opens a parameter run; anything else is a two-byte escape. */
            j->esc = (c == '[') ? ESC_CSI : ESC_GROUND;
            continue;

        case ESC_CSI:
            if (c >= 0x40 && c <= 0x7e) {
                j->esc = ESC_GROUND;
            }
            continue;

        default:
            break;
        }

        if (c == 0x1b) {
            j->esc = ESC_SEEN;
            continue;
        }
        if (c == '\r') {
            continue; /* carriage returns are noise in a file of lines */
        }
        if (c < 0x20 && c != '\n' && c != '\t') {
            continue;
        }

        put_byte(j, (char)c);
    }
}

void ag_journal_puts(ag_journal_t *j, const char *text)
{
    if (text != NULL) {
        ag_journal_write(j, text, strlen(text));
    }
}

/* ---------------------------------------------------------------------- */

void ag_journal_begin(const ag_journal_t *j, ag_journal_iter_t *it)
{
    if (j == NULL || it == NULL) {
        return;
    }

    if (!j->wrapped) {
        it->pos = 0;
        it->left = j->head;
        return;
    }

    /*
     * The oldest byte is the one about to be overwritten, and it is almost
     * certainly in the middle of a line.  Skip to just past the first newline
     * so that reading starts at a line boundary rather than mid-word.
     */
    uint32_t pos = j->head;
    uint32_t left = j->size;

    while (left > 0) {
        const char c = j->buf[pos];
        pos = (pos + 1 == j->size) ? 0 : pos + 1;
        left--;
        if (c == '\n') {
            break;
        }
    }

    it->pos = pos;
    it->left = left;
}

bool ag_journal_next(const ag_journal_t *j, ag_journal_iter_t *it, char *out,
                     size_t outlen)
{
    if (j == NULL || it == NULL || out == NULL || outlen == 0) {
        return false;
    }
    if (it->left == 0) {
        return false;
    }

    size_t written = 0;
    bool   any = false;

    while (it->left > 0) {
        const char c = j->buf[it->pos];
        it->pos = (it->pos + 1 == j->size) ? 0 : it->pos + 1;
        it->left--;
        any = true;

        if (c == '\n') {
            break;
        }
        /* A line longer than the caller's buffer is truncated, not split. */
        if (written + 1 < outlen) {
            out[written++] = c;
        }
    }

    out[written] = '\0';
    return any;
}

uint32_t ag_journal_count(const ag_journal_t *j)
{
    if (j == NULL || j->buf == NULL) {
        return 0;
    }

    ag_journal_iter_t it;
    ag_journal_begin(j, &it);

    uint32_t lines = 0;
    char     line[AG_JOURNAL_LINE_MAX];
    while (ag_journal_next(j, &it, line, sizeof(line))) {
        lines++;
    }
    return lines;
}

uint32_t ag_journal_lost(const ag_journal_t *j)
{
    if (j == NULL || j->buf == NULL) {
        return 0;
    }

    /*
     * Counting what survives and subtracting is exact, where counting losses as
     * they happen is not: a line can become unreadable without its terminator
     * being touched.  The count can exceed lines_written by one when a line is
     * still being written, hence the guard.
     */
    const uint32_t held = ag_journal_count(j);
    return (j->lines_written > held) ? (j->lines_written - held) : 0;
}
