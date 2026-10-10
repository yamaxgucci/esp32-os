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
 *   void    ag_port_audio_pins(const ag_port_audio_pins_t *pins)
 *   const char *ag_port_audio_name(void)
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

/* For the -AG_Exxx a read answers with; the same header the other port
 * contracts take their error codes from. */
#include <argon/abi.h>

/*
 * Where the sound comes out, when that is the board's decision and not the
 * chip's.
 *
 * The original ESP32's converters are wired to two pins inside the part and
 * cannot be anywhere else, so a port for that chip ignores this entirely.  A
 * codec on I2S sits wherever whoever laid the board out put it, and the same
 * firmware runs on boards that disagree - so the pins come from BOARD.CFG,
 * they arrive before present() is asked, and on such a machine present()
 * answers yes exactly when the board has said where.
 *
 * Called once, at the devices stage, whether or not the board said anything:
 * a port that has no use for pins must still define this, and doing nothing is
 * the correct implementation rather than a missing one.
 */
typedef struct {
    int16_t  bclk;
    int16_t  ws;
    int16_t  dout;
    int16_t  mclk; /* -1 when the codec makes its own clock, as most do */
    /*
     * The line the codec sends on, and -1 on a board that only plays.  It
     * shares bclk and ws with dout because it is the same peripheral in the
     * other direction; see ag_port_audio_capture below for why that is not an
     * economy but a requirement.
     */
    int16_t  din;
    /*
     * The I2C bus the codec is programmed over, or -1 on a board whose output
     * needs no programming.  A converter wired straight to pins is ready the
     * moment it is clocked; a codec is a small computer that wakes up muted,
     * with its converters unpowered, and will sit through a whole song saying
     * nothing.  The bus is opened by the kernel before open() is called - the
     * port is told the number, not the wires, because the wires are already in
     * BOARD.CFG under [i2c0] and one place is enough.
     */
    int16_t  i2c;
    /* The codec's input preamplifier, in dB (0..24): what is plugged in, not
     * what sounds nice.  See ag_board_audio_t. */
    int16_t  ingain;
    /*
     * How many frames the converter may hold in each direction, or 0 for the
     * port's own default.
     *
     * This is latency, and on an instrument it is the whole difference between
     * an amplifier and a delay pedal.  The port's default is generous because
     * most sound is played rather than performed - a synthesiser or a game can
     * be forty milliseconds behind and nobody can tell.  A guitar cannot: the
     * player hears the string and the speaker at once, and twenty milliseconds
     * between them is felt in the hands.
     *
     * The floor is two of whatever the application writes at a time: one
     * buffer playing while the next is computed.  Below that the converter
     * runs dry between blocks and the sound breaks up, which is worse than
     * being late.
     */
    uint16_t dma_frames;
    uint32_t rate; /* the board's default; open() may still ask for another */
} ag_port_audio_pins_t;

void ag_port_audio_pins(const ag_port_audio_pins_t *pins);

/*
 * What the output is, in one word, for `dev` to print: "dac", "i2s", "none".
 * The device table says who drives a device, and a codec listed as the chip's
 * own converter is the kind of small lie that costs an hour on the day
 * something is wrong with the sound.
 */
const char *ag_port_audio_name(void);

bool    ag_port_audio_present(void);
bool    ag_port_audio_open(uint32_t rate, uint8_t channels);

/*
 * The block the stream means to work in, in frames.  Returns what it will
 * actually get, which can be less when the board's ceiling says so.
 *
 * The ring is two of these: one playing while the next is filled, which is
 * the floor for DMA and also the floor for not running dry.  Asking for more
 * than half the ring is the fault this exists to prevent - the read cannot be
 * answered inside the block it belongs to, so every block is born late and
 * the wait is counted as if it were work.
 *
 * Call it before the converter opens; after, it takes effect on the next
 * open, because the ring is cut when the channel is created.
 */
uint32_t ag_port_audio_period(uint32_t frames);
void    ag_port_audio_close(void);
int32_t ag_port_audio_write(const int16_t *pcm, int32_t frames);
int32_t ag_port_audio_space(void);
/* How many times the converter ran out and stopped since the start: each is a
 * click.  0 where it is not counted. */
uint32_t ag_port_audio_stops(void);
/* The rate the converter really plays at, in millihertz of the samples it was
 * given, measured by its own clock since the last call - 0 where it is not
 * measured or nothing played. */
uint32_t ag_port_audio_rate_mhz(void);

/*
 * SOUND COMING IN
 *
 * capture() answers whether this machine can record at all, which is a
 * question about the board rather than about the chip: a codec wired for both
 * directions has a data line going the other way and a board with an amplifier
 * on a pin does not.  It answers what the board said, exactly as present()
 * does, and it may only be true once open() has succeeded.
 *
 * **There is no separate open for it, and that is the point.**  Recording and
 * playing share one peripheral and one clock, because a converter fed by one
 * clock and read by another drifts, and the drift is heard as a crackle every
 * few seconds as the two buffers slide past each other.  The pedal this is for
 * lives or dies on that - see the Blackstomp notes in docs/10-watch-board.md,
 * where one I2S port in both directions is the first thing their design does.
 * So open() brings up whatever the board has, both directions together, and a
 * port that cannot do that must bring up neither.
 *
 * read() returns frames delivered, which may be fewer than asked for, or a
 * negative -AG_Exxx.  It may block for as long as the frames it is waiting for
 * take to arrive; like write() it must not block forever.  On a machine with
 * no input it answers -AG_ENOSYS rather than silence, because silence is a
 * thing a microphone can legitimately produce and "there is no microphone" is
 * not.
 */
bool    ag_port_audio_capture(void);
int32_t ag_port_audio_read(int16_t *pcm, int32_t frames);

#endif /* ARGON_PORT_AUDIO_H */
