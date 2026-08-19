/*
 * ArgonOS port contract - sound the machine can actually make.
 *
 * Everything above this line deals in interleaved signed 16-bit frames at a
 * sample rate, because that is what a synthesiser or an emulator produces and
 * what /dev/pcm0 accepts.  What a particular machine wants underneath is its
 * own business: an I2S codec wants exactly those frames, and the original
 * ESP32 has a pair of 8-bit digital-to-analogue converters that want unsigned
 * bytes at half the channels.  The conversion belongs here rather than in the
 * kernel, so that a board with a codec needs no change above.
 *
 * What a port must supply:
 *
 *   bool    ag_port_audio_present(void)
 *   bool    ag_port_audio_open(uint32_t rate, uint8_t channels)
 *   void    ag_port_audio_close(void)
 *   int32_t ag_port_audio_write(const int16_t *pcm, int32_t frames)
 *   int32_t ag_port_audio_space(void)
 *
 * present() is asked once, at boot, and decides whether /dev/pcm0 exists at
 * all.  It answers what the machine was built with, not what is working: a
 * board with no output at all is the common case, and on it the system runs
 * exactly as before with /dev/pcmnull.
 *
 * open() is deferred until something actually plays.  Sound hardware costs
 * DMA buffers and a peripheral, and a machine sitting at a prompt should pay
 * for neither.  close() gives them back.
 *
 * write() returns frames accepted, which may be fewer than asked for, or a
 * negative -AG_Exxx.  It may block for as long as the buffer it is filling
 * takes to drain - about a tenth of a second - because the alternative is to
 * drop the sound, and a caller that has audio to play has nothing better to do
 * meanwhile.  It must not block forever.
 *
 * space() is a hint in frames: how much can be written now without waiting.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_AUDIO_H
#define ARGON_PORT_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

bool    ag_port_audio_present(void);
bool    ag_port_audio_open(uint32_t rate, uint8_t channels);
void    ag_port_audio_close(void);
int32_t ag_port_audio_write(const int16_t *pcm, int32_t frames);
int32_t ag_port_audio_space(void);

#endif /* ARGON_PORT_AUDIO_H */
