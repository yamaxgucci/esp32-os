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

#if defined(CONFIG_ARGON_ENABLE_PM) && CONFIG_ARGON_ENABLE_PM
#define AG_PM_BUILT 1
#else
#define AG_PM_BUILT 0
#endif

#define AG_CPU_MAX_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ

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
 * The clock tree of this family, not a range.  CPU_CLK comes off the PLL
 * divided by two, three or six - so the settings are 240, 160 and 80, and there
 * is nothing between them and nothing above them.  240 is the ceiling on both
 * the ESP32 and the S3: the PLL is 480 MHz and there is no divider of one for
 * the processor.  This part does not overclock, and a caller asking for 320 is
 * told -AG_EINVAL rather than given a number that looks like it worked.
 *
 * The crystal - a fourth setting, 40 MHz - is deliberately not offered, and
 * that is a measurement rather than an opinion.  While the processor runs off
 * the PLL the peripheral bus stays at 80 MHz and every divider latched off it
 * still means what it meant; on the crystal the bus follows the processor down.
 * On the board (ESP32-2432S028R, 22 August 2026) `power eco 40` turned the
 * console into unreadable bytes inside one line and left it that way: the baud
 * rate was suddenly wrong, and a board that cannot be talked to cannot be told
 * to speed up again.  Only a reset came back.
 *
 * It is not only the console - SPI and the card divide off the same bus - so
 * offering the step would mean reconfiguring every bus in the system on the way
 * down and on the way up.  That is a piece of work, not a constant, and until
 * somebody does it the honest list has three entries.
 */
uint32_t ag_port_power_cpu_steps(uint16_t *out, uint32_t max)
{
    static const uint16_t k_steps[] = {240, 160, 80};

    uint32_t n = 0;
    for (unsigned i = 0; i < sizeof(k_steps) / sizeof(k_steps[0]); i++) {
        if (k_steps[i] > (uint16_t)AG_CPU_MAX_MHZ) {
            continue;
        }
        if (out != NULL && n < max) {
            out[n] = k_steps[i];
        }
        n++;
    }
    return n;
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
