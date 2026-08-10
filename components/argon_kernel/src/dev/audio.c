/*
 * ArgonOS - PCM audio: I2S std TX when BOARD.CFG pins are set, else a stub that
 * accepts and discards samples (QEMU / unwired boards).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "argon/audio.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"

#include "argon/board.h"
#include "argon/device.h"
#include "argon/log.h"
#include "argon/proc.h"

#define TAG "audio"

static ag_device_t       *s_dev;
static i2s_chan_handle_t s_tx;
static int               s_hw;
static int               s_open;
static ag_audio_fmt_t    s_fmt;
static ag_pid_t          s_owner = AG_PID_KERNEL;

static int pins_ready(const ag_board_audio_t *a)
{
    return a->bclk != AG_PIN_NONE && a->ws != AG_PIN_NONE &&
           a->dout != AG_PIN_NONE;
}

static int want_hw(const ag_board_audio_t *a)
{
    if (strcmp(a->driver, "stub") == 0) {
        return 0;
    }
    if (strcmp(a->driver, "i2s") == 0) {
        return pins_ready(a);
    }
    return pins_ready(a);
}

static void i2s_teardown(void)
{
    if (s_tx != NULL) {
        (void)i2s_channel_disable(s_tx);
        (void)i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    s_hw = 0;
}

static ag_err_t i2s_setup(const ag_board_audio_t *a, const ag_audio_fmt_t *fmt)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO,
                                                           I2S_ROLE_MASTER);
    i2s_std_config_t  std_cfg;
    esp_err_t         err;

    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;

    err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ag_log(AG_LOG_ERROR, TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return -AG_EIO;
    }

    memset(&std_cfg, 0, sizeof(std_cfg));
    std_cfg.clk_cfg = (i2s_std_clk_config_t)I2S_STD_CLK_DEFAULT_CONFIG(fmt->rate);
    if (fmt->channels <= 1u) {
        std_cfg.slot_cfg =
            (i2s_std_slot_config_t)I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    } else {
        std_cfg.slot_cfg =
            (i2s_std_slot_config_t)I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    }
    std_cfg.gpio_cfg.mclk =
        (a->mclk != AG_PIN_NONE) ? (gpio_num_t)a->mclk : I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = (gpio_num_t)a->bclk;
    std_cfg.gpio_cfg.ws = (gpio_num_t)a->ws;
    std_cfg.gpio_cfg.dout = (gpio_num_t)a->dout;
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;

    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ag_log(AG_LOG_ERROR, TAG, "i2s std init: %s", esp_err_to_name(err));
        i2s_teardown();
        return -AG_EIO;
    }
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ag_log(AG_LOG_ERROR, TAG, "i2s enable: %s", esp_err_to_name(err));
        i2s_teardown();
        return -AG_EIO;
    }
    s_hw = 1;
    return AG_OK;
}

static int audio_present(void)
{
    return 1;
}

static int audio_is_hw(void)
{
    return s_hw && s_open;
}

static ag_err_t audio_open(const ag_audio_fmt_t *fmt)
{
    const ag_board_audio_t *a = &ag_board()->audio;
    ag_audio_fmt_t          use;

    if (s_open) {
        return -AG_EBUSY;
    }
    if (fmt != NULL) {
        use = *fmt;
    } else {
        use.rate = a->rate ? a->rate : 22050u;
        use.channels = 2;
        use.bits = 16;
    }
    if (use.rate < 8000u || use.rate > 48000u || use.bits != 16u ||
        (use.channels != 1u && use.channels != 2u)) {
        return -AG_EINVAL;
    }

    i2s_teardown();
    if (want_hw(a)) {
        const ag_err_t err = i2s_setup(a, &use);
        if (err != AG_OK) {
            return err;
        }
        ag_log(AG_LOG_INFO, TAG, "I2S TX %u Hz %u ch (bclk=%d ws=%d dout=%d)",
               (unsigned)use.rate, (unsigned)use.channels, (int)a->bclk,
               (int)a->ws, (int)a->dout);
    } else {
        ag_log(AG_LOG_INFO, TAG, "audio stub (no I2S pins) %u Hz %u ch",
               (unsigned)use.rate, (unsigned)use.channels);
    }

    s_fmt = use;
    s_open = 1;
    s_owner = ag_proc_self();
    return AG_OK;
}

static void audio_close(void)
{
    if (!s_open) {
        return;
    }
    i2s_teardown();
    s_open = 0;
    s_owner = AG_PID_KERNEL;
}

static int32_t audio_write_pcm(const int16_t *pcm, int32_t frames)
{
    size_t    bytes;
    size_t    written = 0;
    esp_err_t err;

    if (!s_open || pcm == NULL || frames <= 0) {
        return -AG_EINVAL;
    }
    if (!s_hw) {
        return frames;
    }
    bytes = (size_t)frames * (size_t)s_fmt.channels * sizeof(int16_t);
    err = i2s_channel_write(s_tx, pcm, bytes, &written, pdMS_TO_TICKS(8));
    if (err == ESP_OK || err == ESP_ERR_TIMEOUT) {
        return (int32_t)(written / (sizeof(int16_t) * (size_t)s_fmt.channels));
    }
    return -AG_EIO;
}

static int32_t audio_space(void)
{
    if (!s_open) {
        return 0;
    }
    return s_hw ? 512 : 4096;
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

static int32_t pcm_dev_write(ag_device_t *dev, const void *buf, size_t len,
                             uint64_t off)
{
    int32_t frames;
    int32_t n;
    (void)dev;
    (void)off;
    if (buf == NULL || len < 2u) {
        return -AG_EINVAL;
    }
    if (!s_open) {
        const ag_err_t err = audio_open(NULL);
        if (err != AG_OK) {
            return err;
        }
    }
    frames = (int32_t)(len / (sizeof(int16_t) * (size_t)s_fmt.channels));
    if (frames <= 0) {
        return -AG_EINVAL;
    }
    n = audio_write_pcm((const int16_t *)buf, frames);
    if (n < 0) {
        return n;
    }
    return (int32_t)((size_t)n * sizeof(int16_t) * (size_t)s_fmt.channels);
}

static const ag_dev_ops_t k_pcm_ops = {
    .write = pcm_dev_write,
};

int ag_audio_opened(void)
{
    return s_open;
}

ag_err_t ag_audio_init(void)
{
    const char *drv = want_hw(&ag_board()->audio) ? "i2s" : "stub";
    const ag_dev_desc_t desc = {
        .name = "pcm0",
        .driver = drv,
        .cls = AG_DEV_AUDIO,
        .flags = AG_DEVF_EXCLUSIVE | AG_DEVF_DMA,
        .ops = &k_pcm_ops,
        .priv = NULL,
    };
    const ag_err_t err = ag_dev_register(&desc, &s_dev);
    if (err != AG_OK) {
        return err;
    }
    ag_log(AG_LOG_INFO, TAG, "pcm0 ready (%s)", drv);
    return AG_OK;
}
