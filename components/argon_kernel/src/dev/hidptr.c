/*
 * ArgonOS - a boot HID mouse, as far as everything else is concerned.
 *
 * A port hands over HID reports; this turns them into the pointer events the
 * terminal decoder and the touchscreen driver already produce, so nothing above
 * can tell a mouse on the board from a finger on the glass or a click in a
 * serial terminal.  The same arrangement as hidkbd.c, and for the same reason:
 * the report is identical over Bluetooth and USB, so the transports share this
 * and neither is visible.
 *
 * Two things a keyboard does not make you decide.
 *
 * First, a mouse says how far it moved and never where it is, so the position
 * lives here rather than in the device.  It is kept in surface pixels because
 * that is the finer of the two units in play, and clamped to the surface -
 * a pointer that can leave the screen is a pointer you cannot get back.
 *
 * Second, which units.  Pixels of the surface, always (ABI 0.42) - a mouse is
 * already counting in something finer than a cell, and throwing that away to
 * fit an 8x16 grid discards the only thing a mouse is better at than a
 * touchscreen.  The sources that do count in cells - a terminal, a touch
 * driver - are scaled on the way in by ag_input_to_pixels, so nothing above
 * needs to ask where an event came from.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/hidptr.h>

#if AG_PORT_HAS_BT || AG_PORT_HAS_USB_HID

#include <argon/console.h>
#include <argon/display.h>
#include <argon/screen.h>

#include <argon/port/time.h>

/* Boot mouse buttons, in the order the report puts them. */
#define BTN_LEFT   0x01u
#define BTN_RIGHT  0x02u
#define BTN_MIDDLE 0x04u

static int32_t s_x;
static int32_t s_y;
static uint8_t s_buttons;
static bool    s_placed; /* false until the surface size is known once     */

/*
 * Where the pointer may go.  The surface if there is one; failing that the
 * console's cells, so a machine with no framebuffer still has a pointer that
 * lands somewhere sensible instead of one confined to a single cell.
 */
static void bounds(int32_t *w, int32_t *h)
{
    uint16_t sw = 0, sh = 0;
    if (ag_display_size(&sw, &sh) && sw > 0 && sh > 0) {
        *w = sw;
        *h = sh;
        return;
    }
    const ag_screen_t *sc = ag_console_screen();
    if (sc != NULL && sc->cols > 0 && sc->rows > 0) {
        *w = sc->cols;
        *h = sc->rows;
        return;
    }
    *w = 320;
    *h = 240;
}

static int32_t clamp(int32_t v, int32_t hi)
{
    if (v < 0) {
        return 0;
    }
    if (v >= hi) {
        return hi - 1;
    }
    return v;
}

void ag_hidptr_reset(void)
{
    int32_t w, h;
    bounds(&w, &h);
    s_x = w / 2;
    s_y = h / 2;
    s_buttons = 0;
    s_placed = true;
}

void ag_hidptr_pos(int32_t *x, int32_t *y)
{
    if (x != NULL) {
        *x = s_x;
    }
    if (y != NULL) {
        *y = s_y;
    }
}

static void emit(ag_event_type_t type, int16_t dx, int16_t dy, uint8_t buttons)
{
    ag_event_t ev;
    ev.type = type;
    ev.ts = ag_port_us();
    ev.ptr.x = (int16_t)s_x;
    ev.ptr.y = (int16_t)s_y;
    ev.ptr.dx = dx;
    ev.ptr.dy = dy;
    ev.ptr.buttons = buttons;
    ev.ptr.slot = 0;
    (void)ag_console_inject_event(&ev);
}

void ag_hidptr_report(const uint8_t *data, uint32_t len)
{
    /*
     * Three bytes is the shortest thing that is a mouse.  Anything shorter is
     * not guessed at: a device whose report this code does not understand is
     * better off doing nothing than moving the pointer by whatever happened to
     * be in the first byte.
     */
    if (data == NULL || len < 3u) {
        return;
    }
    if (!s_placed) {
        ag_hidptr_reset();
    }

    const uint8_t buttons = (uint8_t)(data[0] & (BTN_LEFT | BTN_RIGHT | BTN_MIDDLE));
    const int32_t dx = (int32_t)(int8_t)data[1];
    const int32_t dy = (int32_t)(int8_t)data[2];
    const int32_t wheel = (len >= 4u) ? (int32_t)(int8_t)data[3] : 0;

    int32_t w, h;
    bounds(&w, &h);

    if (dx != 0 || dy != 0) {
        s_x = clamp(s_x + dx, w);
        s_y = clamp(s_y + dy, h);
        emit(AG_EV_POINTER_MOVE, (int16_t)dx, (int16_t)dy, buttons);
    }

    /*
     * Button edges, not levels.  A held button arrives in every report while it
     * is held, and an application that saw one POINTER_DOWN per report would
     * see a click for every millimetre of drag.
     */
    const uint8_t changed = (uint8_t)(buttons ^ s_buttons);
    if (changed != 0) {
        for (uint8_t bit = BTN_LEFT; bit <= BTN_MIDDLE; bit = (uint8_t)(bit << 1)) {
            if ((changed & bit) == 0) {
                continue;
            }
            emit((buttons & bit) ? AG_EV_POINTER_DOWN : AG_EV_POINTER_UP, 0, 0,
                 buttons);
        }
        s_buttons = buttons;
    }

    if (wheel != 0) {
        /* dy carries the notches, the way the terminal's wheel report does. */
        emit(AG_EV_WHEEL, 0, (int16_t)wheel, buttons);
    }
}

#endif /* AG_PORT_HAS_BT || AG_PORT_HAS_USB_HID */
