/*
 * ArgonOS port: ESP-IDF - the original ESP32's own digital-to-analogue output.
 *
 * The chip has two 8-bit converters wired to fixed pins (GPIO 25 and 26) and a
 * DMA path that feeds them from memory.  On the "cheap yellow display" boards
 * GPIO 26 goes to a small amplifier and a speaker, which is the only way this
 * machine makes a sound - it has no I2S codec and no USB.
 *
 * Eight bits and one channel is what comes out, and the conversion from the
 * system's signed 16-bit stereo happens here for the reason argon/port/audio.h
 * gives: a board with a codec should not have to undo it.  Eight bits is about
 * 48 dB of range, which is a 1980s console or a chip synthesiser and not much
 * else - which is exactly what this is for.
 *
 * The ESP32-S3 has no converters at all; there present() says so and the
 * system keeps its mute sink.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/audio.h>

#include "sdkconfig.h"

#if CONFIG_ARGON_ENABLE_AUDIO

#include <string.h>

#include "driver/dac_continuous.h"
#include "esp_err.h"
#include "esp_log.h"

#define TAG "audio_hw"

/*
 * Four buffers of half a kilobyte: two thousand samples, which at 22 kHz is
 * ninety milliseconds of sound.  That is the whole latency budget and it is
 * also the whole memory cost - about four kilobytes, and only while something
 * is playing.  Fewer or smaller and the DMA runs dry between writes on a board
 * that is also driving a display over SPI.
 */
#define DAC_DESCS    4
#define DAC_BUF      512
#define DAC_FRAMES   (DAC_DESCS * DAC_BUF)

/* How long write() may wait for the converter to drain: one buffer's worth. */
#define DAC_WAIT_MS  120

/*
 * One whole DMA chain's worth of converted bytes, filled before any of it is
 * handed over.
 *
 * Not an optimisation: the driver underneath takes a descriptor per call
 * regardless of how little is in it, and feeding it a hundred and twenty eight
 * bytes at a time starves the chain within four calls.  Filling two kilobytes
 * first means one call per ninety milliseconds of sound and a chain that is
 * always loaded to the top.
 */
#define ACC_BYTES (DAC_DESCS * DAC_BUF)

static dac_continuous_handle_t s_dac;
static uint8_t                 s_channels = 2;
static uint8_t                 s_acc[ACC_BYTES];
static size_t                  s_acc_used;

/* Hands the buffer over, in as few calls as the driver will take. */
static bool acc_flush(void)
{
    size_t off = 0;

    while (off < s_acc_used) {
        size_t          loaded = 0;
        const esp_err_t err = dac_continuous_write(
            s_dac, s_acc + off, s_acc_used - off, &loaded, DAC_WAIT_MS);
        off += loaded;
        if (err != ESP_OK) {
            break; /* the converter is still full: the rest is dropped */
        }
    }
    const bool all = (off == s_acc_used);
    s_acc_used = 0;
    return all;
}

bool ag_port_audio_present(void)
{
    return true;
}

bool ag_port_audio_open(uint32_t rate, uint8_t channels)
{
    if (s_dac != NULL) {
        return true;
    }
    if (channels != 1u && channels != 2u) {
        return false;
    }
    /*
     * The digital controller's default clock will not divide below about
     * 19.6 kHz on this part, so a slower rate is not refused - it is raised.
     * A sound played slightly fast is better than no sound, and the rates that
     * reach here are 22050 and up in practice.
     */
    if (rate < 20000u) {
        rate = 20000u;
    }

    const dac_continuous_config_t cfg = {
        /* Channel 1 is GPIO 26: the pin the amplifier is on. */
        .chan_mask = DAC_CHANNEL_MASK_CH1,
        .desc_num = DAC_DESCS,
        .buf_size = DAC_BUF,
        .freq_hz = rate,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_DEFAULT,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };

    esp_err_t err = dac_continuous_new_channels(&cfg, &s_dac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dac channels: %s", esp_err_to_name(err));
        s_dac = NULL;
        return false;
    }
    err = dac_continuous_enable(s_dac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dac enable: %s", esp_err_to_name(err));
        (void)dac_continuous_del_channels(s_dac);
        s_dac = NULL;
        return false;
    }

    s_channels = channels;
    s_acc_used = 0;
    ESP_LOGI(TAG, "dac out on gpio26, %u Hz, %u ch in", (unsigned)rate,
             (unsigned)channels);
    return true;
}

void ag_port_audio_close(void)
{
    if (s_dac == NULL) {
        return;
    }
    (void)acc_flush();

    /*
     * Silence is 128, not 0: the converter swings from ground to the supply
     * and the middle of that is what the amplifier's input is coupled around.
     * Stopping at 128 and then cutting the peripheral drops the pin to ground
     * in one step, which is a thump in the speaker - so walk it down first,
     * over about twenty milliseconds, which is below what a speaker this size
     * will reproduce.
     */
    for (int level = 128; level > 0; level -= 4) {
        memset(s_acc + s_acc_used, (uint8_t)level, 16u);
        s_acc_used += 16u;
    }
    (void)acc_flush();

    (void)dac_continuous_disable(s_dac);
    (void)dac_continuous_del_channels(s_dac);
    s_dac = NULL;
}

int32_t ag_port_audio_write(const int16_t *pcm, int32_t frames)
{
    if (s_dac == NULL || pcm == NULL || frames <= 0) {
        return 0;
    }

    int32_t done = 0;
    while (done < frames) {
        int32_t n = frames - done;
        const int32_t room = (int32_t)(ACC_BYTES - s_acc_used);
        if (n > room) {
            n = room;
        }

        /*
         * Signed 16-bit to unsigned 8-bit is the top byte with the sign
         * flipped.  Two channels become one by averaging rather than by
         * dropping the right: an effect that lives in one channel would
         * otherwise vanish, and this speaker has no stereo to lose.
         */
        uint8_t *out = s_acc + s_acc_used;
        if (s_channels == 2u) {
            const int16_t *src = pcm + (size_t)done * 2u;
            for (int32_t i = 0; i < n; i++) {
                const int32_t mono = ((int32_t)src[i * 2] + src[i * 2 + 1]) / 2;
                out[i] = (uint8_t)((mono + 32768) >> 8);
            }
        } else {
            const int16_t *src = pcm + done;
            for (int32_t i = 0; i < n; i++) {
                out[i] = (uint8_t)(((int32_t)src[i] + 32768) >> 8);
            }
        }
        s_acc_used += (size_t)n;
        done += n;

        if (s_acc_used >= ACC_BYTES && !acc_flush()) {
            break; /* the converter would not take it: say what was played */
        }
    }
    return done;
}

int32_t ag_port_audio_space(void)
{
    return s_dac != NULL ? (int32_t)(ACC_BYTES - s_acc_used) : 0;
}

#else /* !CONFIG_ARGON_ENABLE_AUDIO */

bool ag_port_audio_present(void)
{
    return false;
}

bool ag_port_audio_open(uint32_t rate, uint8_t channels)
{
    (void)rate;
    (void)channels;
    return false;
}

void ag_port_audio_close(void) {}

int32_t ag_port_audio_write(const int16_t *pcm, int32_t frames)
{
    (void)pcm;
    (void)frames;
    return 0;
}

int32_t ag_port_audio_space(void)
{
    return 0;
}

#endif /* CONFIG_ARGON_ENABLE_AUDIO */
