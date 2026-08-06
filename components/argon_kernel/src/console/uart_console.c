/*
 * ArgonOS - UART console endpoint.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "console/uart_console.h"

#include <argon/console.h>

#include "driver/uart.h"

/*
 * 2 KB rather than 1: this is what absorbs the bytes already in flight when the
 * console sends XOFF, and at 115200 baud a kilobyte is only 89 ms of line.
 */
#define AG_UART_RX_BUFFER 2048

typedef struct {
    uart_port_t port;
} ag_uart_ctx_t;

static ag_uart_ctx_t s_ports[UART_NUM_MAX];

static int32_t uart_ep_write(void *ctx, const char *data, size_t len)
{
    const ag_uart_ctx_t *u = (const ag_uart_ctx_t *)ctx;
    return uart_write_bytes(u->port, data, len);
}

static int32_t uart_ep_read(void *ctx, uint8_t *buf, size_t len)
{
    const ag_uart_ctx_t *u = (const ag_uart_ctx_t *)ctx;
    /* Zero timeout: the console task polls, it does not wait here. */
    const int n = uart_read_bytes(u->port, buf, len, 0);
    return (n < 0) ? 0 : n;
}

static const ag_con_transport_t k_uart_transport = {
    .name = "uart",
    .write = uart_ep_write,
    .read = uart_ep_read,
};

ag_err_t ag_uart_console_attach(int port, int baud)
{
    if (port < 0 || port >= UART_NUM_MAX) {
        return -AG_EINVAL;
    }

    const uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (!uart_is_driver_installed(port)) {
        if (uart_driver_install(port, AG_UART_RX_BUFFER, 0, 0, NULL, 0) !=
            ESP_OK) {
            return -AG_EIO;
        }
    }
    if (uart_param_config(port, &cfg) != ESP_OK) {
        return -AG_EIO;
    }

    s_ports[port].port = (uart_port_t)port;
    return ag_console_attach(&k_uart_transport, &s_ports[port]);
}
