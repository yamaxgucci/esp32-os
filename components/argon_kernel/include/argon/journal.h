/*
 * ArgonOS - log journal.
 *
 * Exists because of a concrete failure: log output routed through the text
 * screen is lost when it arrives faster than the renderer flushes, since the
 * renderer sends the current state of a row rather than its history.  A real
 * error message about a failed mount existed and was invisible, and finding it
 * took an hour.
 *
 * The journal is the record; the screen is a view of it.  Lines are kept in a
 * ring buffer, so the newest are always present and the oldest are dropped -
 * and counted, because a log that quietly loses its beginning is worse than one
 * that admits to it.
 *
 * This file is free of kernel and ESP-IDF dependencies so it can be unit
 * tested on a host.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_JOURNAL_H
#define ARGON_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest single line kept intact; anything longer is truncated. */
#define AG_JOURNAL_LINE_MAX 200

typedef struct {
    char    *buf;
    uint32_t size;
    uint32_t head;    /* where the next byte goes                          */
    bool     wrapped; /* the ring has been round at least once             */

    uint32_t lines_written;
    /*
     * Line terminators overwritten by newer data.  Cheap to maintain but not
     * quite the number of lines lost: a line whose beginning was overwritten is
     * unreadable while its newline may still be present.  Use
     * ag_journal_lost() for the number that matters.
     */
    uint32_t lines_dropped;
    uint32_t bytes_written;

    /*
     * Escape sequences are stripped on the way in.  A journal is read as text,
     * and colour codes stored in it would come back out as "[0;32m" in every
     * listing and every crash report.
     */
    uint8_t esc;
} ag_journal_t;

/*
 * Binds storage to the journal.  Nothing is allocated; the buffer must outlive
 * the journal, which for the system journal means static storage available
 * before anything else has started.
 */
ag_err_t ag_journal_init(ag_journal_t *j, char *storage, size_t size);

/*
 * Appends text.  Embedded newlines separate lines; text without a trailing
 * newline is completed by the next call, so a printf split across several calls
 * still ends up as one line.
 */
void ag_journal_write(ag_journal_t *j, const char *text, size_t len);
void ag_journal_puts(ag_journal_t *j, const char *text);

typedef struct {
    uint32_t pos;  /* index into the ring          */
    uint32_t left; /* bytes still to be scanned    */
} ag_journal_iter_t;

/* Starts at the oldest line still held. */
void ag_journal_begin(const ag_journal_t *j, ag_journal_iter_t *it);

/*
 * Copies the next line into `out`, NUL terminated and without its newline.
 * Returns false when there are no more.
 */
bool ag_journal_next(const ag_journal_t *j, ag_journal_iter_t *it, char *out,
                     size_t outlen);

/* Lines currently held, which is fewer than lines_written once it has wrapped. */
uint32_t ag_journal_count(const ag_journal_t *j);

/*
 * Lines that were written and can no longer be read in full.  Derived rather
 * than counted, so it is exact.
 */
uint32_t ag_journal_lost(const ag_journal_t *j);

void ag_journal_clear(ag_journal_t *j);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_JOURNAL_H */
