/*
 * ArgonOS port: ESP-IDF - the sound this machine can actually make.
 *
 * Everything above this line deals in interleaved signed 16-bit frames at a
 * sample rate, because that is what a synthesiser or an emulator produces and
 * what /dev/pcm0 accepts.  What the machine underneath wants is its own
 * business, and the two machines this port runs on want different things.
 *
 * The original ESP32 has a pair of 8-bit digital-to-analogue converters on
 * fixed pins and a DMA path that feeds them from memory.  On the "cheap yellow
 * display" boards GPIO 26 goes to a small amplifier and a speaker, and that is
 * the only way that machine makes a sound.  Eight bits is about 48 dB of
 * range, which is a 1980s console or a chip synthesiser and not much else -
 * which is exactly what it is for.
 *
 * The ESP32-S3 has no converters at all.  Sound leaves it as I2S and becomes
 * analogue somewhere else: a codec, or an amplifier with a converter inside
 * it.  Which pins carry it is not a fact about the chip, so it arrives from
 * BOARD.CFG through ag_port_audio_pins, and present() answers no until the
 * board has said - a development board with nothing soldered to it keeps the
 * mute sink and comes up exactly as it did before.
 *
 * The conversion belongs here rather than in the kernel for the reason
 * argon/port/audio.h gives: a board with a codec should not have to undo what
 * a board with a converter needed.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/audio.h>

#include "sdkconfig.h"

#if CONFIG_ARGON_ENABLE_AUDIO

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

#define TAG "audio_hw"

/*
 * What the board said, or nothing at all.  Kept whether or not this chip can
 * use it: the call is part of the contract and arrives before present() on
 * every machine, so a port that has no use for it should discard it in one
 * obvious place rather than by not having the function.
 */
static ag_port_audio_pins_t s_pins = {
    .bclk = -1, .ws = -1, .dout = -1, .mclk = -1, .rate = 22050,
};

void ag_port_audio_pins(const ag_port_audio_pins_t *pins)
{
    if (pins != NULL) {
        s_pins = *pins;
    }
}

#if SOC_DAC_SUPPORTED

#include "driver/dac_continuous.h"

const char *ag_port_audio_name(void) { return "dac"; }

/* Four descriptors of half a kilobyte: about ninety milliseconds at 22 kHz,
 * which is the whole latency budget and about four kilobytes of DMA buffers. */
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

#elif SOC_I2S_SUPPORTED

#include "driver/i2s_std.h"

/*
 * Four descriptors of 240 frames is about forty milliseconds at 22 kHz: long
 * enough that a task which stops to read a file does not empty the chain,
 * short enough that a note which has been let go is not still playing a fifth
 * of a second later.  It is about two kilobytes, and only while something is
 * playing.
 *
 * auto_clear matters more than it looks.  A starved chain repeats whatever was
 * left in it, and a repeated buffer is not a gap in the sound - it is a loud
 * buzz at that buffer's own frequency, which is the first thing anyone hears
 * when an application falls behind.  The price is a memset per descriptor.
 */
#define I2S_DESCS   4
#define I2S_FRAMES  240
#define I2S_WAIT_MS 120

static i2s_chan_handle_t s_tx;
static uint8_t           s_channels = 2;

const char *ag_port_audio_name(void) { return "i2s"; }

/*
 * Not "does this chip have I2S" - every S3 does - but "has anyone said where
 * the wires go".  A board with nothing soldered to it must come up silent
 * rather than clocking three arbitrary pins, and the only place that answer
 * can come from is BOARD.CFG.
 */
bool ag_port_audio_present(void)
{
    return s_pins.bclk >= 0 && s_pins.ws >= 0 && s_pins.dout >= 0;
}

bool ag_port_audio_open(uint32_t rate, uint8_t channels)
{
    if (s_tx != NULL) {
        return true;
    }
    if (!ag_port_audio_present()) {
        return false;
    }
    if (channels != 1u && channels != 2u) {
        return false;
    }

    i2s_chan_config_t chan =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan.dma_desc_num = I2S_DESCS;
    chan.dma_frame_num = I2S_FRAMES;
    chan.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s channel: %s", esp_err_to_name(err));
        s_tx = NULL;
        return false;
    }

    const i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            (channels == 1u) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (s_pins.mclk >= 0) ? (gpio_num_t)s_pins.mclk
                                       : I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)s_pins.bclk,
            .ws = (gpio_num_t)s_pins.ws,
            .dout = (gpio_num_t)s_pins.dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false,
                              .bclk_inv = false,
                              .ws_inv = false },
        },
    };

    err = i2s_channel_init_std_mode(s_tx, &std);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_tx);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s start: %s", esp_err_to_name(err));
        (void)i2s_del_channel(s_tx);
        s_tx = NULL;
        return false;
    }

    s_channels = channels;
    ESP_LOGI(TAG, "i2s out on bclk=%d ws=%d dout=%d, %u Hz, %u ch",
             (int)s_pins.bclk, (int)s_pins.ws, (int)s_pins.dout,
             (unsigned)rate, (unsigned)channels);
    return true;
}

void ag_port_audio_close(void)
{
    if (s_tx == NULL) {
        return;
    }
    /*
     * Nothing is walked down to silence the way the converter's output is.
     * What leaves this pin is a number, and the thing at the other end gets
     * zeros from auto_clear until the clock stops; the thump the other port
     * takes twenty milliseconds to avoid belongs to a pin that idles at half
     * the supply, and this one does not.
     */
    (void)i2s_channel_disable(s_tx);
    (void)i2s_del_channel(s_tx);
    s_tx = NULL;
}

int32_t ag_port_audio_write(const int16_t *pcm, int32_t frames)
{
    if (s_tx == NULL || pcm == NULL || frames <= 0) {
        return 0;
    }

    const size_t    frame_bytes = (size_t)s_channels * sizeof(int16_t);
    size_t          wrote = 0;
    const esp_err_t err = i2s_channel_write(
        s_tx, pcm, (size_t)frames * frame_bytes, &wrote, I2S_WAIT_MS);

    if (err != ESP_OK && wrote == 0) {
        return 0;
    }
    return (int32_t)(wrote / frame_bytes);
}

/*
 * A hint, as the contract says, and here it is the whole chain rather than
 * what is free in it: the driver does not offer that number, and keeping a
 * count of it in this layer would be keeping it wrong.  The caller uses it to
 * size a ring, and a ring the size of the chain is the right size.
 */
int32_t ag_port_audio_space(void)
{
    return (s_tx != NULL) ? (int32_t)(I2S_DESCS * I2S_FRAMES) : 0;
}

#else /* a part with neither converters nor I2S */

const char *ag_port_audio_name(void) { return "none"; }

bool ag_port_audio_present(void) { return false; }

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

int32_t ag_port_audio_space(void) { return 0; }

#endif /* SOC_DAC_SUPPORTED / SOC_I2S_SUPPORTED */

#else /* !CONFIG_ARGON_ENABLE_AUDIO */

void ag_port_audio_pins(const ag_port_audio_pins_t *pins) { (void)pins; }

const char *ag_port_audio_name(void) { return "none"; }

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
