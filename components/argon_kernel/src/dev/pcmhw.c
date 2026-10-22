/*
 * ArgonOS - /dev/pcm0: the sound the machine itself can make.
 *
 * Everything about how it makes it is below, in argon/port/audio.h.  What is
 * here is the device: a format, an exclusive open, and writes that go straight
 * through.  Registered only when the port says the machine has an output at
 * all, so on a board without one nothing appears and /dev/pcmnull stays the
 * only sink, exactly as before.
 *
 * The name is the one the applications already look for: `--out audio` and
 * audio_out=i2s in a config both resolve to /dev/pcm0 (apps/common/audio_out.h),
 * which is why a synthesiser written before this existed plays through it
 * without being touched.
 *
 * The hardware is opened on the first write rather than on open(), because
 * open() is also what a program does to ask what format the device wants, and
 * spinning up DMA to answer a question is not worth four kilobytes.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "argon/audio.h"
#include "argon/board.h"
#include "argon/device.h"
#include "argon/log.h"

#include <argon/port/audio.h>
#include <argon/port/time.h>

/*
 * Nothing at all where the build has no output.
 *
 * ag_port_audio_present() already answers false there and pcm0 is never
 * registered, but the format, the device pointer and the counters were held
 * regardless - thirty bytes of a data segment that, on the S3, has about twenty
 * to spare.  Small, and the third time this branch spent memory on a possibility
 * rather than a thing.
 */
#if CONFIG_ARGON_ENABLE_AUDIO

#define TAG "pcm0"

static ag_device_t    *s_dev;
static ag_audio_fmt_t  s_fmt;
static bool            s_hw_open;
static uint64_t        s_frames_in;
static uint64_t        s_frames_out;
static uint32_t        s_peak; /* the loudest sample since the last look */
/*
 * SINCE THE START, NOT SINCE THE OPEN (2 Oct 2026).  The two above begin
 * again at every open, as the stats of that open; the proof that a sound was
 * played cannot.  After the S3's tone, the CYD's beep and the tone again, the
 * second tone read exactly as the first - the same frames and milliseconds -
 * and "it played again" could not be told from "nothing changed".
 */
static uint32_t        s_all_in, s_all_out;
static uint64_t        s_all_us;

static void default_fmt(ag_audio_fmt_t *out)
{
    const ag_board_audio_t *a = &ag_board()->audio;
    out->rate = a->rate ? a->rate : 22050u;
    out->channels = 2;
    out->bits = 16;
}

static int fmt_ok(const ag_audio_fmt_t *fmt)
{
    return fmt != NULL && fmt->rate >= 8000u && fmt->rate <= 48000u &&
           fmt->bits == 16u && (fmt->channels == 1u || fmt->channels == 2u);
}

static void hw_close(void)
{
    if (s_hw_open) {
        ag_port_audio_close();
        s_hw_open = false;
    }
}

/*
 * A FAILED START IS NOT TRIED AGAIN ON THE NEXT WRITE.  A write comes every
 * period, and a converter that cannot start - no DMA memory for its
 * descriptors - was tried again every six milliseconds, each failure four
 * lines from the driver below: the Zero spent 64% of a core printing them,
 * its picture stopped going out, and its console could not be heard over them
 * (26 Sep 2026).  After a failure the next try waits a second, then two, then
 * four, up to eight; the writes in between are refused at once.
 */
static int64_t  s_retry_us;
static uint32_t s_backoff_ms;

static ag_err_t hw_open(void)
{
    if (s_hw_open) {
        return AG_OK;
    }
    const int64_t now = ag_port_us();
    if (s_retry_us != 0 && now < s_retry_us) {
        return -AG_EIO;
    }
    if (!ag_port_audio_open(s_fmt.rate, s_fmt.channels)) {
        s_backoff_ms = (s_backoff_ms == 0u) ? 1000u
                       : (s_backoff_ms < 8000u) ? s_backoff_ms * 2u
                                                : 8000u;
        s_retry_us = now + (int64_t)s_backoff_ms * 1000;
        ag_log(AG_LOG_WARN, TAG,
               "the converter would not start (no DMA memory?); trying again "
               "in %u ms", (unsigned)s_backoff_ms);
        return -AG_EIO;
    }
    s_retry_us = 0;
    s_backoff_ms = 0;
    s_hw_open = true;
    return AG_OK;
}

static ag_err_t pcm_open(ag_device_t *dev, uint32_t flags)
{
    (void)dev;
    (void)flags;
    s_frames_in = 0;
    s_frames_out = 0;
    return AG_OK;
}

static ag_err_t pcm_close(ag_device_t *dev)
{
    (void)dev;
    hw_close();
    return AG_OK;
}

static ag_err_t pcm_ioctl(ag_device_t *dev, uint32_t cmd, void *arg,
                          size_t arglen)
{
    (void)dev;

    if (cmd == AG_IOC_AUDIO_GETFMT) {
        if (arg == NULL || arglen < sizeof(ag_audio_fmt_t)) {
            return -AG_EINVAL;
        }
        *(ag_audio_fmt_t *)arg = s_fmt;
        return AG_OK;
    }
    if (cmd == AG_IOC_AUDIO_SETFMT) {
        if (arg == NULL || arglen < sizeof(ag_audio_fmt_t)) {
            return -AG_EINVAL;
        }
        const ag_audio_fmt_t *want = (const ag_audio_fmt_t *)arg;
        if (!fmt_ok(want)) {
            return -AG_EINVAL;
        }
        /* A rate change is a different clock divider, so the converter has to
         * be taken down and brought back up - on the next write, not now. */
        if (want->rate != s_fmt.rate || want->channels != s_fmt.channels) {
            hw_close();
        }
        s_fmt = *want;
        return AG_OK;
    }
    if (cmd == AG_IOC_AUDIO_GETSTATS) {
        if (arg == NULL || arglen < sizeof(ag_audio_stats_t)) {
            return -AG_EINVAL;
        }
        ag_audio_stats_t *st = (ag_audio_stats_t *)arg;
        const size_t      fb = (size_t)s_fmt.channels * sizeof(int16_t);
        memset(st, 0, sizeof(*st));
        st->bytes_in = s_frames_in * fb;
        st->bytes_sent = s_frames_out * fb;
        st->bytes_drop_overflow = (s_frames_in - s_frames_out) * fb;
        st->ring_cap = (uint32_t)(ag_port_audio_space() * (int32_t)fb);
        return AG_OK;
    }
    if (cmd == AG_IOC_FLUSH) {
        return AG_OK;
    }
    if (cmd == AG_IOC_RESET) {
        hw_close();
        return AG_OK;
    }
    return -AG_ENOTSUP;
}

/*
 * WHAT A CLICK IS, COUNTED (27 Sep 2026).  Every counter the sound had was a
 * piece turned away, and Maxim heard clicks far more often than any of them
 * said: the converter running out between two pieces that both arrived.  The
 * converter has no word for that here, so its clock is kept instead - frames
 * given since a start against the time since it, at the stream's rate.  When
 * the time has run past the frames by more than DRY_SLACK_US the converter
 * played nothing for that long.  A pause of DRY_STOP_US or more is a sound
 * that ended, not a click.
 */
#define DRY_SLACK_US 2000
#define DRY_STOP_US  300000
static int64_t  s_dry_t0_us;
static uint64_t s_dry_frames;
static uint32_t s_dry_times, s_dry_max_us;
/*
 * AND WHEN (29 Sep 2026).  Maxim hears clicks at the start and none after;
 * a count cannot say which.  The first few are kept as milliseconds since
 * the first sound this board played, the top bit set for a pause of
 * DRY_STOP_US or more - which the count leaves out as a sound that ended,
 * and which at the start of one may be exactly what is heard.
 */
#define DRY_KEEP 8u
#define DRY_PAUSE_BIT 0x80000000u
static int64_t  s_dry_first_us;
static uint32_t s_dry_at[DRY_KEEP];
static uint32_t s_dry_at_n, s_dry_pauses;

static void count_dry(uint32_t frames)
{
    const int64_t now = ag_port_us();
    const uint32_t rate = (s_fmt.rate != 0u) ? s_fmt.rate : 22050u;
    if (s_dry_first_us == 0) {
        s_dry_first_us = now;
    }
    /*
     * What the converter holds ahead of what it is playing covers a late
     * piece: the S3's I2S chain is 960 frames, 43 ms at 22 kHz, and a clock
     * that counted from the first write called every 3 ms late a click
     * (27 Sep 2026).  Late by more than the chain is silence.
     */
    const int32_t chain = ag_port_audio_space();
    const int64_t slack = DRY_SLACK_US +
                          ((chain > 0) ? (int64_t)chain * 1000000 / rate : 0);
    if (s_dry_t0_us != 0) {
        const int64_t played_until =
            s_dry_t0_us + (int64_t)(s_dry_frames * 1000000u / rate);
        const int64_t late = now - played_until;
        if (late > slack) {
            if (late < DRY_STOP_US) {
                s_dry_times++;
                if ((uint32_t)late > s_dry_max_us) {
                    s_dry_max_us = (uint32_t)late;
                }
            } else {
                s_dry_pauses++;
            }
            if (s_dry_at_n < DRY_KEEP) {
                const uint32_t at =
                    (uint32_t)((now - s_dry_first_us) / 1000) & ~DRY_PAUSE_BIT;
                s_dry_at[s_dry_at_n++] =
                    at | ((late >= DRY_STOP_US) ? DRY_PAUSE_BIT : 0u);
            }
            s_dry_t0_us = 0; /* a new start */
        }
    }
    if (s_dry_t0_us == 0) {
        s_dry_t0_us = now;
        s_dry_frames = 0;
    }
    s_dry_frames += frames;
}

void ag_pcmhw_dry(uint32_t *times, uint32_t *longest_us)
{
    *times = s_dry_times;
    *longest_us = s_dry_max_us;
}

void ag_pcmhw_counts(uint32_t *frames_in, uint32_t *frames_out,
                     uint32_t *played_ms, uint32_t *rate)
{
    *frames_in = s_all_in;
    *frames_out = s_all_out;
    *played_ms = (uint32_t)(s_all_us / 1000u);
    *rate = s_fmt.rate;
}

uint32_t ag_pcmhw_peak(uint32_t *channels)
{
    const uint32_t p = s_peak;
    s_peak = 0;
    *channels = s_fmt.channels;
    return p;
}

uint32_t ag_pcmhw_dry_at(uint32_t *at_ms, uint32_t cap, uint32_t *pauses)
{
    const uint32_t n = (s_dry_at_n < cap) ? s_dry_at_n : cap;
    for (uint32_t i = 0; i < n; i++) {
        at_ms[i] = s_dry_at[i];
    }
    *pauses = s_dry_pauses;
    return n;
}

static int32_t pcm_write(ag_device_t *dev, const void *buf, size_t len,
                         uint64_t off)
{
    (void)dev;
    (void)off;

    if (buf == NULL) {
        return -AG_EINVAL;
    }

    const size_t frame_bytes = (size_t)s_fmt.channels * sizeof(int16_t);
    const size_t frames = len / frame_bytes;
    if (frames == 0u) {
        return -AG_EINVAL;
    }

    const ag_err_t err = hw_open();
    if (err != AG_OK) {
        return err;
    }

    const int32_t done = ag_port_audio_write((const int16_t *)buf,
                                             (int32_t)frames);
    if (done < 0) {
        return done;
    }
    s_frames_in += frames;
    s_frames_out += (uint64_t)done;
    s_all_in += (uint32_t)frames;
    s_all_out += (uint32_t)done;
    s_all_us += (uint64_t)done * 1000000u / s_fmt.rate;
    count_dry((uint32_t)done);
    /* the loudest sample given to the converter: proof there was a sound in
     * what it took, not just samples (2 Oct 2026, "the speaker hisses and
     * nothing plays") */
    {
        const int16_t *const pcm = (const int16_t *)buf;
        const size_t         n = frames * s_fmt.channels;
        for (size_t i = 0; i < n; i++) {
            const int32_t v = (pcm[i] < 0) ? -(int32_t)pcm[i] : pcm[i];
            if ((uint32_t)v > s_peak) {
                s_peak = (uint32_t)v;
            }
        }
    }

    /*
     * Bytes accepted, which is what a caller checks against what it asked for.
     * A short answer means the converter was still busy and the rest of that
     * buffer is gone: dropping a few milliseconds is better than holding up a
     * program that has a frame to draw.
     */
    return (int32_t)((size_t)done * frame_bytes);
}

static const ag_dev_ops_t k_pcm_ops = {
    .open = pcm_open,
    .close = pcm_close,
    .write = pcm_write,
    .ioctl = pcm_ioctl,
};

ag_err_t ag_pcmhw_init(void)
{
    /*
     * The board describes where the sound goes before the port is asked
     * whether there is any.  On a chip whose converters are on fixed pins this
     * changes nothing; on one that has none, it is the whole question - three
     * pins in BOARD.CFG are the difference between a machine with an output
     * and a machine without one, and neither the kernel nor the port is
     * allowed to guess which pins those are.
     */
    const ag_board_audio_t   *a = &ag_board()->audio;
    const ag_port_audio_pins_t pins = {
        .bclk = a->bclk,
        .ws = a->ws,
        .dout = a->dout,
        .mclk = a->mclk,
        .rate = a->rate,
    };
    ag_port_audio_pins(&pins);

    if (!ag_port_audio_present()) {
        return AG_OK; /* no output on this machine; pcmnull stands alone */
    }

    /*
     * The pins are deliberately not reserved.
     *
     * A reservation in this system is permanent - ag_io_release refuses one,
     * because "wait for it" and "you will never get this" have to be different
     * answers.  That is right for the console and the card, which are in use
     * from boot to power-off, and wrong for an output that is silent most of
     * the time: the board on this desk has its addressable LED on the same
     * wire as the word clock, and a pin reserved at boot would mean the light
     * could never be turned off again.  The honest state - taken while the
     * device is open, given back on close - does not exist yet, and inventing
     * it in passing here would be the wrong place to invent it.
     */

    default_fmt(&s_fmt);

    const ag_dev_desc_t desc = {
        .name = "pcm0",
        .driver = ag_port_audio_name(),
        .cls = AG_DEV_AUDIO,
        .flags = AG_DEVF_EXCLUSIVE | AG_DEVF_DMA,
        .ops = &k_pcm_ops,
        .priv = NULL,
    };
    const ag_err_t err = ag_dev_register(&desc, &s_dev);
    if (err != AG_OK) {
        return err;
    }
    ag_log(AG_LOG_INFO, TAG, "pcm0 ready (%s, %u Hz, opens on first write)",
           ag_port_audio_name(), (unsigned)s_fmt.rate);
    return AG_OK;
}
#else /* !CONFIG_ARGON_ENABLE_AUDIO */

ag_err_t ag_pcmhw_init(void)
{
    return AG_OK; /* nothing to register, and that is not a failure */
}

void ag_pcmhw_dry(uint32_t *times, uint32_t *longest_us)
{
    *times = 0;
    *longest_us = 0;
}

void ag_pcmhw_counts(uint32_t *frames_in, uint32_t *frames_out,
                     uint32_t *played_ms, uint32_t *rate)
{
    *frames_in = 0;
    *frames_out = 0;
    *played_ms = 0;
    *rate = 0;
}

uint32_t ag_pcmhw_peak(uint32_t *channels)
{
    *channels = 0;
    return 0;
}

uint32_t ag_pcmhw_dry_at(uint32_t *at_ms, uint32_t cap, uint32_t *pauses)
{
    (void)at_ms;
    (void)cap;
    *pauses = 0;
    return 0;
}

#endif

