/*
 * ArgonOS - command line editing.
 *
 * Kept separate from the shell and from the console so it can be tested
 * without either: it consumes key events and owns a buffer, and says nothing
 * about how the line is drawn or where the events came from.
 *
 * This file is free of kernel and ESP-IDF dependencies so it can be unit
 * tested on a host.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_LINEEDIT_H
#define ARGON_LINEEDIT_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>
#include <argon/keys.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AG_LINE_MAX 256
#define AG_HISTORY_DEPTH 8

typedef enum {
    AG_LINE_IDLE = 0,  /* the key did nothing visible                     */
    AG_LINE_CHANGED,   /* buffer or cursor moved, the line needs redrawing */
    AG_LINE_DONE,      /* Enter: the buffer holds the finished line        */
    AG_LINE_CANCEL,    /* Ctrl+C: abandon the line                         */
    AG_LINE_EOF,       /* Ctrl+D on an empty line                          */
    AG_LINE_COMPLETE,  /* Tab: the caller may offer a completion           */
} ag_line_result_t;

typedef struct {
    char     buf[AG_LINE_MAX];
    uint16_t len;
    uint16_t cursor;

    char    history[AG_HISTORY_DEPTH][AG_LINE_MAX];
    uint8_t history_count;
    /* -1 while editing a fresh line, otherwise an index into history. */
    int16_t history_pos;
    /* The line being edited, parked while the user browses history. */
    char parked[AG_LINE_MAX];
} ag_lineedit_t;

void ag_lineedit_init(ag_lineedit_t *le);

/* Clears the buffer but keeps the history. */
void ag_lineedit_reset(ag_lineedit_t *le);

/*
 * Feeds one key event.  Only AG_EV_KEY_DOWN is meaningful; anything else
 * returns AG_LINE_IDLE.
 */
ag_line_result_t ag_lineedit_key(ag_lineedit_t *le, const ag_event_t *ev);

/*
 * Records a line for recall.  Empty lines and immediate repeats are ignored,
 * which is what makes history worth having.
 */
void ag_lineedit_remember(ag_lineedit_t *le, const char *line);

/* Replaces the buffer, putting the cursor at the end. */
void ag_lineedit_set(ag_lineedit_t *le, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_LINEEDIT_H */
