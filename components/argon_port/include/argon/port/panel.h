/*
 * ArgonOS port contract - a screen the machine can actually show.
 *
 * ArgonOS draws into a framebuffer of its own and knows nothing about panels:
 * the font, the blitter, the dirty rectangles, the double buffering and the
 * text console on top of them are all in src/dev/display.c and src/dev/draw.c,
 * and none of it is here.  What is here is the last step - putting rows on
 * something a person can look at.
 *
 * What a port must supply:
 *
 *   bool ag_port_panel_open(uint16_t w, uint16_t h, void **fb)
 *   void ag_port_panel_present(int32_t y, int32_t h)
 *
 * open() answers false when this machine has no panel, which is the common
 * case: the system runs perfectly well with a serial console and a soft
 * framebuffer nobody displays, and `gfxdump` writes that framebuffer to a file.
 *
 * On success *fb is RGB565, w*h*2 bytes, tightly packed, and it belongs to the
 * port - the caller draws into it and never frees it.  Handing back the panel's
 * own memory is the point: it means presenting is a register write rather than
 * a copy of the whole screen.
 *
 * present() shows rows y .. y+h.  Rows, not rectangles, and that is not a
 * simplification - it is the contract.  A partial-width region is not
 * contiguous in a framebuffer, and a panel that is handed a pointer plus a
 * width and a height reads w*h pixels straight from it: the top-left corner of
 * the frame gets painted into the region, which looks like console text
 * reappearing in the middle of the screen in diagonal bands.  Presenting whole
 * rows costs nothing on the caller's side, because the copy it paid for was
 * already narrow.
 *
 * present() must not block waiting for the panel.  A guest that spins on a
 * status bit belonging to something that only moves when the guest yields is a
 * deadlock, and the emulator is exactly that shape.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_PANEL_H
#define ARGON_PORT_PANEL_H

#include <stdbool.h>
#include <stdint.h>

bool ag_port_panel_open(uint16_t w, uint16_t h, void **fb);
void ag_port_panel_present(int32_t y, int32_t h);

#endif /* ARGON_PORT_PANEL_H */
