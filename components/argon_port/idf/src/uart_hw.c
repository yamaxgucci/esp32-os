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
#include "freertos/FreeRTOS.h"

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

    uart_config_t hw;
    fill(&hw, cfg);
    return from_esp(uart_param_config(port, &hw));
}

ag_err_t ag_port_uart_pins(int port, int tx, int rx)
{
    if (!valid(port)) {
        return -AG_EINVAL;
    }
    return from_esp(uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
}

bool ag_port_uart_is_open(int port)
{
    return valid(port) && uart_is_driver_installed(port);
}

ag_err_t ag_port_uart_flush(int port)
{
    if (!valid(port)) {
        return -AG_EINVAL;
    }
    return from_esp(uart_flush(port));
}

int32_t ag_port_uart_write(int port, const void *buf, size_t len)
{
    if (!valid(port) || buf == NULL) {
        return -AG_EINVAL;
    }
    return (int32_t)uart_write_bytes(port, buf, len);
}

int32_t ag_port_uart_read(int port, void *buf, size_t len, uint32_t timeout_ms)
{
    if (!valid(port) || buf == NULL) {
        return -AG_EINVAL;
    }
    const int n = uart_read_bytes(port, buf, len, pdMS_TO_TICKS(timeout_ms));
    return (n < 0) ? 0 : (int32_t)n;
}

int32_t ag_port_uart_pending(int port)
{
    if (!valid(port)) {
        return -AG_EINVAL;
    }
    size_t avail = 0;
    if (uart_get_buffered_data_len(port, &avail) != ESP_OK) {
        return -AG_EIO;
    }
    return (int32_t)avail;
}
