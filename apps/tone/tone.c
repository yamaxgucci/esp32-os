/*
 * ArgonOS - a test tone straight to the audio output.
 *
 * A pure sine into /dev/pcm0, at a frequency, a level and a length you choose:
 *
 *   run t:\tone.axe                 440 Hz, level 50, 15 s
 *   run t:\tone.axe 1000 80 20      1 kHz, level 80, 20 s
 *
 * The level IS the volume: this DAC has no gain of its own, so loudness is the
 * amplitude of the samples and nothing else.  A steady tone is also the
 * cleanest way to hear a break in the stream - a click or a crackle on a sine
 * is a dropout, where on music it hides.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

#include "audio_out.h"
#include "math.h"

AG_APP("TONE", "1.0", "argon", 0);

#define RATE_HZ   44100u
#define CHANNELS  2
#define FRAMES    512               /* per write; 11.6 ms at 44.1 kHz */

static int16_t s_buf[FRAMES * CHANNELS];

/* Tiny base-10 parse: no libc for one number, and a bad arg falls to the def. */
static int parse_int(const char *s, int def)
{
    if (s == NULL || *s == '\0') {
        return def;
    }
    int n = 0, sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    if (*s < '0' || *s > '9') {
        return def;
    }
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s++ - '0');
    }
    return n * sign;
}

int ag_main(int argc, char **argv)
{
    int freq = parse_int((argc > 1) ? argv[1] : NULL, 440);
    int vol  = parse_int((argc > 2) ? argv[2] : NULL, 50);
    int secs = parse_int((argc > 3) ? argv[3] : NULL, 15);

    if (freq < 20) freq = 20;
    if (freq > 20000) freq = 20000;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    if (secs < 1) secs = 1;

    const int16_t amp = (int16_t)(32767 * vol / 100);
    ag_printf("tone: %d Hz, level %d (amp %d), %d s -> /dev/pcm0\n", freq, vol,
              (int)amp, secs);
    ag_printf("Ctrl+C to stop.\n");

    const ag_handle_t h = ag_audio_out_open_dev("/dev/pcm0", RATE_HZ, CHANNELS);
    if (h < 0) {
        ag_printf("cannot open /dev/pcm0 (%d) - is [audio] set in BOARD.CFG?\n",
                  (int)h);
        return 1;
    }

    const float    step = 6.2831853f * (float)freq / (float)RATE_HZ;
    float          phase = 0.0f;
    const uint64_t want = (uint64_t)secs * RATE_HZ;
    uint64_t       done = 0;

    while (done < want && !ag_interrupted()) {
        for (int i = 0; i < FRAMES; i++) {
            const int16_t s = (int16_t)((float)amp * sinf(phase));
            s_buf[i * 2] = s;
            s_buf[i * 2 + 1] = s;
            phase += step;
            if (phase >= 6.2831853f) {
                phase -= 6.2831853f;
            }
        }
        const int32_t n = ag_dev_write(h, s_buf, sizeof(s_buf));
        if (n < 0) {
            ag_printf("write error %d\n", (int)n);
            break;
        }
        done += FRAMES;
    }

    (void)ag_dev_close(h);
    ag_printf("stopped\n");
    return 0;
}
