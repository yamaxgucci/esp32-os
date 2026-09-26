/*
 * ArgonOS - waking an ES8388 up.
 *
 * A bare converter is ready the moment it is clocked.  A codec is not: it is a
 * small computer with sixty registers, and out of reset every one of them says
 * no.  Its converters are unpowered, its outputs are muted, its input switch
 * points at nothing.  Clock it without programming it and the board reports a
 * working audio device, the write calls all succeed, the pins carry real
 * samples - and it is silent.  That failure has no symptom to search for,
 * which is why this file exists and why it logs what it did.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_CODEC_ES8388_H
#define ARGON_CODEC_ES8388_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Programs the codec on `bus` for playback and capture at 16 bits, as an I2S
 * slave taking its clocks from us.  `capture` selects whether the input half
 * is powered at all.
 *
 * Answers false if nothing is at the codec's address, which is the honest
 * answer for a board that named an I2C bus and has no codec on it - the sound
 * still runs, it is simply nobody's job to unmute.  Call it after the I2S
 * clocks are already running: the chip divides MCLK internally, and a part
 * told to start converting with no clock to divide comes up in an undefined
 * state that the next write does not necessarily clear.
 */
bool ag_codec_es8388_begin(int bus, bool capture);

/* Puts the outputs back to muted and the converters back to unpowered, so a
 * board that stops playing does not sit with a live amplifier and an open
 * input.  Safe on a board where begin() answered false. */
void ag_codec_es8388_end(int bus);

#endif /* ARGON_CODEC_ES8388_H */
