/*
 * ArgonOS port: ESP-IDF - serial ports.
 *
 * A thin pass-through to the IDF UART driver, and thin on purpose: everything
 * above this (who owns the pins, which port the console is on, how a burst is
 * throttled) is policy and lives in the kernel.  What is here is the part that
 * would have to be written again for a different chip.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/uart.h>

#include "driver/uart.h"
#include "esp_rom_uart.h"
#if CONFIG_IDF_TARGET_ESP32
/*
 * On the original ESP32 the drain below is this header's `uart_tx_wait_idle`
 * and not the one esp_rom_uart.h declares.  The ROM routine of that name has a
 * bug, so IDF replaces it with a static inline here - and a static inline emits
 * no symbol, while `esp32.rom.api.ld` still writes
 * PROVIDE(esp_rom_uart_tx_wait_idle = uart_tx_wait_idle) against a name nothing
 * defines.  Every other chip has the real thing in its own <chip>.rom.ld, which
 * is why the link broke on this target alone.  Reaching for the address in
 * `esp32.rom.redefined.ld` would have linked the buggy version; this takes the
 * one IDF wrote to replace it.
 */
#include "esp32/rom/uart.h"
#endif
#include "freertos/FreeRTOS.h"

#ifdef AG_PORT_UART_JTAG
/*
 * The console pseudo-port on a board whose only console is the native
 * USB-Serial-JTAG (see impl/uart.h).  It is not a UART at all, so every entry
 * point below forks to the usb_serial_jtag driver for this one port number and
 * leaves the real UART path untouched for the rest.
 */
#include "driver/usb_serial_jtag.h"

static bool s_jtag_open;

static bool is_jtag(int port) { return port == AG_PORT_UART_JTAG; }
#else
static bool is_jtag(int port) { (void)port; return false; }
#endif

static bool valid(int port)
{
    return port >= 0 && port < AG_PORT_UART_PORTS;
}

static ag_err_t from_esp(esp_err_t err)
{
    switch (err) {
    case ESP_OK:                return AG_OK;
    case ESP_ERR_INVALID_ARG:   return -AG_EINVAL;
    case ESP_ERR_INVALID_STATE: return -AG_EBUSY;
    case ESP_ERR_NO_MEM:        return -AG_ENOMEM;
    case ESP_ERR_TIMEOUT:       return -AG_ETIMEDOUT;
    default:                    return -AG_EIO;
    }
}

static void fill(uart_config_t *out, const ag_port_uart_cfg_t *cfg)
{
    /*
     * Zero first, and here rather than at the one call site, so that adding a
     * second caller cannot lose it again.
     *
     * IDF keeps growing uart_config_t - rx_flow_ctrl_thresh, and then a flags
     * word whose allow_pd bit asks for sleep retention that this UART does not
     * have - and every field this function does not know about is a field read
     * off the stack.  What that produces is not a compile error and not a
     * consistent runtime one either: uart_param_config refuses the port only
     * when the garbage happens to carry the wrong bit, so the same call can
     * succeed at one baud rate and fail at the next, and the error names light
     * sleep rather than anything the caller did.  Here it arrived as
     * `uart1 at 921600 baud: input/output error` from an application, while
     * 115200 through the same path worked.
     */
    *out = (uart_config_t){0};

    static const uart_word_length_t k_bits[] = {
        UART_DATA_5_BITS, UART_DATA_6_BITS, UART_DATA_7_BITS, UART_DATA_8_BITS,
    };
    /* 0 none, 1 odd, 2 even - the order the ABI documents, not IDF's. */
    static const uart_parity_t k_parity[] = {
        UART_PARITY_DISABLE, UART_PARITY_ODD, UART_PARITY_EVEN,
    };

    const uint8_t bits = (cfg->data_bits >= 5 && cfg->data_bits <= 8)
                             ? (uint8_t)(cfg->data_bits - 5)
                             : 3;

    out->baud_rate = (int)cfg->baud;
    out->data_bits = k_bits[bits];
    out->parity = k_parity[(cfg->parity <= 2) ? cfg->parity : 0];
    out->stop_bits = (cfg->stop_bits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
    out->flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

    /*
     * Not the default clock, and this is the line that lets the processor slow
     * down to the crystal.
     *
     * A UART divides its baud rate out of whatever it is clocked by.  The
     * default is the peripheral bus, which on this family stays at 80 MHz while
     * the processor runs off the PLL - and follows the processor down when it
     * runs off the crystal.  A port set up for 115200 at 80 MHz then talks at
     * half that, which arrives as unreadable bytes rather than as an error: on
     * the board, `power eco 40` ended the conversation mid-line and only a reset
     * brought it back.
     *
     * So the clock is chosen to be one that does not move.  On the S3 that is
     * the crystal itself; on the ESP32 it is REF_TICK, a 1 MHz tick whose
     * divider ESP-IDF re-programs on every frequency change for exactly this
     * reason.  REF_TICK cannot divide down to fast baud rates, so anything
     * above 115200 keeps the bus clock and keeps the old limitation with it -
     * a file transfer at 921600 is not a thing to do at 40 MHz anyway.
     */
#if SOC_UART_SUPPORT_XTAL_CLK
    out->source_clk = UART_SCLK_XTAL;
#elif SOC_UART_SUPPORT_REF_TICK
    out->source_clk =
        (cfg->baud <= 115200u) ? UART_SCLK_REF_TICK : UART_SCLK_APB;
#else
    out->source_clk = UART_SCLK_DEFAULT;
#endif
}

ag_err_t ag_port_uart_open(int port, const ag_port_uart_cfg_t *cfg,
                           uint32_t rx_bytes, uint32_t tx_bytes)
{
    if (!valid(port) || cfg == NULL) {
        return -AG_EINVAL;
    }

#ifdef AG_PORT_UART_JTAG
    if (is_jtag(port)) {
        if (s_jtag_open) {
            return AG_OK; /* already up: two subsystems may both want it */
        }
        usb_serial_jtag_driver_config_t jc = {
            .rx_buffer_size = (int)(rx_bytes ? rx_bytes : 256u),
            .tx_buffer_size = (int)(tx_bytes ? tx_bytes : 256u),
        };
        const esp_err_t rc = usb_serial_jtag_driver_install(&jc);
        if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
            return from_esp(rc);
        }
        s_jtag_open = true;
        return AG_OK;
    }
#endif

    if (!uart_is_driver_installed(port)) {
        /*
         * Already installed is not a failure: two subsystems may both want the
         * port up and neither knows about the other.
         */
        const esp_err_t rc = uart_driver_install(port, (int)rx_bytes,
                                                 (int)tx_bytes, 0, NULL, 0);
        if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
            return from_esp(rc);
        }
    }
    return ag_port_uart_config(port, cfg);
}

ag_err_t ag_port_uart_config(int port, const ag_port_uart_cfg_t *cfg)
{
    if (!valid(port) || cfg == NULL) {
        return -AG_EINVAL;
    }

    /* USB-Serial-JTAG has no baud, parity or stop bits to set. */
    if (is_jtag(port)) {
        return AG_OK;
    }

    /*
     * Let whatever is already going out finish before the port is
     * reconfigured.  This is the console's take-over from the ROM/second-stage
     * boot log on UART0: reprogramming the divider and line while a boot-log
     * byte is still in the shift register wedges the port, and the system comes
     * up silent - no banner, no prompt.  It is timing-sensitive, so it hid
     * until an unrelated change to the image's layout (the PSRAM code arena)
     * shifted the moment of the take-over and made it reproducible.  Draining
     * first removes the race rather than the symptom; on a port with nothing in
     * flight it returns at once.
     */
#if CONFIG_IDF_TARGET_ESP32
    uart_tx_wait_idle((uint8_t)port);
#else
    esp_rom_uart_tx_wait_idle((uint8_t)port);
#endif

    uart_config_t hw;
    fill(&hw, cfg);
    return from_esp(uart_param_config(port, &hw));
}

ag_err_t ag_port_uart_pins(int port, int tx, int rx)
{
    if (!valid(port)) {
        return -AG_EINVAL;
    }
    if (is_jtag(port)) {
        return AG_OK; /* fixed on the USB pins; nothing to route */
    }
    return from_esp(uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
}

bool ag_port_uart_is_open(int port)
{
#ifdef AG_PORT_UART_JTAG
    if (is_jtag(port)) {
        return s_jtag_open;
    }
#endif
    return valid(port) && uart_is_driver_installed(port);
}

ag_err_t ag_port_uart_flush(int port)
{
    if (!valid(port)) {
        return -AG_EINVAL;
    }
    if (is_jtag(port)) {
        return AG_OK; /* write_bytes already hands bytes to the peripheral */
    }
    return from_esp(uart_flush(port));
}

int32_t ag_port_uart_write(int port, const void *buf, size_t len)
{
    if (!valid(port) || buf == NULL) {
        return -AG_EINVAL;
    }
#ifdef AG_PORT_UART_JTAG
    if (is_jtag(port)) {
        /* A bounded wait, so a board with nothing attached to its USB does not
         * wedge the writer once the peripheral's buffer fills - the same way a
         * UART with no listener drops into the void. */
        const int n = usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(100));
        return (n < 0) ? 0 : (int32_t)n;
    }
#endif
    return (int32_t)uart_write_bytes(port, buf, len);
}

int32_t ag_port_uart_read(int port, void *buf, size_t len, uint32_t timeout_ms)
{
    if (!valid(port) || buf == NULL) {
        return -AG_EINVAL;
    }
#ifdef AG_PORT_UART_JTAG
    if (is_jtag(port)) {
        const int n = usb_serial_jtag_read_bytes(buf, len,
                                                 pdMS_TO_TICKS(timeout_ms));
        return (n < 0) ? 0 : (int32_t)n;
    }
#endif
    const int n = uart_read_bytes(port, buf, len, pdMS_TO_TICKS(timeout_ms));
    return (n < 0) ? 0 : (int32_t)n;
}

int32_t ag_port_uart_pending(int port)
{
    if (!valid(port)) {
        return -AG_EINVAL;
    }
    if (is_jtag(port)) {
        /* No count to ask for; callers fall back to a timed read. */
        return -AG_ENOTSUP;
    }
    size_t avail = 0;
    if (uart_get_buffered_data_len(port, &avail) != ESP_OK) {
        return -AG_EIO;
    }
    return (int32_t)avail;
}
