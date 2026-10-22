/*
 * ArgonOS - where a mouse keeps its buttons and its axes, read off the
 * device's own report descriptor.
 *
 * The boot protocol fixes the layout - buttons, x, y, one byte each - and a
 * host that asks for it need not read anything.  Not every device honours the
 * request.  A Logitech receiver (046d:c542, a keyboard and a mouse on one
 * dongle) went on sending its report protocol: a report ID first, then x and y
 * as twelve bits each.  Read as boot, the ID was the buttons, the buttons were
 * x and the low byte of x was y - moving the mouse sideways moved the pointer
 * up and down, and nothing moved it sideways at all (8 Oct 2026).
 *
 * A PC never meets that, because its HID driver reads the descriptor and
 * decodes what it says.  This is the part of that driver a mouse needs: the
 * Input fields of the report that carries X and Y, their bit offsets and
 * sizes, and the buttons and wheel beside them.  Nothing else in a descriptor
 * is interpreted.
 *
 * Pure: no port, no state, so the host tests can feed it real descriptors.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_HIDREP_H
#define ARGON_HIDREP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bit offsets count from the first byte after the report ID (or from the
 * first byte, when report_id is 0 and the device uses none).  A size of 0
 * means the field is absent.
 */
typedef struct {
    uint8_t  report_id;
    uint16_t btn_off;
    uint8_t  btn_count;  /* at most 8 are kept */
    uint16_t x_off;
    uint8_t  x_size;
    uint16_t y_off;
    uint8_t  y_size;
    uint16_t wheel_off;
    uint8_t  wheel_size;
} ag_hidrep_mouse_t;

/*
 * Find the mouse in a report descriptor.  True when a report with both X and
 * Y was found; *out is then filled in.  False leaves *out zeroed - a device
 * this does not understand is left to the boot protocol.
 */
bool ag_hidrep_parse_mouse(const uint8_t *desc, size_t len,
                           ag_hidrep_mouse_t *out);

/*
 * One report through that layout.  False when the report is not the mouse's
 * (another report ID, or too short for the fields) - it is then not a move.
 */
bool ag_hidrep_decode_mouse(const ag_hidrep_mouse_t *m, const uint8_t *data,
                            size_t len, uint8_t *buttons, int32_t *dx,
                            int32_t *dy, int32_t *wheel);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_HIDREP_H */
