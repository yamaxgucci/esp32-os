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
 *   run t:\tone.axe sweep 30 60     a siren, 700 Hz +-25%
 *   run t:\tone.axe beep 30         three short beeps, a marker
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

/*
 * One cycle of sine, computed once.  A sinf() per sample is 44 100 a second
 * of soft float on a chip with no FPU, and the C3 could not keep up: thirty
 * seconds of siren took it 31.3, and the mixer filled what was late with
 * silence - a stutter Maxim heard and blamed, rightly at first, on the wire
 * (25 Sep 2026).  A table read is the same sound at no cost.
 */
#define TABLE 1024
static int16_t s_sine[TABLE];

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
     * "sweep" in place of a frequency: a siren, 700 Hz swinging 25% either way
     * and back, two seconds each way - Maxim's ask, after a 200-1600 Hz
     * glide that was loud, high and too busy to hear a fault in.  A dropout
     * on a slow drift is a hole in a sound that is otherwise always there.
     */
    const bool sweep = (argc > 1) && argv[1][0] == 's';
    /*
     * "beep": three short beeps and nothing else - 880 Hz, 150 ms each, 150 ms
     * apart.  A marker, so that a listener comparing two sounds knows which one
     * is starting.
     */
    const bool beep = (argc > 1) && argv[1][0] == 'b';
    int freq = sweep ? 700 : beep ? 880
                                  : parse_int((argc > 1) ? argv[1] : NULL, 440);
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
        ag_printf("tone: siren 525-875 Hz, level %d (amp %d), %d s -> %s\n",
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
     * The pitch moves by the same number of hertz every sample: 525 -> 875 Hz
     * (700 +-25%) in two seconds and back.  It was a ratio per sample from
     * powf(hi/lo, 1/(2*rate)), and this libc's powf gives exactly 1.0 for an
     * exponent that small - a "siren" of one steady note (25 Sep 2026).
     */
    const float lo = 525.0f;
    const float hi = 875.0f;
    const float step = (hi - lo) / (2.0f * (float)RATE_HZ);
    float       hz = (float)freq;
    float       dir = step;
    float       phase = 0.0f;
    for (int i = 0; i < TABLE; i++) {
        s_sine[i] = (int16_t)((float)amp * sinf(6.2831853f * (float)i / (float)TABLE));
    }

    const uint64_t want = beep ? (uint64_t)RATE_HZ * 9u / 10u
                               : (uint64_t)secs * RATE_HZ;
    uint64_t       done = 0;

    while (done < want && !ag_interrupted()) {
        for (int i = 0; i < FRAMES; i++) {
            /* in beep mode: on for 150 ms of every 300 */
            const bool on = !beep || ((done + (uint64_t)i) % (RATE_HZ * 3u / 10u)) <
                                         (RATE_HZ * 15u / 100u);
            const int16_t smp =
                on ? s_sine[(int)(phase * ((float)TABLE / 6.2831853f)) & (TABLE - 1)] : 0;
            s_buf[i * 2] = smp;
            s_buf[i * 2 + 1] = smp;
            phase += 6.2831853f * hz / (float)RATE_HZ;
            if (phase >= 6.2831853f) {
                phase -= 6.2831853f;
            }
            if (sweep) {
                hz += dir;
                if (hz >= hi) {
                    dir = -step;
                } else if (hz <= lo) {
                    dir = step;
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
