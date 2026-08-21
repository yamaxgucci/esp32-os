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

#ifdef CONFIG_XTAL_FREQ
#define AG_XTAL_MHZ CONFIG_XTAL_FREQ
#else
#define AG_XTAL_MHZ 40
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
 * divided by two or four, or off the crystal directly - so the settings are
 * 240, 160, 80 and 40, and there is nothing between them and nothing above
 * them.  240 is the ceiling on both the ESP32 and the S3: the PLL is 480 MHz
 * and there is no divider of one for the processor.  This part does not
 * overclock, and a caller asking for 320 is told -AG_EINVAL rather than
 * given a number that looks like it worked.
 */
uint32_t ag_port_power_cpu_steps(uint16_t *out, uint32_t max)
{
    /* The crystal last: it is 40 MHz on every part in this family and 26 on a
     * few modules, so it never collides with a setting off the PLL. */
    static const uint16_t k_steps[] = {240, 160, 80, AG_XTAL_MHZ};

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
