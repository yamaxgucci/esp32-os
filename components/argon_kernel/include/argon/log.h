/*
 * ArgonOS - logging.
 *
 * Everything that gets logged - by the kernel, by ESP-IDF, by a driver - lands
 * in the journal first.  The console is one reader of it, not the destination:
 * a message written while the screen is scrolling would otherwise be gone
 * before anyone could see it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_LOG_H
#define ARGON_LOG_H

#include <stdarg.h>

#include <argon/abi.h>
#include <argon/journal.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts the journal and takes over ESP-IDF's log output.  Runs early, before
 * the console exists, so that a failure during bring-up is recorded even though
 * there is nowhere to show it yet.
 */
ag_err_t ag_log_init(void);

void ag_log(ag_log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void ag_vlog(ag_log_level_t level, const char *tag, const char *fmt,
             va_list ap);

/*
 * Whether log lines also go to the console as they arrive.  On by default; the
 * shell turns it off while an application owns the screen, so that a driver
 * logging in the background does not draw over it.
 */
void ag_log_set_echo(bool on);
bool ag_log_echo(void);

const ag_journal_t *ag_log_journal(void);
void                ag_log_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_LOG_H */
