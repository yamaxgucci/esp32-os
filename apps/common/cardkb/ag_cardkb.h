/*
 * ArgonOS - an I2C keyboard that answers like M5Stack's CardKB, the logic only.
 *
 * CardKB sits at 0x5F and answers a one-byte read with the key pressed last,
 * or 0 when there is none: printable ASCII as itself, and
 *
 *   0x08 Backspace   0x09 Tab   0x0D Enter   0x1B Esc   0x7F Delete
 *   0xB4 left        0xB5 up    0xB6 down    0xB7 right
 *
 * CARDKB.SYS reads two bytes.  A real CardKB has only the one, and the second
 * comes back as the idle line, 0xFF.  The ESP-07 stand-in
 * (firmware/esp8266-segapad) sends the key's complement under AG_CARDKB_CHECK
 * as the second, and keeps the key until the board writes any byte back to say
 * it has it: that module answers the bus in software and its radio garbles a
 * bit now and then, and a keyboard that types the wrong letter is worse than
 * one that types a letter a few milliseconds late.
 *
 * Here and not in the driver so that the host tests (host-tests/test_cardkb.c)
 * run the same code the driver does.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_CARDKB_H
#define AG_CARDKB_H

#include <stdint.h>

#ifndef AG_CARDKB_FN
#define AG_CARDKB_FN
#endif

#define AG_CARDKB_ADDR  0x5Fu
/* The stand-in's check byte is the key under one of two masks, and the mask
 * changes with every key it hands over: a key the board took and read again
 * because its "taken" never arrived comes back under the same mask, and is
 * not typed twice.  Every bit of the two masks differs, so one flipped bit in
 * either byte still matches neither. */
#define AG_CARDKB_CHECK  0xA5u
#define AG_CARDKB_CHECK1 0x5Au

#define AG_CARDKB_LEFT  0xB4u
#define AG_CARDKB_UP    0xB5u
#define AG_CARDKB_DOWN  0xB6u
#define AG_CARDKB_RIGHT 0xB7u

/* What a two-byte read says. */
enum {
    AG_CARDKB_NONE = 0,     /* no key                                       */
    AG_CARDKB_KEY = 1,      /* a key, checked: write a byte back to take it */
    AG_CARDKB_PLAIN = 2,    /* a key from a real CardKB, already taken      */
    AG_CARDKB_GARBLED = -1, /* the two bytes disagree: read again           */
};

/* `*mask` is 0 or 1 for a checked key - which of the two masks it came under -
 * and -1 otherwise. */
AG_CARDKB_FN int ag_cardkb_judge(uint8_t key, uint8_t check, int *mask);

/*
 * The key a code is, as the input layer names keys (AG_KEY_*, USB usage ids,
 * US layout), with the character it types and the modifiers it needs.  0 for
 * a code no key makes.
 */
AG_CARDKB_FN uint16_t ag_cardkb_key(uint8_t code, uint32_t *unicode,
                                    uint16_t *mods);

#endif
