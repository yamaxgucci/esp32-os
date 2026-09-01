/*
 * ArgonOS port contract - RMT, the pulse-train generator, driving WS2812 LEDs.
 *
 * The RMT peripheral clocks out arbitrary on/off pulses with nanosecond timing,
 * which is how a chip with no dedicated LED driver still talks to a WS2812
 * ("NeoPixel") strip: each colour bit is a short or a long high pulse a few
 * hundred nanoseconds wide, far too fast to bit-bang from a task.  The encoding
 * is fixed by the LED, so it lives here; the kernel just hands down the colours.
 *
 * grb is three bytes per LED in the green-red-blue order the wire expects, len
 * the byte count (3 x the number of LEDs).  One call configures the channel,
 * clocks the whole chain out, holds the reset latch, and tears the channel down.
 * Returns 0, or negative if the channel could not be brought up.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_RMT_H
#define ARGON_PORT_RMT_H

#include <stdint.h>

int ag_port_rmt_ws2812(int pin, const uint8_t *grb, int len);

#endif /* ARGON_PORT_RMT_H */
