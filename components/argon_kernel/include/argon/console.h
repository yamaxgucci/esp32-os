/*
 * ArgonOS - console subsystem.
 *
 * One virtual text screen, several endpoints watching it.  An endpoint is
 * anything that can carry bytes both ways: UART0, USB CDC, a telnet session,
 * or a local display paired with a keyboard.  Everything that writes to the
 * console writes to the screen; the endpoints catch up on their own schedule.
 *
 * Input from every endpoint is decoded into key events and merged into one
 * queue, so a command typed over telnet is indistinguishable from one typed
 * on a keyboard plugged into the board.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_CONSOLE_H
#define ARGON_CONSOLE_H

#include <stdarg.h>

#include <argon/screen.h>
#include <argon/vtin.h>
#include <argon/vtout.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AG_CON_MAX_ENDPOINTS 4

typedef struct {
    const char *name;
    /* Writes to the transport.  May block briefly; must not block forever. */
    int32_t (*write)(void *ctx, const char *data, size_t len);
    /* Reads whatever is available right now.  Returns 0 when nothing is. */
    int32_t (*read)(void *ctx, uint8_t *buf, size_t len);
} ag_con_transport_t;

ag_err_t ag_console_init(uint16_t cols, uint16_t rows);
bool     ag_console_ready(void);

/* Adds an endpoint.  It starts with a full repaint owed to it. */
ag_err_t ag_console_attach(const ag_con_transport_t *transport, void *ctx);

/*
 * Sends console output somewhere else until cleared, which is how the shell
 * implements "dir > files.txt".  The console does not know what the sink is,
 * so it does not have to know about the filesystem.
 *
 * Pass NULL to go back to the screen.  One redirection at a time; the shell
 * is the only thing that redirects and it does so around a single command.
 */
typedef int32_t (*ag_con_sink_fn)(void *ctx, const char *data, size_t len);
void ag_console_redirect(ag_con_sink_fn sink, void *ctx);

void ag_console_write(const char *buf, size_t len);
void ag_console_puts(const char *s);
int  ag_console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  ag_console_vprintf(const char *fmt, va_list ap);

/* Blocks up to timeout_ms; UINT32_MAX waits forever. */
bool ag_console_read_event(ag_event_t *ev, uint32_t timeout_ms);

/* Returns a character, or < 0 on timeout.  Keys with no character are skipped. */
int32_t ag_console_getch(uint32_t timeout_ms);

/*
 * Direct screen access for code that draws rather than prints.  Take the lock
 * around a group of operations that must appear at once.
 */
ag_screen_t *ag_console_screen(void);
void         ag_console_lock(void);
void         ag_console_unlock(void);

/* Pushes pending output to every endpoint now instead of at the next tick. */
void ag_console_sync(void);

/*
 * Input events lost to a full queue.  Should be zero: the console reads only
 * what the queue can hold.  Reported so that a regression is visible rather
 * than showing up as mistyped commands.
 */
uint32_t ag_console_dropped_events(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_CONSOLE_H */
