/*
 * ArgonOS - decoding terminal input into key events.
 *
 * A terminal delivers a byte stream in which "the up arrow" is three bytes and
 * "Escape" is one byte that looks exactly like the start of those three.  This
 * module turns that stream into the same ag_event_t values a USB keyboard
 * produces, so the rest of the system never learns where the keystroke came
 * from.
 *
 * One instance per input endpoint (UART, USB CDC, each telnet session).
 *
 * This file is free of kernel and ESP-IDF dependencies so it can be unit
 * tested on a host.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_VTIN_H
#define ARGON_VTIN_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>
#include <argon/keys.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AG_VTIN_GROUND = 0,
    AG_VTIN_ESC,
    AG_VTIN_CSI,
    AG_VTIN_SS3,
    AG_VTIN_UTF8,
} ag_vtin_state_t;

#define AG_VTIN_SEQ_MAX 24

typedef struct {
    ag_vtin_state_t state;
    char            seq[AG_VTIN_SEQ_MAX];
    uint8_t         seqlen;

    /* Partial UTF-8 code point. */
    uint32_t cp;
    uint8_t  cp_remaining;

    /* Set while a pointer button is held, so drags report the right state. */
    uint8_t buttons;
} ag_vtin_t;

void ag_vtin_init(ag_vtin_t *in);

/*
 * Feeds one byte.  Returns true and fills `ev` when the byte completed an
 * event; returns false while a multi-byte sequence is still being collected.
 */
bool ag_vtin_feed(ag_vtin_t *in, uint8_t byte, ag_event_t *ev);

/*
 * Resolves a sequence that stopped arriving.  A lone ESC is indistinguishable
 * from the start of an escape sequence until either the next byte arrives or
 * enough time passes without one, so the console driver calls this after a
 * short idle (30 ms is the usual figure) and gets the Escape key.
 *
 * Returns true and fills `ev` when a pending ESC became an Escape key; any
 * other half-finished sequence is discarded and false is returned.
 */
bool ag_vtin_idle(ag_vtin_t *in, ag_event_t *ev);

/* True when a sequence is in progress, i.e. ag_vtin_idle() has work to do. */
bool ag_vtin_busy(const ag_vtin_t *in);

/*
 * US layout mapping from a printable ASCII character to the physical key that
 * produces it, plus the modifiers needed.  Exposed because the PS/2 driver and
 * the shell's key-name parser want the same table.
 */
uint16_t ag_key_from_ascii(char c, uint16_t *mods);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_VTIN_H */
