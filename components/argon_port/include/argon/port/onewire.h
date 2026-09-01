/*
 * ArgonOS port contract - 1-Wire (Dallas/Maxim), one open-drain pin.
 *
 * 1-Wire is a single line with a pull-up: a device is read by whether it holds
 * the line down inside a time slot a few microseconds wide, so the timing is
 * the protocol and it has to live down here where interrupts can be held off.
 * The kernel does the device on top - ROM commands, the DS18B20's registers,
 * the CRC - through nothing but these three calls.
 *
 * reset() pulses the line and reports whether anything answered: 1 a device is
 * present, 0 the line is idle, negative on error.  write()/read() move whole
 * bytes least-significant bit first, the order every 1-Wire part expects.  The
 * line needs an external ~4.7k pull-up to 3.3 V; the internal one is enabled as
 * a weak fallback but is too soft for anything but the shortest wire.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_ONEWIRE_H
#define ARGON_PORT_ONEWIRE_H

#include <stdint.h>

int  ag_port_ow_reset(int pin);
void ag_port_ow_write(int pin, const uint8_t *buf, int len);
void ag_port_ow_read(int pin, uint8_t *buf, int len);

#endif /* ARGON_PORT_ONEWIRE_H */
