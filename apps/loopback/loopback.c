/*
 * ArgonOS - does the board hear itself?
 *
 * A tone out of the converter and, at the same instant, whatever is arriving
 * at its input, with a cable between the two.  It prints the level of what
 * came back once a second.
 *
 * This exists because of a whole day spent asking a person in another room
 * whether there was a sound.  Every question that could be answered from
 * inside the machine had been: the application started, the codec held every
 * register it was given, the I2S accepted every byte and consumed them at
 * 44074 Hz without a stumble for thirty-five seconds.  All of it said the
 * sound should be there, none of it said the sound WAS there, and the one
 * instrument that could tell the difference was somebody's ears - which are
 * not in the room at four in the morning and cannot be asked twice about the
 * same second.
 *
 * With a cable from the output jack to the input jack the machine becomes its
 * own witness, and the answer stops depending on who is standing next to it:
 *
 *   run c:\loopback.axe              440 Hz, level 80, until stopped
 *   run c:\loopback.axe 1000 50 20   1 kHz, level 50, twenty seconds
 *
 * What the numbers mean.  `out` is what is being written - it is generated
 * here, so it is a statement of intent and never a surprise.  `in` is what
 * came back, as a percentage of full scale.  A quiet reading is not
 * necessarily a fault of the output: the input may be the wrong jack, or the
 * codec may be listening to the microphones rather than the line, and both of
 * those look exactly like silence.  What a LOUD reading proves is the whole
 * chain at once, and that is worth more than any number of correct registers.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <string.h>

#include "ag_pcm.h"

AG_APP("LOOPBACK", "1.0", "argon", 0);

#define RATE     44100u
#define CHANNELS 2
#define CHUNK    256u

static int16_t s_out[CHUNK * CHANNELS];
static int16_t s_in[CHUNK * CHANNELS];

/* Digits only, so that one number does not drag a libc in. */
static int parse_int(const char *s, int def)
{
    int n = 0;
    if (s == NULL || *s == '\0' || *s < '0' || *s > '9') {
        return def;
    }
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s++ - '0');
    }
    return n;
}

/* A sine without libm: one multiply-accumulate pair per sample, which is a
 * resonator, and it stays a sine as long as nothing perturbs it. */
typedef struct {
    float s;
    float c;
    float k;
} osc_t;

static void osc_init(osc_t *o, int freq)
{
    const float w = 6.2831853f * (float)freq / (float)RATE;
    /* Small-angle start: exact enough at audio rates and needs no cosf. */
    o->k = 2.0f - w * w;
    o->s = 0.0f;
    o->c = w;
}

static float osc_next(osc_t *o)
{
    const float n = o->k * o->c - o->s;
    o->s = o->c;
    o->c = n;
    return o->s;
}

int ag_main(int argc, char **argv)
{
    const int freq = parse_int((argc > 1) ? argv[1] : NULL, 440);
    const int vol = parse_int((argc > 2) ? argv[2] : NULL, 80);
    const int secs = parse_int((argc > 3) ? argv[3] : NULL, 0);

    ag_pcm_t pcm;
    memset(&pcm, 0, sizeof(pcm));
    if (ag_pcm_open(&pcm, "/dev/pcm0", RATE, CHANNELS) != 0) {
        ag_printf("loopback: cannot open /dev/pcm0 - is [audio] in BOARD.CFG?\n");
        return 1;
    }
    ag_pcm_set_chunk(&pcm, CHUNK);

    ag_printf("loopback: %d Hz out at level %d, listening on the same device\n",
              freq, vol);
    if (secs > 0) {
        ag_printf("loopback: %d s, then it stops by itself\n", secs);
    } else {
        ag_printf("loopback: no time limit; reset the board to stop it\n");
    }

    osc_t         osc;
    const float   amp = 32767.0f * (float)vol / 100.0f;
    uint32_t      blocks = 0;
    uint32_t      in_peak = 0;
    uint64_t      in_sum = 0;   /* of squares, for a level that is not a spike */
    uint32_t      in_frames = 0;
    int32_t       starved = 0;
    const uint32_t want_blocks =
        (secs > 0) ? ((uint32_t)secs * RATE / CHUNK) : 0u;
    const uint32_t say_every = RATE / CHUNK; /* about a second */

    osc_init(&osc, freq);
    ag_pcm_pace_start(&pcm);

    for (;;) {
        for (uint32_t i = 0; i < CHUNK; i++) {
            const int16_t v = (int16_t)(osc_next(&osc) * amp);
            s_out[i * CHANNELS] = v;
            s_out[i * CHANNELS + 1] = v;
        }
        if (ag_pcm_write(&pcm, s_out, (int32_t)CHUNK) < 0) {
            ag_printf("loopback: the sink refused a block; stopping\n");
            break;
        }

        const int32_t got = ag_pcm_read(&pcm, s_in, (int32_t)CHUNK);
        if (got < 0) {
            /* -AG_ENOSYS means this machine cannot record at all, which is a
             * different answer from "nothing came back" and worth stopping
             * for rather than printing zeroes for ever. */
            ag_printf("loopback: this device does not record (%d)\n",
                      (int)got);
            break;
        }
        if (got == 0) {
            starved++;
        }
        for (int32_t i = 0; i < got * CHANNELS; i++) {
            int32_t v = s_in[i];
            if (v < 0) {
                v = -v;
            }
            if ((uint32_t)v > in_peak) {
                in_peak = (uint32_t)v;
            }
            in_sum += (uint64_t)((int64_t)s_in[i] * s_in[i]);
        }
        in_frames += (uint32_t)got;

        blocks++;
        if (blocks % say_every == 0u) {
            /* Mean square, then a crude integer square root: the level wanted
             * here is "loud, quiet or nothing", and a decibel would need a
             * logarithm for no extra meaning. */
            uint32_t rms = 0;
            if (in_frames > 0u) {
                const uint64_t mean =
                    in_sum / ((uint64_t)in_frames * CHANNELS);
                uint64_t r = mean;
                uint64_t last = 0;
                while (r != last && r > 0u) {
                    last = r;
                    r = (r + mean / r) / 2u;
                }
                rms = (uint32_t)r;
            }
            /*
             * Raw counts, not percentages.  A percentage rounds the two
             * answers that matter into the same "0%": a converter delivering
             * digital zeros, which says nothing about the output at all, and
             * a converter delivering its own noise floor, which says the
             * input works and the signal is genuinely absent.  Those are
             * different faults and the print must not merge them.
             */
            ag_printf("loopback: out %d%%, in peak %u rms %u of 32767 "
                      "(%u frames, %d empty reads)\n",
                      vol, (unsigned)in_peak, (unsigned)rms,
                      (unsigned)in_frames, (int)starved);
            in_peak = 0;
            in_sum = 0;
            in_frames = 0;
            starved = 0;
        }
        if (want_blocks > 0u && blocks >= want_blocks) {
            break;
        }
        ag_pcm_pace_wait(&pcm);
    }

    ag_pcm_close(&pcm);
    return 0;
}
