/*
 * ArgonOS - built-in mute PCM (/dev/pcmnull) and mixer (/dev/pcmmix).
 *
 * Real output (virt TCP, I2S, …) is published by loadable .SYS drivers.
 * api->audio is a convenience alias for the null sink.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "argon/audio.h"

#include <string.h>

#include "argon/board.h"
#include "argon/device.h"
#include "argon/log.h"
#include "argon/proc.h"

#define TAG "audio"

static ag_device_t    *s_dev;
static int            s_api_held; /* api->audio holds exclusive open */
static int            s_fmt_set;
static ag_audio_fmt_t s_fmt;
static ag_pid_t       s_owner = AG_PID_KERNEL;

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

static ag_err_t apply_fmt(const ag_audio_fmt_t *fmt)
{
    ag_audio_fmt_t use;
    if (fmt != NULL) {
        use = *fmt;
    } else {
        default_fmt(&use);
    }
    if (!fmt_ok(&use)) {
        return -AG_EINVAL;
    }
    s_fmt = use;
    s_fmt_set = 1;
    return AG_OK;
}

static ag_err_t pcm_open(ag_device_t *dev, uint32_t flags)
{
    (void)dev;
    (void)flags;
    if (!s_fmt_set) {
        (void)apply_fmt(NULL);
    }
    s_owner = ag_proc_self();
    return AG_OK;
}

static ag_err_t pcm_close(ag_device_t *dev)
{
    (void)dev;
    if (s_api_held) {
        s_api_held = 0;
    }
    s_owner = AG_PID_KERNEL;
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
        if (!s_fmt_set) {
            (void)apply_fmt(NULL);
        }
        *(ag_audio_fmt_t *)arg = s_fmt;
        return AG_OK;
    }
    if (cmd == AG_IOC_AUDIO_SETFMT) {
        if (arg == NULL || arglen < sizeof(ag_audio_fmt_t)) {
            return -AG_EINVAL;
        }
        return apply_fmt((const ag_audio_fmt_t *)arg);
    }
    if (cmd == AG_IOC_AUDIO_GETSTATS) {
        if (arg == NULL || arglen < sizeof(ag_audio_stats_t)) {
            return -AG_EINVAL;
        }
        memset(arg, 0, sizeof(ag_audio_stats_t));
        return AG_OK;
    }
    if (cmd == AG_IOC_FLUSH || cmd == AG_IOC_RESET) {
        return AG_OK;
    }
    return -AG_ENOTSUP;
}

static int32_t pcm_write(ag_device_t *dev, const void *buf, size_t len,
                         uint64_t off)
{
    (void)dev;
    (void)off;
    if (buf == NULL || len < 2u) {
        return -AG_EINVAL;
    }
    if (!s_fmt_set) {
        (void)apply_fmt(NULL);
    }
    {
        const size_t frame_bytes =
            (size_t)s_fmt.channels * sizeof(int16_t);
        const size_t frames = len / frame_bytes;
        if (frames == 0u) {
            return -AG_EINVAL;
        }
        /* Mute: accept and discard. */
        return (int32_t)(frames * frame_bytes);
    }
}

static const ag_dev_ops_t k_pcm_ops = {
    .open = pcm_open,
    .close = pcm_close,
    .write = pcm_write,
    .ioctl = pcm_ioctl,
};

static int audio_present(void)
{
    return s_dev != NULL ? 1 : 0;
}

static int audio_is_hw(void)
{
    return 0;
}

static ag_err_t audio_open(const ag_audio_fmt_t *fmt)
{
    ag_err_t err;

    if (s_dev == NULL) {
        return -AG_ENODEV;
    }
    if (s_api_held) {
        return -AG_EBUSY;
    }
    err = apply_fmt(fmt);
    if (err != AG_OK) {
        return err;
    }
    err = ag_dev_open(s_dev, AG_O_WRONLY);
    if (err != AG_OK) {
        return err;
    }
    s_api_held = 1;
    ag_log(AG_LOG_INFO, TAG, "pcmnull open %u Hz %u ch (discard)",
           (unsigned)s_fmt.rate, (unsigned)s_fmt.channels);
    return AG_OK;
}

static void audio_close(void)
{
    if (!s_api_held || s_dev == NULL) {
        return;
    }
    s_api_held = 0;
    (void)ag_dev_close(s_dev);
}

static int32_t audio_write_pcm(const int16_t *pcm, int32_t frames)
{
    size_t  bytes;
    int32_t n;

    if (!s_api_held || pcm == NULL || frames <= 0 || !s_fmt_set) {
        return -AG_EINVAL;
    }
    bytes = (size_t)frames * (size_t)s_fmt.channels * sizeof(int16_t);
    n = pcm_write(s_dev, pcm, bytes, 0);
    if (n < 0) {
        return n;
    }
    return (int32_t)((size_t)n / (sizeof(int16_t) * (size_t)s_fmt.channels));
}

static int32_t audio_space(void)
{
    return s_api_held ? 4096 : 0;
}

const ag_audio_api_t ag_audio_api_table = {
    .size = sizeof(ag_audio_api_t),
    .present = audio_present,
    .is_hw = audio_is_hw,
    .open = audio_open,
    .close = audio_close,
    .write = audio_write_pcm,
    .space = audio_space,
};

int ag_audio_opened(void)
{
    return s_api_held;
}

ag_err_t ag_audio_init(void)
{
    const ag_dev_desc_t desc = {
        .name = "pcmnull",
        .driver = "null",
        .cls = AG_DEV_AUDIO,
        .flags = AG_DEVF_EXCLUSIVE | AG_DEVF_DMA,
        .ops = &k_pcm_ops,
        .priv = NULL,
    };
    const ag_err_t err = ag_dev_register(&desc, &s_dev);
    if (err != AG_OK) {
        return err;
    }
    (void)apply_fmt(NULL);
    ag_log(AG_LOG_INFO, TAG, "pcmnull ready (mute; load .SYS for virt/I2S)");
    {
        /*
         * The real output first, so the mixer finds it: pcmmix picks its sink
         * when it starts and pcm0 has to exist by then.
         */
        const ag_err_t hw_err = ag_pcmhw_init();
        if (hw_err != AG_OK) {
            return hw_err;
        }
        const ag_err_t mix_err = ag_pcmmix_init();
        if (mix_err != AG_OK) {
            return mix_err;
        }
    }
    return AG_OK;
}
