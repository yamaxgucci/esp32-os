/*
 * ArgonOS port: ESP-IDF - restarting into the chip's own ROM loader.
 *
 * WHY THIS EXISTS.  A board that is being flashed by its NEIGHBOUR rather than
 * by a PC has to get into the ROM download loader somehow, and the classic way
 * needs two wires nobody has: the boot strap held low while the reset line is
 * pulsed.  On the C3 SuperMini the strap (GPIO 9) is on the header and the
 * reset line is not brought out at all, so that way is simply unavailable.
 *
 * The newer parts can do it in software instead: one bit in an RTC register
 * that the FIRST-STAGE ROM reads before anything else, so a running system can
 * ask to come back up as a loader.  ESP-IDF uses exactly this to implement
 * "reboot into the bootloader" over the native USB console
 * (esp_system/port/usb_console.c); here it is used to hand the board to a
 * different flasher on the other end of a UART.
 *
 * WHAT IT COSTS, and the cost is the whole reason this file carries a warning
 * rather than a convenience: the bit lives in the RTC domain, which a software
 * reset does not clear, and NOTHING in the ROM or in ESP-IDF clears it either -
 * searched, and the only two writes in the whole of IDF are the two that set
 * it.  So a board sent here stays a loader across every reset until it loses
 * power.  After flashing it, pull the plug.
 *
 * Which parts have the bit is not a guess and not a family rule: the register
 * is absent from the ESP32's headers and from the C6's, and present on the C3
 * and the S3.  So the capability is asked, never assumed.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/sys.h>

#include "sdkconfig.h"

#if defined(__has_include)
#if __has_include("soc/rtc_cntl_reg.h")
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#endif
#endif

#include "esp_system.h"

#if defined(RTC_CNTL_OPTION1_REG) && defined(RTC_CNTL_FORCE_DOWNLOAD_BOOT)
#define AG_HAS_DOWNLOAD_BOOT 1
#else
#define AG_HAS_DOWNLOAD_BOOT 0
#endif

bool ag_port_download_boot_capable(void)
{
    return AG_HAS_DOWNLOAD_BOOT != 0;
}

void ag_port_download_boot(void)
{
#if AG_HAS_DOWNLOAD_BOOT
    /*
     * The whole register, not a read-modify-write, and that is IDF's own
     * spelling of it: every other bit in OPTION1 is a force-this-at-boot of
     * the same kind, and carrying one of them over a deliberate restart would
     * be inheriting a decision nobody made here.
     */
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
#endif
}
