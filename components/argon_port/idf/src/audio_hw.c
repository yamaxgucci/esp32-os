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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

const char *ag_port_audio_name(void) { return "dac"; }

/*
 * Eight descriptors of a quarter kilobyte: still about ninety milliseconds at
 * 22 kHz, which is the whole latency budget, but each one is 11.6 ms instead of
 * 23.
 *
 * The length of one descriptor is not a detail - it is the shortest a write can
 * possibly wait.  A descriptor becomes free to load only once it has been
 * played, so at the moment the converter starts, *every* descriptor is busy and
 * the first write has to wait a whole one.  With half-kilobyte descriptors that
 * is 23 ms, and a poke shorter than that fails every time with
 * `dac_continuous: Get available descriptor timeout` - which is exactly how the
 * first version of the short-timeout flush below silenced `beep`.
 */
#define DAC_DESCS    8
#define DAC_BUF      256
#define DAC_FRAMES   (DAC_DESCS * DAC_BUF)

/*
 * How long a write may wait for the converter to drain.
 *
 * DAC_WAIT_MS is for closing, where waiting is the point: the walk down to
 * silence has to actually reach the pin before the peripheral stops.
 *
 * DAC_POKE_MS is for the ordinary path, and it is short because a write to a
 * device is made with the kernel's device registry held.  Waiting here does
 * not only slow the caller down, it stops every other device the caller's
 * machine has - which on the CYD is the display, being fed pixels down a UART
 * with no flow control.  See ag_port_audio_write.
 *
 * It cannot be *very* short, and two milliseconds was a mistake: a descriptor
 * is 11.6 ms of sound and does not come free before it has been played, so a
 * poke shorter than one descriptor fails every time and the sound never
 * starts.  Twenty is one descriptor and a margin.
 */
#define DAC_WAIT_MS  120
#define DAC_POKE_MS  20

/*
 * OUT BY THE CONVERTER'S OWN CLOCK, FROM A RING (2 Oct 2026).  Written in
 * calls, the driver gave each call's last bytes two descriptors half full and
 * stopped its DMA whenever the chain ran to its end: one big write every 46
 * ms stopped it every 46 ms, small writes stopped it after each - Maxim heard
 * the S3's tone through the CYD "with frequent clicks", then its own beep
 * choppy.  Now the DMA goes round its descriptors for as long as the device
 * is open, and each one it has played is filled again in its interrupt from
 * this ring - with what is there, and when nothing is, with the last sample
 * held: no stop, no restart.  And after running out it plays again only with
 * DAC_PRIME waiting, so sound that comes at a neighbour's clock, a little
 * slower than this one, has a cushion instead of a click a period.
 */
#define DAC_RING  3072u            /* samples as played: 70 ms at 44.1 kHz */
#define DAC_PRIME 1536u            /* 35 ms */

static dac_continuous_handle_t s_dac;
/* playing: enabled and going round (s_dac alone is only "made") */
static bool                    s_dac_on;
/* the rate s_dac was made at, as played (after s_up) */
static uint32_t                s_dac_hz;
static uint8_t                 s_channels = 2;
/* each input sample is given this many times (see ag_port_audio_open) */
static uint8_t                 s_up = 1;
static uint8_t                 s_ring[DAC_RING];
static volatile uint32_t       s_w, s_r; /* samples in, samples played */
static volatile bool           s_priming = true;
static volatile uint8_t        s_last = 128;
/* the ring ran out while playing: a gap, counted (2 Oct 2026) */
static volatile uint32_t       s_dac_stops;

uint32_t ag_port_audio_stops(void)
{
    return s_dac_stops;
}

static bool IRAM_ATTR on_dac_done(dac_continuous_handle_t h,
                                  const dac_event_data_t *e, void *arg)
{
    (void)arg;
    uint8_t        piece[DAC_BUF];
    const uint32_t want =
        (e->buf_size / 2u < DAC_BUF) ? e->buf_size / 2u : DAC_BUF;
    const uint32_t have = s_w - s_r;
    uint32_t       n = 0;
    if (s_priming && have >= DAC_PRIME) {
        s_priming = false;
    }
    if (!s_priming) {
        n = (have < want) ? have : want;
        for (uint32_t i = 0; i < n; i++) {
            piece[i] = s_ring[(s_r + i) % DAC_RING];
        }
        s_r += n;
        if (n > 0u) {
            s_last = piece[n - 1u];
        }
        if (n < want) {
            s_dac_stops++;
            s_priming = true;
        }
    }
    for (uint32_t i = n; i < want; i++) {
        piece[i] = s_last; /* held, not dropped to the rail */
    }
    (void)dac_continuous_write_asynchronously(h, e->buf, e->buf_size, piece,
                                              want, NULL);
    return false;
}

/*
 * THE CONVERTER'S MEMORY, KEPT FOR IT FROM THE START (2 Oct 2026).  It opens
 * on the first sound, and by then the CYD had 1 KB of byte memory left - the
 * neighbour's picture and the link had the rest - so its DMA buffers could
 * not be had: "failed to allocate dma buffer", sound arriving and nothing
 * played.  So a block the size of what the driver takes is held from the
 * first question about the converter until the driver asks for its buffers.
 *
 * AND NOT GIVEN BACK AFTER THAT (2 Oct 2026).  Held again at every close, the
 * block came back somewhere else: the heap's best fit for 3 KB was the CYD's
 * last 5 KB piece, not the hole the driver had left, and one `beep` took the
 * largest block of byte memory from 5 KB to 2 KB.  The neighbour's next
 * request then had no 4 KB for the task it is served in - its open of
 * cyd.pcm0 answered -12, the S3's mixer had "nowhere to send sound", and the
 * CYD played nothing more of it.  So the driver keeps its channels from the
 * first open on, stopped while the device is closed, and the memory never
 * moves; only a different rate makes them anew - and 22 050 and 44 100, the
 * two that reach here, are both played at 44 100 (see the open).
 */
#define DAC_RESERVE (DAC_DESCS * DAC_BUF + 1024u)
static void *s_reserve;

static void dac_reserve(void)
{
    if (s_reserve == NULL && s_dac == NULL) {
        s_reserve = heap_caps_malloc(DAC_RESERVE,
                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
}

bool ag_port_audio_present(void)
{
    dac_reserve(); /* asked once at boot, before anything else holds it */
    return true;
}

static void dac_drop(void)
{
    if (s_dac != NULL) {
        (void)dac_continuous_del_channels(s_dac);
        s_dac = NULL;
    }
    dac_reserve();
}

bool ag_port_audio_open(uint32_t rate, uint8_t channels)
{
    if (s_dac_on) {
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
    /*
     * And below 32 kHz the converter is run at twice the rate, each sample
     * given twice.  Near the bottom of what the divider reaches the rate it
     * gives is not the rate asked for: at 22 050 this DAC played about 16%
     * slow, measured as a stream from the C3 that the CYD could only keep up
     * with by throwing a sixth of it away (25 Sep 2026), where at 44 100 a
     * sixty-second tone takes sixty seconds.  A sample held twice is the same
     * sound to an 8-bit converter behind a small speaker.
     */
    s_up = (rate < 32000u) ? 2u : 1u;
    rate *= s_up;

    if (s_dac != NULL && s_dac_hz != rate) {
        /* another clock: the one thing that needs the channels made anew */
        (void)dac_continuous_del_channels(s_dac);
        s_dac = NULL;
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

    esp_err_t err = ESP_OK;
    if (s_dac == NULL) {
        if (s_reserve != NULL) {
            heap_caps_free(s_reserve); /* for the driver's own buffers, now */
            s_reserve = NULL;
        }
        err = dac_continuous_new_channels(&cfg, &s_dac);
        if (err == ESP_OK) {
            const dac_event_callbacks_t cbs = {.on_convert_done = on_dac_done,
                                               .on_stop = NULL};
            err = dac_continuous_register_event_callback(s_dac, &cbs, NULL);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "dac: %s", esp_err_to_name(err));
            dac_drop();
            return false;
        }
        s_dac_hz = rate;
    }

    s_w = s_r = 0;
    s_priming = true;
    s_last = 128;
    err = dac_continuous_enable(s_dac);
    if (err == ESP_OK) {
        err = dac_continuous_start_async_writing(s_dac);
        if (err != ESP_OK) {
            (void)dac_continuous_disable(s_dac);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dac: %s", esp_err_to_name(err));
        dac_drop();
        return false;
    }
    s_dac_on = true;
    s_channels = channels;
    ESP_LOGI(TAG, "dac out on gpio26, %u Hz, %u ch in", (unsigned)rate,
             (unsigned)channels);
    return true;
}

/* What is waiting is played out, for at most `ms`. */
static void ring_drain(uint32_t ms)
{
    for (uint32_t t = 0; t < ms && s_w != s_r && !s_priming; t += 2u) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static void ring_put(uint8_t v)
{
    s_ring[s_w % DAC_RING] = v;
    s_w++;
}

void ag_port_audio_close(void)
{
    if (!s_dac_on) {
        return;
    }
    ring_drain(DAC_WAIT_MS);
    /*
     * Silence is 128, not 0: the converter swings from ground to the supply
     * and the middle of that is what the amplifier's input is coupled around.
     * Cutting the peripheral drops the pin to ground in one step, which is a
     * thump in the speaker - so walk it down first, over about twenty
     * milliseconds, which is below what a speaker this size will reproduce.
     */
    s_priming = false;
    for (int level = s_last; level > 0; level -= 1) {
        for (int k = 0; k < 3 && s_w - s_r < DAC_RING; k++) {
            ring_put((uint8_t)level);
        }
    }
    ring_drain(DAC_WAIT_MS);
    (void)dac_continuous_stop_async_writing(s_dac);
    (void)dac_continuous_disable(s_dac);
    s_dac_on = false; /* the channels and their memory stay, for the next */
}

int32_t ag_port_audio_write(const int16_t *pcm, int32_t frames)
{
    if (!s_dac_on || pcm == NULL || frames <= 0) {
        return 0;
    }
    int32_t  done = 0;
    uint32_t waited = 0;
    while (done < frames) {
        const uint32_t room = DAC_RING - (s_w - s_r);
        if (room < s_up) {
            /* a full ring paces the writer, as a full chain did - briefly,
             * because a device write holds the registry (DAC_POKE_MS) */
            if (waited >= DAC_POKE_MS) {
                break;
            }
            vTaskDelay(1);
            waited += portTICK_PERIOD_MS;
            continue;
        }
        int32_t n = frames - done;
        if ((uint32_t)n > room / s_up) {
            n = (int32_t)(room / s_up);
        }
        /*
         * Signed 16-bit to unsigned 8-bit is the top byte with the sign
         * flipped.  Two channels become one by averaging rather than by
         * dropping the right: an effect that lives in one channel would
         * otherwise vanish, and this speaker has no stereo to lose.
         */
        for (int32_t i = 0; i < n; i++) {
            const int32_t v16 =
                (s_channels == 2u)
                    ? ((int32_t)pcm[(size_t)(done + i) * 2u] +
                       pcm[(size_t)(done + i) * 2u + 1u]) / 2
                    : (int32_t)pcm[done + i];
            const uint8_t v = (uint8_t)((v16 + 32768) >> 8);
            for (uint8_t k = 0; k < s_up; k++) {
                ring_put(v);
            }
        }
        done += n;
        waited = 0;
    }
    return done;
}

int32_t ag_port_audio_space(void)
{
    return s_dac_on ? (int32_t)((DAC_RING - (s_w - s_r)) / s_up) : 0;
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
/*
 * Eight, not four (27 Sep 2026): sound poured from a neighbour arrives with
 * the wire's jitter, and four descriptors are 22 ms at 44.1 kHz - the S3-Zero
 * ran dry 70-115 times in five minutes with every period arriving, while the
 * CYD beside it, whose converter holds some 90 ms, never did.  Eight are
 * 44 ms, and 3.8 KB more while something plays.
 */
#define I2S_DESCS   8
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

    i2s_std_config_t std = {
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

    /*
     * A DAC that is given an MCLK needs it exact.  The default I2S clock source
     * is only approximate; a converter like the CS4344 counts MCLK against LRCK
     * and stays in power-down until it sees a whole-number ratio, so an
     * inexact MCLK reads as silence.  The APLL is there to make precise audio
     * rates - use it whenever a board wires MCLK.  (Confirmed against a working
     * AcidBox setup, use_apll = true, on this exact board and DAC.)  Boards with
     * no MCLK pin - the CYD's analog amp - keep the default source.
     */
#if SOC_I2S_SUPPORTS_APLL
    if (s_pins.mclk >= 0) {
        std.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    }
#endif

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
    ESP_LOGI(TAG, "i2s out on bclk=%d ws=%d dout=%d mclk=%d, %u Hz, %u ch",
             (int)s_pins.bclk, (int)s_pins.ws, (int)s_pins.dout,
             (int)s_pins.mclk, (unsigned)rate, (unsigned)channels);
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
uint32_t ag_port_audio_stops(void) { return 0; }

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

uint32_t ag_port_audio_stops(void) { return 0; }

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

uint32_t ag_port_audio_stops(void) { return 0; }

int32_t ag_port_audio_space(void)
{
    return 0;
}

#endif /* CONFIG_ARGON_ENABLE_AUDIO */
