/*
 * ArgonOS - the ES8388 on an AI-Thinker Audio Kit.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "codec_es8388.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <argon/port/io.h>

#define TAG "es8388"

/* The chip answers here; the scan that found it printed this number. */
#define ES8388_ADDR 0x10

/* One register write is two bytes and there is no other shape of transfer. */
#define ES_TIMEOUT_MS 50

/*
 * The registers this file touches, by the names the datasheet uses.  Numbers
 * are spelled out rather than computed from a base, because a register map is
 * not an array and the one time somebody writes DACCONTROL(7) they will be
 * off by the gap at 0x1C.
 */
#define ES_CONTROL1    0x00
#define ES_CONTROL2    0x01
#define ES_CHIPPOWER   0x02
#define ES_ADCPOWER    0x03
#define ES_DACPOWER    0x04
#define ES_MASTERMODE  0x08
#define ES_ADCCONTROL1 0x09 /* microphone preamplifier gain               */
#define ES_ADCCONTROL2 0x0a /* which of the input pairs is listened to    */
#define ES_ADCCONTROL3 0x0b
#define ES_ADCCONTROL4 0x0c /* input word format                          */
#define ES_ADCCONTROL5 0x0d /* input oversampling ratio                   */
#define ES_ADCVOL_L    0x10
#define ES_ADCVOL_R    0x11
#define ES_DACCONTROL1 0x17 /* output word format                         */
#define ES_DACCONTROL2 0x18 /* output oversampling ratio                  */
#define ES_DACCONTROL3 0x19 /* the mute bit                               */
#define ES_DACVOL_L    0x1a
#define ES_DACVOL_R    0x1b
#define ES_MIX_LSEL    0x26
#define ES_MIX_L       0x27
#define ES_MIX_R       0x2a
#define ES_LRC_SHARE   0x2b
#define ES_VROI        0x2d
#define ES_LOUT1VOL    0x2e
#define ES_ROUT1VOL    0x2f
#define ES_LOUT2VOL    0x30
#define ES_ROUT2VOL    0x31

static ag_err_t wr(int bus, uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    return ag_port_i2c_write(bus, ES8388_ADDR, buf, sizeof(buf), ES_TIMEOUT_MS);
}

static ag_err_t rd(int bus, uint8_t reg, uint8_t *val)
{
    return ag_port_i2c_wrrd(bus, ES8388_ADDR, &reg, 1, val, 1, ES_TIMEOUT_MS);
}

/*
 * Read back the handful of registers that decide whether a sound comes out,
 * and say what they hold.
 *
 * A write that returns AG_OK means the bus acknowledged a byte, not that the
 * chip kept it.  A codec whose reference never came up, or that reset itself
 * under a clock it did not like, acknowledges everything and holds nothing -
 * and the setup above would report twenty-one successes into a silence.  This
 * is the difference between "we told it to unmute" and "it is unmuted", and
 * the two are worth an afternoon apart.
 */
static void verify(int bus)
{
    static const struct {
        uint8_t     reg;
        uint8_t     want;
        const char *what;
    } k_check[] = {
        {ES_CHIPPOWER, 0x00, "chip power"},
        {ES_DACPOWER, 0x3c, "outputs open"},
        {ES_DACCONTROL3, 0x00, "unmuted"},
        {ES_DACCONTROL1, 0x18, "I2S 16 bit"},
        {ES_DACCONTROL2, 0x02, "256x"},
        {ES_MASTERMODE, 0x00, "slave"},
        {ES_LOUT1VOL, 0x1e, "headphone volume"},
        {ES_MIX_L, 0x90, "left to mixer"},
    };
    for (size_t i = 0; i < sizeof(k_check) / sizeof(k_check[0]); i++) {
        uint8_t        got = 0xff;
        const ag_err_t err = rd(bus, k_check[i].reg, &got);
        if (err != AG_OK) {
            ESP_LOGW(TAG, "readback 0x%02x (%s): bus said %d",
                     k_check[i].reg, k_check[i].what, (int)err);
        } else if (got != k_check[i].want) {
            ESP_LOGW(TAG, "readback 0x%02x (%s): 0x%02x, wrote 0x%02x",
                     k_check[i].reg, k_check[i].what, got, k_check[i].want);
        } else {
            ESP_LOGI(TAG, "readback 0x%02x (%s): 0x%02x as written",
                     k_check[i].reg, k_check[i].what, got);
        }
    }
}

/*
 * A step in the sequence.  It is a table and not a run of calls so that the
 * order is readable as an order: every line of it matters, and three of them
 * matter in a way that is invisible from the call site.
 */
typedef struct {
    uint8_t     reg;
    uint8_t     val;
    const char *why;
} es_step_t;

/*
 * THE OUTPUT SIDE.
 *
 * Read top to bottom this is: stop everything, say what the words look like,
 * route the converters to the mixers, open the outputs, unmute last.  Unmuting
 * last is not tidiness - the chip is being reconfigured while it is powered,
 * and every intermediate state on the way is audible as a click or worse into
 * whatever is plugged in, which on this board may be headphones on somebody's
 * head.
 */
static const es_step_t k_out[] = {
    {ES_DACCONTROL3, 0x04, "mute the output before touching anything else"},
    {ES_CONTROL2, 0x50, "analogue reference on, the value the part wants"},
    {ES_CHIPPOWER, 0x00, "all internal blocks out of power-down"},
    {ES_MASTERMODE, 0x00, "slave: every clock on this board comes from us"},
    {ES_DACPOWER, 0xc0, "converters powered, all four outputs still shut"},
    {ES_CONTROL1, 0x12, "normal operation, reference up"},
    {ES_DACCONTROL1, 0x18, "I2S, 16 bits - the same words the port sends"},
    {ES_DACCONTROL2, 0x02, "256x oversampling, which is what MCLK is set to"},
    {ES_LRC_SHARE, 0x80, "one word clock for both directions"},
    /*
     * AND RESTART THE DIGITAL ENGINE, because that register just moved.
     *
     * This is the one line this driver was missing, and it is taken from
     * Espressif's own es8388_start: it reads DACCONTROL21 before and after
     * writing it and, if the value changed, pulses CHIPPOWER 0xF0 -> 0x00.
     * What that pulse does is stop and restart the chip's DEM and STM - the
     * decimator/interpolator and the state machine that count the clocks.
     *
     * Reconfiguring which word clock those two run from, and leaving them
     * running, is what produced the symptom this board spent a day on: six
     * opens inside one boot giving alive, dead, alive, dead, alive, dead.  An
     * exact alternation is a state machine left in the wrong phase, and the
     * reference driver has always known to restart it.
     *
     * Found by Maxim's suggestion - stop theorising and go read a project
     * that works - after five of my own theories had each been measured and
     * each been wrong.
     */
    {ES_CHIPPOWER, 0xf0, "stop the decimator and the state machine"},
    {ES_CHIPPOWER, 0x00, "and start them again on the clock just chosen"},
    {ES_MIX_LSEL, 0x00, "nothing from the analogue inputs into the output"},
    {ES_MIX_L, 0x90, "left converter into the left mixer"},
    {ES_MIX_R, 0x90, "right converter into the right mixer"},
    {ES_VROI, 0x00, "low output resistance while playing"},
    {ES_DACVOL_L, 0x00, "0 dB: the digital path attenuates nothing"},
    {ES_DACVOL_R, 0x00, "0 dB"},
    {ES_LOUT1VOL, 0x1e, "headphone left, 0 dB"},
    {ES_ROUT1VOL, 0x1e, "headphone right, 0 dB"},
    {ES_LOUT2VOL, 0x1e, "speaker amplifier left, 0 dB"},
    {ES_ROUT2VOL, 0x1e, "speaker amplifier right, 0 dB"},
    {ES_DACPOWER, 0x3c, "now open all four outputs"},
    {ES_DACCONTROL3, 0x00, "and only now unmute"},
};

/*
 * THE INPUT SIDE.
 *
 * LINPUT2/RINPUT2 is the line input on this board; LINPUT1/RINPUT1 is the pair
 * of microphones soldered to it.  A guitar is not a microphone and must not go
 * through 24 dB of preamplifier, so the gain here is 0 dB - the level this
 * expects is the one the buffer in docs/10-watch-board.md provides.  If the
 * board turns out to have the two pairs the other way round, this is the one
 * line to change and the symptom will be unmistakable: the onboard microphones
 * will be what the amplifier is modelling.
 */
static const es_step_t k_in[] = {
    {ES_ADCPOWER, 0xff, "input side fully down while it is configured"},
    /* ES_ADCCONTROL1 is written separately: it carries the board's ingain. */
    {ES_ADCCONTROL2, 0x50, "listen to LINPUT2/RINPUT2, the line input"},
    {ES_ADCCONTROL3, 0x02, "ordinary stereo, no swap"},
    {ES_ADCCONTROL4, 0x0c, "I2S, 16 bits, matching the output"},
    {ES_ADCCONTROL5, 0x02, "256x, the same ratio as the output"},
    {ES_ADCVOL_L, 0x00, "0 dB"},
    {ES_ADCVOL_R, 0x00, "0 dB"},
    {ES_ADCPOWER, 0x09, "converters and analogue inputs up, bias off"},
};

static bool run(int bus, const es_step_t *steps, size_t n, const char *what)
{
    for (size_t i = 0; i < n; i++) {
        const ag_err_t err = wr(bus, steps[i].reg, steps[i].val);
        if (err != AG_OK) {
            /*
             * Naming the step rather than the register number: the number is
             * in the datasheet and the reason is not, and whoever reads this
             * line is looking at a board that will not make a sound.
             */
            ESP_LOGE(TAG, "%s stopped at step %u (%s): %d", what, (unsigned)i,
                     steps[i].why, (int)err);
            return false;
        }
    }
    return true;
}

/*
 * Ask the codec, while it is playing, whether it is still the codec we set up.
 *
 * A part that browns out, or that loses the clock it divides, comes back with
 * its power-on defaults: muted, outputs shut.  Every register write we made
 * would still have returned success, hours ago, and nothing since would say
 * otherwise.  This is the only way the board can notice that from the inside,
 * and it exists because the alternative was a person standing next to it.
 *
 * Silent when all is well: a line every few seconds saying "still fine" is a
 * line nobody reads by the time it matters.
 */
void ag_codec_es8388_poll(int bus)
{
    static const struct {
        uint8_t     reg;
        uint8_t     want;
        const char *what;
    } k_watch[] = {
        {ES_DACPOWER, 0x3c, "outputs"},
        {ES_DACCONTROL3, 0x00, "mute"},
        {ES_CHIPPOWER, 0x00, "chip power"},
    };
    if (bus < 0) {
        return;
    }
    for (size_t i = 0; i < sizeof(k_watch) / sizeof(k_watch[0]); i++) {
        uint8_t        got = 0xff;
        const ag_err_t err = rd(bus, k_watch[i].reg, &got);
        if (err != AG_OK) {
            ESP_LOGW(TAG, "codec stopped answering on %s: %d",
                     k_watch[i].what, (int)err);
        } else if (got != k_watch[i].want) {
            ESP_LOGW(TAG, "codec changed under us: %s is 0x%02x, was 0x%02x",
                     k_watch[i].what, got, k_watch[i].want);
        }
    }
}

bool ag_codec_es8388_begin(int bus, bool capture, int gain_db)
{
    if (bus < 0) {
        return false;
    }

    const ag_err_t seen = ag_port_i2c_probe(bus, ES8388_ADDR, ES_TIMEOUT_MS);
    if (seen != AG_OK) {
        /*
         * Not an error here.  The board said there is a bus; it did not
         * promise this chip is on it, and a board with a bare converter and a
         * stray audio.i2c line should play exactly as it always did.
         */
        ESP_LOGI(TAG,
                 "no codec at 0x%02x on i2c%d (%d); leaving the output as the "
                 "wires alone make it",
                 ES8388_ADDR, bus, (int)seen);
        return false;
    }

    /*
     * Reset first, unconditionally.  This runs on every open, and the second
     * open of a session inherits whatever the first one left - including a
     * half-configured chip from an open that failed in the middle.
     */
    if (wr(bus, ES_CONTROL1, 0x80) != AG_OK) {
        ESP_LOGE(TAG, "codec answered the probe and refused the reset");
        return false;
    }
    (void)wr(bus, ES_CONTROL1, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10)); /* the part's own power-on settling */

    if (!run(bus, k_out, sizeof(k_out) / sizeof(k_out[0]), "output setup")) {
        return false;
    }
    /*
     * The preamplifier, from the board rather than from a constant.
     *
     * Register 0x09 carries two four-bit gains, left in the high nibble and
     * right in the low, each a step of 3 dB up to 24.  Nothing here decides
     * what a guitar needs; the board says what is wired to its jack.
     */
    if (capture) {
        const uint8_t step = (uint8_t)((gain_db < 0 ? 0 : gain_db) / 3);
        const uint8_t both = (uint8_t)((step > 8u ? 8u : step) * 0x11u);
        if (wr(bus, ES_ADCCONTROL1, both) != AG_OK) {
            ESP_LOGW(TAG, "the codec refused the input gain");
        } else if (both != 0u) {
            ESP_LOGI(TAG, "input preamplifier at +%d dB", (int)(step * 3));
        }
    }
    if (capture &&
        !run(bus, k_in, sizeof(k_in) / sizeof(k_in[0]), "input setup")) {
        /* The output is already up and working; losing the input is not a
         * reason to take it down with it. */
        ESP_LOGW(TAG, "playing, but nothing will be recorded");
    }

    verify(bus);
    ESP_LOGI(TAG, "ES8388 up on i2c%d: out unmuted at 0 dB, in %s", bus,
             capture ? "on line input at 0 dB" : "off");
    return true;
}

void ag_codec_es8388_end(int bus)
{
    if (bus < 0 || ag_port_i2c_probe(bus, ES8388_ADDR, ES_TIMEOUT_MS) != AG_OK) {
        return;
    }
    /* Mute before shutting anything down, for the same reason the setup
     * unmutes last. */
    (void)wr(bus, ES_DACCONTROL3, 0x04);
    (void)wr(bus, ES_DACPOWER, 0xc0);
    (void)wr(bus, ES_ADCPOWER, 0xff);
    (void)wr(bus, ES_CHIPPOWER, 0xf3);
}
