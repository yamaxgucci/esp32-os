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

static ag_err_t hw_open(void)
{
    if (s_hw_open) {
        return AG_OK;
    }
    if (!ag_port_audio_open(s_fmt.rate, s_fmt.channels)) {
        return -AG_EIO;
    }
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
    if (!ag_port_audio_present()) {
        return AG_OK; /* no output on this machine; pcmnull stands alone */
    }

    default_fmt(&s_fmt);

    const ag_dev_desc_t desc = {
        .name = "pcm0",
        .driver = "dac",
        .cls = AG_DEV_AUDIO,
        .flags = AG_DEVF_EXCLUSIVE | AG_DEVF_DMA,
        .ops = &k_pcm_ops,
        .priv = NULL,
    };
    const ag_err_t err = ag_dev_register(&desc, &s_dev);
    if (err != AG_OK) {
        return err;
    }
    ag_log(AG_LOG_INFO, TAG, "pcm0 ready (%u Hz, opens on first write)",
           (unsigned)s_fmt.rate);
    return AG_OK;
}
#else /* !CONFIG_ARGON_ENABLE_AUDIO */

ag_err_t ag_pcmhw_init(void)
{
    return AG_OK; /* nothing to register, and that is not a failure */
}

#endif

