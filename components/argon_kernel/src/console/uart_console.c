/*
 * ArgonOS - UART console endpoint.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "console/uart_console.h"

#include <argon/console.h>

#include <argon/port/uart.h>

/*
 * 2 KB rather than 1: this is what absorbs the bytes already in flight when the
 * console sends XOFF, and at 115200 baud a kilobyte is only 89 ms of line.
 */
#define AG_UART_RX_BUFFER 2048

typedef struct {
    int port;
} ag_uart_ctx_t;

static ag_uart_ctx_t s_ports[AG_PORT_UART_PORTS];

static int32_t uart_ep_write(void *ctx, const char *data, size_t len)
{
    const ag_uart_ctx_t *u = (const ag_uart_ctx_t *)ctx;
    return ag_port_uart_write(u->port, data, len);
}

static int32_t uart_ep_read(void *ctx, uint8_t *buf, size_t len)
{
    const ag_uart_ctx_t *u = (const ag_uart_ctx_t *)ctx;
    /* Zero timeout: the console task polls, it does not wait here. */
    const int32_t n = ag_port_uart_read(u->port, buf, len, 0);
    return (n < 0) ? 0 : n;
}

static const ag_con_transport_t k_uart_transport = {
    .name = "uart",
    .write = uart_ep_write,
    .read = uart_ep_read,
};

ag_err_t ag_uart_console_attach(int port, int baud)
{
    if (port < 0 || port >= AG_PORT_UART_PORTS) {
        return -AG_EINVAL;
    }

    const ag_port_uart_cfg_t cfg = {
        .baud = (uint32_t)baud,
        .data_bits = 8,
        .parity = 0,
        .stop_bits = 1,
    };

    const ag_err_t err = ag_port_uart_open(port, &cfg, AG_UART_RX_BUFFER, 0);
    if (err != AG_OK) {
        return err;
    }

    s_ports[port].port = port;
    return ag_console_attach(&k_uart_transport, &s_ports[port]);
}
