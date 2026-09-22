/*
 * ArgonOS port: ESP-IDF - the machine itself.
 *
 * AG_PORT_NOINIT is RTC_NOINIT_ATTR: RTC slow memory keeps its contents across
 * everything except a power cycle, which is exactly the distinction the boot
 * counter needs - a crash streak must survive the reboot it causes and must not
 * survive somebody switching the board off and on to clear it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_SYS_H
#define ARGON_PORT_IMPL_SYS_H

#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "sdkconfig.h"

#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

#define AG_PORT_NOINIT      RTC_NOINIT_ATTR
#define AG_PORT_TARGET_NAME CONFIG_IDF_TARGET

/*
 * The chip's own account of why it started, in words.
 *
 * Every one of these means something different to whoever is holding the board:
 * a brownout is a power supply or a radio drawing more than the regulator will
 * give, a task watchdog is code that stopped yielding, a panic is a fault that
 * was reported before the reset.  Collapsing them into "something went wrong"
 * is right for the recovery counter and useless for a person.
 *
 * ALL OF THEM, and the last five are the reason this list is worth a comment.
 * They were missing, so every one of them printed as "unknown" - and "unknown"
 * reads as "this port cannot ask", which is the opposite of the truth: the
 * reason register is read on every boot and the answer was being thrown away
 * here, in the switch.  It cost two sessions of a bench chasing a phantom
 * "the middle board dies" on a C3 whose console is its USB-Serial-JTAG: every
 * attach to COM8 resets that chip - `rst:0x15 (USB_UART_CHIP_RESET)` - which is
 * ESP_RST_USB, and the board said "unknown" while holding the fact.
 *
 * A board that restarts tells nobody why on its own; this line is the only
 * place it can say.  A value that is not named here is not reported as missing,
 * it is reported as a mystery.
 */
static inline const char *ag_port_reset_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external reset";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout - the supply sagged";
    case ESP_RST_SDIO:     return "sdio";
    case ESP_RST_USB:      return "usb-serial-jtag - the console was opened";
    case ESP_RST_JTAG:     return "jtag";
    case ESP_RST_EFUSE:    return "efuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch - the supply flickered";
    case ESP_RST_CPU_LOCKUP: return "cpu lockup - double exception";
    default:               return "unknown";
    }
}

static inline ag_reset_t ag_port_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return AG_RESET_POWERON;
    case ESP_RST_SW:      return AG_RESET_SOFTWARE;
    /*
     * The two resets a HOST causes by attaching.  This chip's console is its
     * own USB-Serial-JTAG, so merely opening the port resets it - measured:
     * `rst:0x15 (USB_UART_CHIP_RESET)` on every `boardtest` run - and a JTAG
     * debugger drives the same reset.  That is somebody looking at the board,
     * not the board failing, so it must not be counted as a crash; the watchdog,
     * the lockup and every other reason fall to AG_RESET_OTHER and stay counted.
     */
    case ESP_RST_USB:     return AG_RESET_ATTACH;
    case ESP_RST_JTAG:    return AG_RESET_ATTACH;
    default:              return AG_RESET_OTHER;
    }
}

static inline void ag_port_restart(void) { esp_restart(); }

/*
 * Out of line, in src/dlboot_hw.c, and that is not a style choice: it wants the
 * chip's RTC register map, and this header is pulled into most of the kernel.
 * A macro table belongs in one translation unit, not in all of them.
 */
bool ag_port_download_boot_capable(void);
void ag_port_download_boot(void);

static inline uint8_t ag_port_cpu_cores(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    return (uint8_t)chip.cores;
}

static inline uint8_t ag_port_cpu_revision(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    return (uint8_t)(chip.revision / 100);
}

static inline uint32_t ag_port_cpu_hz(void)
{
    return (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000u;
}

static inline size_t ag_port_psram_size(void)
{
#if CONFIG_SPIRAM
    return esp_psram_is_initialized() ? esp_psram_get_size() : 0u;
#else
    return 0u;
#endif
}

/*
 * Running code out of PSRAM needs an MMU that can map the SPI RAM onto the
 * instruction bus.  Only the S3 and the P4 can; on the original ESP32 the PSRAM
 * is data-only.  The capability is architectural - whether a given map call
 * succeeds is spike S-1's business, on real hardware.
 */
static inline bool ag_port_psram_executable(void)
{
#if CONFIG_SPIRAM && (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4)
    return true;
#else
    return false;
#endif
}

#endif /* ARGON_PORT_IMPL_SYS_H */
