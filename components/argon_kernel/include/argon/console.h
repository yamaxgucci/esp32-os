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
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
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

/*
 * While the shell is editing a line, it installs a redraw callback here.  Log
 * echo uses ag_console_write_log so a background message starts on a fresh row
 * and the callback restores the prompt afterwards.  The callback runs with the
 * console lock held.  Pass NULL to clear.
 */
typedef void (*ag_con_live_fn)(void *ctx);
void ag_console_set_live(ag_con_live_fn fn, void *ctx);

void ag_console_write(const char *buf, size_t len);
/*
 * Write a complete log line.  Unlike ag_console_write, this cooperates with a
 * live edit line so DHCP (and any other async log) does not glue itself to the
 * prompt.
 */
void ag_console_write_log(const char *buf, size_t len);
void ag_console_puts(const char *s);
int  ag_console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  ag_console_vprintf(const char *fmt, va_list ap);

/* Blocks up to timeout_ms; UINT32_MAX waits forever.  Takes the event. */
bool ag_console_read_event(ag_event_t *ev, uint32_t timeout_ms);

/*
 * Milliseconds since anything arrived from a person - a key, a mouse report, a
 * finger on a panel, a button on a pad.  What the idle timer in
 * src/core/powerctl.c measures, and the only definition of "nobody is using
 * this" the system has: output does not count, because a board printing to
 * nobody is exactly the case worth saving power in.
 */
uint32_t ag_console_idle_ms(void);

/*
 * Push an event into the console input queue (same path as VT decode).
 * Safe for POINTER, WHEEL, and KEY events from drivers.  Returns false if
 * not ready or the queue refused the event.
 */
bool ag_console_inject_event(const ag_event_t *ev);

/*
 * Whether an event is waiting, without taking it - which is the whole point:
 * `if (kbhit()) c = getch();` is the oldest idiom there is, and a look that
 * consumed the event would make it lose the key it just saw.  `ev` may be NULL
 * when only the answer is wanted.
 */
bool ag_console_peek_event(ag_event_t *ev);

/* Returns a character, or < 0 on timeout.  Keys with no character are skipped. */
int32_t ag_console_getch(uint32_t timeout_ms);

/*
 * Reads a line, echoing it, with backspace.  Deliberately minimal: this is for
 * a yes-or-no prompt, not for editing a command - the shell has a proper line
 * editor for that.  Returns the length, or -AG_EKILLED if interrupted.
 */
int32_t ag_console_readline(char *buf, size_t len);

/*
 * Direct screen access for code that draws rather than prints.  Take the lock
 * around a group of operations that must appear at once.
 */
ag_screen_t *ag_console_screen(void);
void         ag_console_lock(void);
void         ag_console_unlock(void);

/*
 * Which task holds the console lock right now, as a FreeRTOS task handle, or
 * NULL.  The supervisor asks before it deletes a task: deleting the holder of
 * this lock leaves it locked forever, which trades a hung application for a
 * hung console - and the console is how anyone would find out.
 */
void *ag_console_lock_holder(void);

/* Pushes pending output to every endpoint now instead of at the next tick. */
void ag_console_sync(void);

/*
 * Default attributes, cursor shown, and every VT endpoint forced to re-emit
 * DECTCEM.  Used after an app exits: loading/hide-cursor otherwise sticks,
 * and a dropped `\e[?25h` is never resent while the endpoint thinks it showed.
 */
void ag_console_restore_tty(void);

/* Throws away input that has arrived but not been read. */
void ag_console_flush_input(void);

/*
 * The modifiers of the last key seen.  A terminal reports them with the key and
 * says nothing in between, so this is "what was held when something was last
 * pressed" rather than "what is held now" - the second question has no answer
 * over a serial line, and will have one when a USB keyboard arrives.
 */
uint16_t ag_console_mods(void);

/*
 * Sticky key state for games: KEY_DOWN arms a key for a short time (or until
 * KEY_UP).  Also drains the input queue so a poll loop does not fill it.
 * Terminals rarely send KEY_UP; auto-repeat refreshes the sticky window.
 */
bool ag_console_key_pressed(uint16_t keycode);

/*
 * A look at every input event before it is queued for whoever is reading.
 * Returning true means the event has been dealt with and must not be delivered;
 * the event may also be rewritten in place and let through, which is how Ctrl+C
 * becomes AG_EV_QUIT for the process it was aimed at.
 *
 * The supervisor installs this.  A key that stops a runaway application has to
 * be seen when nobody is reading the keyboard - which is precisely the situation
 * it exists for - so it cannot be handled by whoever happens to call getch.
 */
void ag_console_set_hotkeys(bool (*fn)(ag_event_t *ev));

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
