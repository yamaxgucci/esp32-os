/*
 * ArgonOS port: ESP-IDF - moving the clock.
 *
 * ESP-IDF will not change the processor's frequency at run time unless the
 * image was built with its power management in it (CONFIG_PM_ENABLE, which
 * ARGON_ENABLE_PM selects).  Without that, esp_pm_configure answers
 * ESP_ERR_NOT_SUPPORTED and there is no back door worth taking: the low level
 * call that sets the clock directly does not tell the timers, the serial port
 * or the flash controller that their dividers now mean something else.
 *
 * So this file is honest in two directions.  Built with power management, it
 * hands the request to ESP-IDF, which owns the locks every driver takes to say
 * "not while I am talking to the bus".  Built without it, caps() says zero and
 * the shell prints the one frequency this image has.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/power.h>

#include "sdkconfig.h"

#include "esp_pm.h"
#include "esp_private/esp_clk.h"

#include <argon/port/bt.h>
#include <argon/port/wifi.h>

#if defined(CONFIG_ARGON_ENABLE_PM) && CONFIG_ARGON_ENABLE_PM
#define AG_PM_BUILT 1
#else
#define AG_PM_BUILT 0
#endif

#define AG_CPU_MAX_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ

/* 40 on every part in this family, 26 on a few modules: whatever it is, it is
 * the one setting where the peripheral bus moves with the processor. */
#ifdef CONFIG_XTAL_FREQ
#define AG_XTAL_MHZ CONFIG_XTAL_FREQ
#else
#define AG_XTAL_MHZ 40
#endif

#if AG_PM_BUILT
static ag_err_t from_esp(esp_err_t err)
{
    switch (err) {
    case ESP_OK:                return AG_OK;
    case ESP_ERR_INVALID_ARG:   return -AG_EINVAL;
    case ESP_ERR_INVALID_STATE: return -AG_EBUSY;
    case ESP_ERR_NOT_SUPPORTED: return -AG_ENOTSUP;
    default:                    return -AG_EIO;
    }
}
#endif

uint32_t ag_port_power_caps(void)
{
#if AG_PM_BUILT
    return AG_PORT_PWR_CPU_BAND;
#else
    return 0u;
#endif
}

uint32_t ag_port_power_cpu_mhz(void)
{
    /*
     * Read, not remembered.  With scaling on, this is wherever the last driver
     * to take a lock left it, and the whole point of printing it is that
     * nothing above can work it out from what was asked for.
     */
    const int hz = esp_clk_cpu_freq();
    return (hz > 0) ? (uint32_t)(hz / 1000000) : (uint32_t)AG_CPU_MAX_MHZ;
}

/*
 * Is a radio running?
 *
 * The one thing on this part that genuinely cannot live on a slower peripheral
 * bus.  Everything else that divides off it - the panel, the card, I2C, the
 * backlight's PWM - simply runs at half the rate and carries on; the radio has
 * a hard requirement for 80 MHz, and giving it less is not a slower radio but a
 * broken one, with nothing on the console to say so.
 *
 * Asked here rather than remembered above, because this is where the fact is.
 */
/*
 * Off until an installation says otherwise: the step below the bus floor works
 * as far as anybody has looked, and "as far as anybody has looked" is not the
 * same as safe.  See ag_port_power_allow_crystal in the contract.
 */
static bool s_allow_crystal;

void ag_port_power_allow_crystal(bool on) { s_allow_crystal = on; }

static bool radio_on(void)
{
#if AG_PORT_HAS_WIFI
    ag_port_wifi_status_t w;
    if (ag_port_wifi_status(&w) == AG_OK && w.state != AG_WIFI_OFF) {
        return true;
    }
#endif
#if AG_PORT_HAS_BT
    ag_port_bt_status_t b;
    if (ag_port_bt_status(&b) == AG_OK && b.state != AG_BT_OFF) {
        return true;
    }
#endif
    return false;
}

/*
 * The clock tree of this family, not a range.  CPU_CLK comes off the PLL
 * divided by two, three or six, or off the crystal directly - so the settings
 * are 240, 160, 80 and 40, and there is nothing between them and nothing above
 * them.  240 is the ceiling on both the ESP32 and the S3: the PLL is 480 MHz
 * and there is no divider of one for the processor.  This part does not
 * overclock, and a caller asking for 320 is told -AG_EINVAL rather than given a
 * number that looks like it worked.
 *
 * The crystal is the odd one and was nearly dropped.  While the processor runs
 * off the PLL the peripheral bus stays at 80 MHz and every divider latched off
 * it still means what it meant; on the crystal the bus follows the processor
 * down.  On the board (22 August 2026) that turned the console into unreadable
 * bytes inside one line and left it that way, because the serial port divides
 * its baud rate out of that bus - and a board that cannot be talked to cannot
 * be told to speed up again.
 *
 * That is fixed where it belonged: the console is now clocked from something
 * that does not move (uart_hw.c).  What remains is a radio, which needs the bus
 * at 80 and cannot be told otherwise, so the step is withheld while one is
 * running rather than offered and then regretted.  Everything else on the bus
 * gets slower and keeps working.
 */
uint32_t ag_port_power_cpu_steps(uint16_t *out, uint32_t max)
{
    static const uint16_t k_steps[] = {240, 160, 80, AG_XTAL_MHZ};

    const bool hold_bus = radio_on();

    uint32_t n = 0;
    for (unsigned i = 0; i < sizeof(k_steps) / sizeof(k_steps[0]); i++) {
        const uint16_t mhz = k_steps[i];
        if (mhz > (uint16_t)AG_CPU_MAX_MHZ) {
            continue;
        }
        if (mhz < 80u && (hold_bus || !s_allow_crystal)) {
            continue; /* below the bus floor: asked for, and nothing on the
                       * machine holding the bus where it is */
        }
        if (out != NULL && n < max) {
            out[n] = mhz;
        }
        n++;
    }
    return n;
}

uint32_t ag_port_power_bus_floor_mhz(void)
{
    /*
     * 80, and it is the same number on both parts in this family: CPU_CLK off
     * the PLL at 240, 160 or 80 all leave APB_CLK at 80 MHz, and only the
     * crystal setting takes it down with the processor.
     */
    return 80u;
}

const char *ag_port_power_note(void)
{
    if (!s_allow_crystal) {
        return "40 MHz (the crystal) is off: it moves the peripheral bus, and "
               "only the console has been made immune. [power] crystal = 1";
    }
    if (radio_on()) {
        return "the radio holds the bus at 80 MHz; 40 is not offered while it "
               "is on";
    }
    return NULL;
}

static bool is_step(uint32_t mhz)
{
    uint16_t steps[AG_PORT_PWR_STEPS_MAX];
    const uint32_t n = ag_port_power_cpu_steps(steps, AG_PORT_PWR_STEPS_MAX);
    for (uint32_t i = 0; i < n && i < AG_PORT_PWR_STEPS_MAX; i++) {
        if ((uint32_t)steps[i] == mhz) {
            return true;
        }
    }
    return false;
}

ag_err_t ag_port_power_cpu_band(uint32_t min_mhz, uint32_t max_mhz)
{
    if (min_mhz == 0u || max_mhz == 0u || min_mhz > max_mhz) {
        return -AG_EINVAL;
    }
    if (!is_step(min_mhz) || !is_step(max_mhz)) {
        return -AG_EINVAL;
    }

#if AG_PM_BUILT
    const esp_pm_config_t cfg = {
        .max_freq_mhz = (int)max_mhz,
        .min_freq_mhz = (int)min_mhz,
        /*
         * Not here.  Light sleep needs every task in the system to stop asking
         * to be woken ten times a second first, and that is a different piece
         * of work with a different way of being measured - see docs/11-power.md.
         */
        .light_sleep_enable = false,
    };
    return from_esp(esp_pm_configure(&cfg));
#else
    return -AG_ENOTSUP;
#endif
}
