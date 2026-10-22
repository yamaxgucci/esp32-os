/*
 * ArgonOS - play a raw PCM file straight at /dev/pcm0, and say how fast it went.
 *
 *   run c:\pcmplay.axe c:\music.raw            22050 Hz, 8-bit unsigned mono
 *   run c:\pcmplay.axe c:\music.raw 22050 16   signed 16-bit mono
 *
 * This exists to answer one question that nothing else could: **can this board
 * play music at all?**  Sound reaching a speaker down a wire from another
 * machine has four places to be wrong - the sender's mixing, the wire, the
 * receiver's buffering, and the converter - and "it plays something, but it is
 * not music" does not say which.  A file on the board's own disk, played by the
 * board, with no wire involved, removes three of them at once.
 *
 * Eight-bit unsigned is the default because that is what the video link carries
 * (apps/remdisp), so `pcmplay` and the link put the same numbers on the same
 * pin.  If the file sounds right here and wrong over the wire, the wire is at
 * fault; if it sounds the same both ways, the converter is.
 *
 * The rate it reports is the one that matters and it is not the one asked for.
 * A converter has a clock divider and takes what it can get: a request for
 * 22050 may come out as something else entirely, and music played at the wrong
 * rate is exactly "not music" - the right samples in the wrong time.  Nothing
 * else on the board measures this, and it cannot be heard as a number.  So the
 * frames actually accepted are counted against the clock:
 *
 *   played 121500 frames in 5512 ms = 22042 Hz (asked for 22050)
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_APP("PCMPLAY", "1.0", "argon", 0);

/* 1024 frames is 46 ms at 22 kHz: long enough that the call overhead is
 * nothing, short enough that a sink which paces by refusing does not have to
 * refuse much. */
#define CHUNK 1024u

static uint8_t s_in[CHUNK];
static int16_t s_out[CHUNK];

static bool parse_u32(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    uint32_t v = 0;
    for (; *s != '\0'; ++s) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        v = v * 10u + (uint32_t)(*s - '0');
    }
    *out = v;
    return true;
}

/*
 * A tune the board makes itself, because getting a file onto a board that has
 * no card takes ten minutes over the console and the question does not need
 * one.  Eight bars of something with an obvious shape: if this comes out as a
 * melody the converter and the amplifier are fine and the fault is upstream; if
 * it comes out as noise, or at the wrong speed, it is not.
 *
 * A square-ish wave with a decay, not a sine: a sine through an eight-bit
 * quantiser is mostly quantisation noise at low level, which would blame the
 * eight bits for something the DAC did.  Notes are A minor because it does not
 * matter and it has to be something.
 */
static const uint16_t k_tune[] = {
    440, 523, 659, 523, 440, 392, 440, 0,
    440, 523, 659, 784, 659, 523, 440, 0,
    392, 440, 523, 440, 392, 349, 330, 0,
    330, 392, 440, 523, 440, 392, 330, 0,
};

static int play_tune(ag_handle_t pcm, uint32_t rate, uint32_t bits)
{
    const uint32_t per_note = rate / 5u; /* 200 ms */
    uint64_t       frames = 0;

    for (unsigned n = 0; n < sizeof(k_tune) / sizeof(k_tune[0]); n++) {
        const uint32_t f = k_tune[n];
        uint32_t       phase = 0;
        /* Fixed point: a whole turn is 65536, so the step is f * 65536 / rate
         * and no floating point is needed for a diagnostic. */
        const uint32_t step = f ? (uint32_t)(((uint64_t)f << 16) / rate) : 0u;

        for (uint32_t done = 0; done < per_note; done += CHUNK) {
            uint32_t take = per_note - done;
            if (take > CHUNK) {
                take = CHUNK;
            }
            for (uint32_t i = 0; i < take; i++) {
                int32_t v = 0;
                if (f != 0u) {
                    /* Triangle rather than square: a square at eight bits is
                     * only ever two values and says nothing about the rest. */
                    const uint32_t p = (phase >> 8) & 0xffu;
                    v = (p < 128u) ? ((int32_t)p * 2 - 128) * 200
                                   : ((int32_t)(255u - p) * 2 - 128) * 200;
                    /* Decay over the note, so notes are separable by ear. */
                    v = v * (int32_t)(per_note - done - i) /
                        (int32_t)per_note;
                    phase += step;
                }
                if (bits == 8u) {
                    /* Round-trip through eight bits, which is what the video
                     * link carries, so the two can be compared by ear. */
                    const uint8_t q = (uint8_t)(((v + 32768) >> 8) & 0xff);
                    s_out[i] = (int16_t)(((int32_t)q - 128) << 8);
                } else {
                    s_out[i] = (int16_t)v;
                }
            }
            uint32_t sent = 0;
            while (sent < take) {
                const int32_t w = ag_dev_write(pcm, s_out + sent,
                                               (size_t)(take - sent) *
                                                   sizeof(int16_t));
                if (w < 0) {
                    return -1;
                }
                if (w == 0) {
                    ag_delay(5);
                    continue;
                }
                sent += (uint32_t)w / sizeof(int16_t);
            }
            frames += take;
        }
        if (ag_kbhit()) {
            (void)ag_getch();
            break;
        }
    }
    return (int)frames;
}

int ag_main(int argc, char **argv)
{
    if (argc < 2) {
        ag_printf("usage: pcmplay <file>|-tune [rate] [bits] [dev] [-loop]\n");
        ag_printf("  raw samples, one channel; bits is 8 (unsigned) or 16\n");
        ag_printf("  -loop repeats the file until a key is pressed\n");
        ag_printf("  -tune plays a melody the board makes itself\n");
        ag_printf("  device is /dev/pcm0 unless given; /dev/pcmmix goes\n");
        ag_printf("  through the mixer to whatever sink is loaded - on the\n");
        ag_printf("  S3 that is the wire to the board that has a speaker\n");
        return 1;
    }

    uint32_t rate = 22050u;
    uint32_t bits = 8u;
    if (argc >= 3 && !parse_u32(argv[2], &rate)) {
        ag_printf("pcmplay: %s is not a rate\n", argv[2]);
        return 1;
    }
    if (argc >= 4 && !parse_u32(argv[3], &bits)) {
        ag_printf("pcmplay: %s is not a bit count\n", argv[3]);
        return 1;
    }
    if (bits != 8u && bits != 16u) {
        ag_printf("pcmplay: bits must be 8 or 16\n");
        return 1;
    }

    bool loop = false;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'l') {
            loop = true;
        }
    }

    const bool tune = argv[1][0] == '-' && argv[1][1] == 't';

    ag_handle_t f = -1;
    if (!tune) {
        f = ag_open(argv[1], AG_O_RDONLY);
        if (f < 0) {
            ag_printf("pcmplay: cannot open %s (%s)\n", argv[1],
                      ag_strerror((ag_err_t)f));
            return 1;
        }
    }

    /*
     * Which output, and this argument is the whole point: the same file played
     * to /dev/pcm0 on the board that has the speaker, and to /dev/pcmmix on the
     * board that has not, puts the same numbers on the same speaker by two
     * different roads.  If they sound different the road is at fault; if they
     * sound the same, whatever is upstream of it is.
     */
    const char       *devname = (argc >= 5) ? argv[4] : "/dev/pcm0";
    const ag_handle_t pcm = ag_dev_open(devname);
    if (pcm < 0) {
        ag_printf("pcmplay: cannot open %s (%s)\n", devname,
                  ag_strerror((ag_err_t)pcm));
        if (f >= 0) {
            (void)ag_close(f);
        }
        return 1;
    }
    {
        const ag_audio_fmt_t fmt = {.rate = rate, .channels = 1u, .bits = 16u};
        (void)ag_dev_ioctl(pcm, AG_IOC_AUDIO_SETFMT, (void *)&fmt, sizeof(fmt));
    }

    ag_printf("pcmplay: %s -> %s, %u Hz, %u-bit mono\n",
              tune ? "built-in tune" : argv[1], devname, (unsigned)rate,
              (unsigned)bits);

    /*
     * The Enter that started this is still in the queue, and both loops below
     * stop on a keypress.  Without this the file path reads nothing at all and
     * reports "0 frames in 1 ms", which reads like a broken file and is a
     * broken program.
     */
    ag_flush_input();

    const uint32_t t0 = (uint32_t)ag_millis();
    uint64_t       frames = 0;
    uint32_t       stalls = 0;

    if (tune) {
        const int n = play_tune(pcm, rate, bits);
        frames = (n > 0) ? (uint64_t)n : 0u;
    }

    while (!tune) {
        if (ag_kbhit()) {
            (void)ag_getch();
            break;
        }

        int32_t got;
        if (bits == 8u) {
            got = ag_read(f, s_in, sizeof(s_in));
            if (got <= 0) {
                if (loop && ag_seek(f, 0, AG_SEEK_SET) >= 0) {
                    continue;
                }
                break;
            }
            for (int32_t i = 0; i < got; i++) {
                /* Unsigned eight-bit to signed sixteen, which is what the far
                 * end of the video link does with the same bytes. */
                s_out[i] = (int16_t)(((int32_t)s_in[i] - 128) << 8);
            }
        } else {
            got = ag_read(f, s_out, sizeof(s_out));
            if (got <= 0) {
                if (loop && ag_seek(f, 0, AG_SEEK_SET) >= 0) {
                    continue;
                }
                break;
            }
            got /= (int32_t)sizeof(int16_t);
        }

        /*
         * Offer the whole chunk, take note of what was accepted, and offer the
         * rest again.  A sink is allowed to take less than it is given - that
         * is how it says "wait" without blocking - and a player that assumes
         * otherwise loses exactly the samples the sink was busy for.
         */
        int32_t done = 0;
        while (done < got) {
            const int32_t n = ag_dev_write(pcm, s_out + done,
                                           (size_t)(got - done) *
                                               sizeof(int16_t));
            if (n < 0) {
                ag_printf("pcmplay: write failed (%s)\n",
                          ag_strerror((ag_err_t)n));
                done = got;
                break;
            }
            if (n == 0) {
                stalls++;
                ag_delay(5);
                continue;
            }
            done += n / (int32_t)sizeof(int16_t);
        }
        frames += (uint64_t)got;

        /*
         * Keep to the clock, because not every sink does.
         *
         * /dev/pcm0 paces by refusing - it holds about ninety milliseconds and
         * takes no more until the converter has played some.  /dev/pcmmix does
         * not: it has a four-kilobyte ring per client, accepts whatever it is
         * given and drops the surplus, so a player that just writes as fast as
         * it can gets 122868 frames accepted in 318 ms - **386 kHz** - and what
         * comes out of the speaker is a fifth of a second of noise.  Measured,
         * on the first attempt to play this file down the wire.
         *
         * So the clock lives here.  A lead of a tenth of a second, which is
         * enough that a sink is never starved by this sleep and short enough
         * that stopping the program stops the sound.
         */
        const uint32_t want_ms =
            (uint32_t)((frames * 1000ull) / (uint64_t)rate);
        const uint32_t gone_ms = (uint32_t)ag_millis() - t0;
        if (want_ms > gone_ms + 100u) {
            ag_delay(want_ms - gone_ms - 100u);
        }
    }

    const uint32_t ms = (uint32_t)ag_millis() - t0;
    (void)ag_dev_close(pcm);
    if (f >= 0) {
        (void)ag_close(f);
    }

    if (ms != 0u) {
        ag_printf("pcmplay: %u frames in %u ms = %u Hz (asked for %u), "
                  "%u waits\n",
                  (unsigned)frames, (unsigned)ms,
                  (unsigned)((frames * 1000u) / ms), (unsigned)rate,
                  (unsigned)stalls);
    }
    return 0;
}
