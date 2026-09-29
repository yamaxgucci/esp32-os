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
#include "ag_dsp.h"

AG_APP("LOOPBACK", "1.0", "argon", 0);

#define RATE     44100u
#define CHANNELS 2
#define CHUNK    256u

/* A tenth of full scale: above the noise floor a dead codec returns, far
 * below anything the cable carries when the chain is working. */
#define HYST     3000

/* Which 64-frame piece carries the timing burst: 200 of them is about a third
 * of a second, which is past the codec's own setup.  It was 40 while the
 * probe still used 256-frame pieces, and moving to 64 quietly made that 58 ms
 * - early enough that the burst went into a chip still muted, and the probe
 * reported nothing three times running. */
#define BURST_AT 200u

static int16_t s_out[CHUNK * CHANNELS];
static int16_t s_in[CHUNK * CHANNELS];
static int      s_shown;

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
    uint32_t      crossings = 0;
    int           armed_high = 0;
    int32_t       starved = 0;
    const uint32_t want_blocks =
        (secs > 0) ? ((uint32_t)secs * RATE / CHUNK) : 0u;
    const uint32_t say_every = RATE / CHUNK; /* about a second */

    /*
     * ROUND TRIP, MEASURED, before anything else runs.
     *
     * One full-scale sample out and the count of frames until it comes back:
     * that is the whole path - the output ring, the converter, the cable, the
     * input ring - in one number, and it is the number a guitarist is
     * complaining about when a pedal feels slow.  Arithmetic over buffer sizes
     * predicts it; this measures it, and the two disagreeing is how a
     * forgotten buffer is found.
     *
     * The click is sent alone and the search is for anything above a tenth of
     * full scale, so a noise floor cannot answer for it.
     */
    {
        /*
         * ROUND TRIP, MEASURED, and by the clock rather than by counting
         * frames.
         *
         * Counting was tried and is wrong: the input stream starts later than
         * the output one, so the two counters have no common origin, and the
         * answer came out at 21 frames when the buffers alone are 256.  Two
         * timestamps have no such problem, and they measure exactly what a
         * player feels - from the application producing a sample to the
         * application seeing it again.
         *
         * The probe runs in 64-frame pieces so the answer lands within a
         * millisecond and a half, and the burst goes out a quarter of a second
         * in: opening the device programs the codec, which is muted for most
         * of that, and a burst sent earlier reads as "loudest 129".
         */
        const uint32_t pchunk = 64u;
        uint32_t       sent, took = 0;
        int32_t        seen = 0;
        ag_time_t      t_burst = 0;

        ag_pcm_set_chunk(&pcm, pchunk);
        for (sent = 0; sent < BURST_AT + 200u && took == 0u; sent++) {
            uint32_t j;
            for (j = 0; j < pchunk * CHANNELS; j++) {
                s_out[j] = 0;
            }
            if (sent == BURST_AT) {
                /*
                 * Sixteen samples rather than one: a single impulse comes back
                 * through the converter's filters spread and small, and never
                 * rose above the threshold at all.
                 */
                for (j = 0; j < 16u; j++) {
                    s_out[j * CHANNELS] = 30000;
                    s_out[j * CHANNELS + 1u] = 30000;
                }
            }
            if (ag_pcm_write(&pcm, s_out, (int32_t)pchunk) < 0) {
                break;
            }
            if (sent == BURST_AT) {
                t_burst = ag_micros();
            }
            {
                const int32_t got = ag_pcm_read(&pcm, s_in, (int32_t)pchunk);
                int32_t       j2;
                for (j2 = 0; j2 < got && took == 0u; j2++) {
                    const int16_t v = s_in[j2 * CHANNELS];
                    const int32_t a = (v < 0) ? -(int32_t)v : (int32_t)v;
                    if (a > seen) {
                        seen = a;
                    }
                    if (a > 3000 && sent >= BURST_AT) {
                        took = (uint32_t)(ag_micros() - t_burst);
                    }
                }
            }
        }
        ag_pcm_set_chunk(&pcm, CHUNK);
        if (took > 0u) {
            ag_printf("loopback: round trip %u us\n", (unsigned)took);
        } else {
            ag_printf("loopback: nothing came back (loudest %d) - cable out?\n",
                      (int)seen);
        }
    }

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
        /*
         * The first block of every run, printed raw.
         *
         * Level and frequency between them describe three outcomes on this
         * board - a clean 440 Hz, a full-scale signal with no period at all,
         * and digital silence - and a summary cannot tell which of those is an
         * analogue fault and which is a frame the two ends disagree about.
         * Twelve samples can: alternating extremes are a bit alignment, a
         * constant is a converter that is not converting, and a ragged small
         * wave is the noise floor of one that is.
         */
        if (!s_shown && blocks > (RATE / CHUNK) && got > 6) {
            s_shown = 1;
            ag_printf("loopback: first samples %d %d %d %d %d %d %d %d "
                      "%d %d %d %d\n",
                      (int)s_in[0], (int)s_in[1], (int)s_in[2], (int)s_in[3],
                      (int)s_in[4], (int)s_in[5], (int)s_in[6], (int)s_in[7],
                      (int)s_in[8], (int)s_in[9], (int)s_in[10],
                      (int)s_in[11]);
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
        /*
         * HOW FAST THE RETURNED TONE ACTUALLY IS.
         *
         * A level says whether anything came back; it cannot say whether what
         * came back is the note that was sent.  The codec divides MCLK to make
         * its own converter clock, so a wrong MCLK does not silence it - it
         * shifts it, and a sine returning at twice or half its frequency is a
         * clock fault wearing the costume of a working board.  Counting sign
         * changes on the left channel is enough to tell 440 Hz from 880, which
         * is the distinction that matters; it is not a tuner.
         *
         * A hysteresis band keeps the noise floor from counting as a tone: at
         * peak 60 the crossings would otherwise read as several kilohertz of
         * nothing.
         */
        for (int32_t i = 0; i < got; i++) {
            const int16_t v = s_in[i * CHANNELS];
            if (armed_high && v < -HYST) {
                armed_high = 0;
                crossings++;
            } else if (!armed_high && v > HYST) {
                armed_high = 1;
                crossings++;
            }
        }
        in_frames += (uint32_t)got;

        blocks++;
        if (blocks % say_every == 0u) {
            /* Mean square, then an integer square root: the level wanted
             * here is "loud, quiet or nothing", and a decibel would need a
             * logarithm for no extra meaning.  ag_isqrt64 rather than a
             * Newton iteration written out here - see ag_dsp.h for what that
             * cost. */
            uint32_t rms = 0;
            if (in_frames > 0u) {
                rms = ag_isqrt64(in_sum /
                                 ((uint64_t)in_frames * CHANNELS));
            }
            /*
             * Raw counts, not percentages.  A percentage rounds the two
             * answers that matter into the same "0%": a converter delivering
             * digital zeros, which says nothing about the output at all, and
             * a converter delivering its own noise floor, which says the
             * input works and the signal is genuinely absent.  Those are
             * different faults and the print must not merge them.
             */
            /* Two crossings to a cycle, over the seconds those frames took. */
            const uint32_t hz =
                (in_frames > 0u)
                    ? (uint32_t)((uint64_t)crossings * RATE / (2ull * in_frames))
                    : 0u;
            ag_printf("loopback: out %d%%, in peak %u rms %u of 32767, "
                      "%u Hz back (%u frames, %d empty reads)\n",
                      vol, (unsigned)in_peak, (unsigned)rms, (unsigned)hz,
                      (unsigned)in_frames, (int)starved);
            in_peak = 0;
            in_sum = 0;
            in_frames = 0;
            crossings = 0;
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
