/*
 * ArgonOS DESKTOP - the two dialogs: say something, ask something.
 *
 * Both are ordinary windows with the modal flag set, which means the window
 * manager already knows how to paint them, drag them and keep input away from
 * everything else.  There is no nested event loop anywhere in this shell: a
 * dialog opens, returns immediately, and calls back when it is answered.
 *
 * That costs the caller a callback where a blocking `if (confirm(...))` would
 * read better, and it buys the thing a blocking call cannot have - a shell
 * that still repaints, still moves its pointer, and still answers Ctrl+C
 * while the question is on the screen.
 *
 * One at a time.  Modal means one; a second attempt is refused rather than
 * queued, because two modal windows is a shell with no way out of either.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_DSK_DLG_H
#define ARGON_DSK_DLG_H

#include "dsk.h"

typedef enum {
    DSK_DLG_OK = 0,
    DSK_DLG_OKCANCEL,
    DSK_DLG_YESNO,
} dsk_dlg_kind_t;

typedef enum {
    DSK_ANSWER_OK = 1,
    DSK_ANSWER_CANCEL,
    DSK_ANSWER_YES,
    DSK_ANSWER_NO,
} dsk_answer_t;

#define DSK_INPUT_MAX 96

/* How many lines a message may carry.  Six is what a file's properties want:
 * name, size, date, attributes, and room. */
#define DSK_DLG_LINES_MAX 6

void dsk_dlg_init(const dsk_metrics_t *m);

/*
 * Whether a box with a text field carries a keyboard drawn on the glass.
 *
 * The decision is the shell's, not this file's: it depends on what input
 * devices the machine has and on what the person chose in Options, and
 * neither of those is a dialog's business.  See dsk_kbd.h for why it exists
 * at all.
 */
void dsk_dlg_keyboard(bool show);

/* False when a dialog is already up. */
bool dsk_dlg_message(const char *title, const char *line1, const char *line2,
                     dsk_dlg_kind_t kind,
                     void (*done)(dsk_answer_t a, void *ctx), void *ctx);

/*
 * A line of text.  `done` gets the answer and the text, which is only valid
 * for the duration of the call - copy it if it is wanted afterwards.
 */
bool dsk_dlg_input(const char *title, const char *prompt, const char *initial,
                   void (*done)(dsk_answer_t a, const char *text, void *ctx),
                   void *ctx);

/*
 * The same, with as many lines as it takes - what properties are.  Always an
 * OK box: a list of facts has nothing to answer.
 */
bool dsk_dlg_lines(const char *title, const char *const *lines, int n,
                   void (*done)(dsk_answer_t a, void *ctx), void *ctx);

bool dsk_dlg_up(void);

/*
 * The caret blinks, so the shell has to be woken to flip it: this says how
 * long until the next flip (UINT32_MAX when nothing is waiting), and tick()
 * does the flipping.
 */
uint32_t dsk_dlg_wait_ms(uint32_t now_ms);
void     dsk_dlg_tick(uint32_t now_ms);

#endif /* ARGON_DSK_DLG_H */
