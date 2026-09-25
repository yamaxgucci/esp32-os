/*
 * ArgonOS - a test tone straight to the audio output.
 *
 * A pure sine at a frequency, a level and a length you choose, into the
 * system's sound - wherever `dev default audio` says it comes out, this board
 * or a neighbour - or into a device you name:
 *
 *   run t:\tone.axe                 440 Hz, level 50, 15 s, the default
 *   run t:\tone.axe 1000 80 20      1 kHz, level 80, 20 s
 *   run t:\tone.axe 440 60 8 pcm0   this board's own converter, by name
 *   run t:\tone.axe 440 60 8 cyd.pcm0   a neighbour's, by the name `dev` lists
 *   run t:\tone.axe sweep 60 30     a glide, 200 -> 1600 -> 200 Hz
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
    /*
     * "sweep" in place of a frequency: the pitch glides 200 -> 1600 -> 200 Hz,
     * one way in four seconds.  A dropout on a steady tone is a click; on a
     * glide it is also a jump in the pitch, so a gap in a stream that crosses
     * a wire is heard even when it is short.
     */
    const bool sweep = (argc > 1) && argv[1][0] == 's';
    int freq = sweep ? 200 : parse_int((argc > 1) ? argv[1] : NULL, 440);
    int vol  = parse_int((argc > 2) ? argv[2] : NULL, 50);
    int secs = parse_int((argc > 3) ? argv[3] : NULL, 15);

    if (freq < 20) freq = 20;
    if (freq > 20000) freq = 20000;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    if (secs < 1) secs = 1;

    /*
     * The mixer by default, not /dev/pcm0: pcm0 is THIS board's converter, and
     * on a board with none (the C3) the tone had nowhere to go even with a
     * neighbour's speaker in the device list.  The mixer sends it to the
     * system's sound device, which is the one `dev default audio` names.
     */
    char        path[40] = "/dev/";
    const char *name = (argc > 4) ? argv[4] : "pcmmix";
    size_t      at = 5;
    for (size_t k = 0; name[k] != '\0' && at + 1 < sizeof(path); k++) {
        path[at++] = name[k];
    }
    path[at] = '\0';

    const int16_t amp = (int16_t)(32767 * vol / 100);
    if (sweep) {
        ag_printf("tone: sweep 200-1600 Hz, level %d (amp %d), %d s -> %s\n",
                  vol, (int)amp, secs, path);
    } else {
        ag_printf("tone: %d Hz, level %d (amp %d), %d s -> %s\n", freq, vol,
                  (int)amp, secs, path);
    }
    ag_printf("Ctrl+C to stop.\n");

    const ag_handle_t h = ag_audio_out_open_dev(path, RATE_HZ, CHANNELS);
    if (h < 0) {
        ag_printf("cannot open %s (%d)\n", path, (int)h);
        return 1;
    }

    /*
     * The pitch is a ratio per sample: log-linear, so each octave takes the
     * same time and the glide sounds even rather than rushing at the top.
     * 200 -> 1600 is three octaves in four seconds.
     */
    const float up = powf(1600.0f / 200.0f, 1.0f / (4.0f * (float)RATE_HZ));
    float       hz = (float)freq;
    float       dir = up;
    float       phase = 0.0f;
    const uint64_t want = (uint64_t)secs * RATE_HZ;
    uint64_t       done = 0;

    while (done < want && !ag_interrupted()) {
        for (int i = 0; i < FRAMES; i++) {
            const int16_t smp = (int16_t)((float)amp * sinf(phase));
            s_buf[i * 2] = smp;
            s_buf[i * 2 + 1] = smp;
            phase += 6.2831853f * hz / (float)RATE_HZ;
            if (phase >= 6.2831853f) {
                phase -= 6.2831853f;
            }
            if (sweep) {
                hz *= dir;
                if (hz >= 1600.0f) {
                    dir = 1.0f / up;
                } else if (hz <= 200.0f) {
                    dir = up;
                }
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
