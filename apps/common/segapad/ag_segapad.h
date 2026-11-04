/*
 * ArgonOS - a Mega Drive pad read through an 8-bit port, the logic only.
 *
 * The pad is a 74HC157 (a 6-button pad adds a counter) behind a DB9: six lines
 * out of the pad and one line into it, SELECT (the console's TH).  Wired to a
 * PCF8574 as SEGAPAD.SYS expects it:
 *
 *   P0  pin 1  up          P4  pin 6  A / B
 *   P1  pin 2  down        P5  pin 9  Start / C
 *   P2  pin 3  left        P6  pin 7  SELECT (an output: the only one)
 *   P3  pin 4  right       P7  -      unused, left high
 *   pad pin 5 to 3.3 V, pin 8 to ground.  Lines are low when pressed.
 *
 * One read is eight steps, SELECT high then low four times, and what each step
 * shows is the pad's answer to how many times SELECT has fallen:
 *
 *   step  SELECT  P0..P5
 *   0     high    up down left right B C
 *   1     low     up down 0    0     A Start   (left/right low: a pad is there)
 *   2,4   high    as step 0
 *   3     low     as step 1
 *   5     low     0  0    0    0     A Start   (all four low: a 6-button pad)
 *   6     high    Z  Y    X    Mode  -  -      (6-button only)
 *   7     low     ignored
 *
 * A 6-button pad's counter returns to zero when SELECT has not moved for about
 * 1.5 ms, so the eight steps go out back to back and the next read waits more
 * than that.  This file says nothing about time; the driver does.
 *
 * Here and not in the driver so that the host tests (host-tests/test_segapad.c)
 * and the ESP-07 that stands in for a pad while there is none
 * (firmware/esp8266-segapad) run the same lines the driver runs.  The pad model
 * below is that stand-in: what this file believes a pad does, which is what a
 * test of this file can prove - not what a pad on the bench does.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_SEGAPAD_H
#define AG_SEGAPAD_H

#include <stdint.h>

/* Where the functions go: nowhere special, except on the ESP8266 stand-in,
 * which answers the bus from an interrupt and puts them in IRAM. */
#ifndef AG_SEGAPAD_FN
#define AG_SEGAPAD_FN
#endif

#define AG_SEGAPAD_STEPS  8
#define AG_SEGAPAD_SELECT 0x40u /* P6 */

/* What a read found, as bits of a held mask. */
#define AG_SP_UP    0x0001u
#define AG_SP_DOWN  0x0002u
#define AG_SP_LEFT  0x0004u
#define AG_SP_RIGHT 0x0008u
#define AG_SP_A     0x0010u
#define AG_SP_B     0x0020u
#define AG_SP_C     0x0040u
#define AG_SP_START 0x0080u
#define AG_SP_X     0x0100u
#define AG_SP_Y     0x0200u
#define AG_SP_Z     0x0400u
#define AG_SP_MODE  0x0800u
#define AG_SP_ALL   0x0fffu

/* No pad (or nothing on the port), a 3-button pad, a 6-button pad. */
#define AG_SP_NONE 0
#define AG_SP_3BTN 3
#define AG_SP_6BTN 6

/* The byte to write to the port before step `step`: SELECT high on the even
 * steps, every other line high so it stays an input. */
static inline uint8_t ag_segapad_out(int step)
{
    return (step & 1) ? (uint8_t)(0xffu & ~AG_SEGAPAD_SELECT) : 0xffu;
}

/* The eight bytes read back, one per step, to what is held.  Returns the kind
 * of pad; *held is zero when there is none. */
int ag_segapad_decode(const uint8_t rd[AG_SEGAPAD_STEPS], uint16_t *held);

/*
 * The stand-in pad.  `held` is what a finger holds; `six` picks the pad.
 * select() is SELECT moving, with the time in microseconds; read() is what
 * the six lines show now, as P0..P5 with P6/P7 high (a PCF8574 reads its own
 * output latch back on the pins it drives high).
 */
typedef struct {
    uint16_t held;
    uint8_t  six;
    uint8_t  sel;    /* SELECT now: 1 high                                */
    uint8_t  falls;  /* times SELECT fell since it was last quiet          */
    uint32_t moved;  /* when SELECT last changed, microseconds             */
} ag_segapad_model_t;

void    ag_segapad_model_init(ag_segapad_model_t *m, int six);
void    ag_segapad_model_select(ag_segapad_model_t *m, int high, uint32_t us);
uint8_t ag_segapad_model_read(const ag_segapad_model_t *m);

#endif
