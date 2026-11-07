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
    /*
     * Each byte in as it comes.  By default the UART hands bytes over 120 at
     * a time and the last few of a burst only when the line has been idle for
     * ten characters - and the last few are the Enter that ends a line.
     *
     * Espressif's QEMU loses that idle-line interrupt when the host is slow:
     * its timer is one-shot, and if it fires while the interrupt handler is
     * still busy with the previous block, the handler clears it without taking
     * the new bytes.  They then sit in the FIFO until something else arrives.
     * On the test PC once it became a VM (7 Nov 2026, 2.6x slower than the
     * laptop) every `recv` in `argon test -Put` ended that way: 109 bytes in
     * the FIFO (read with gdb), threshold 120, no timeout raised, `END` never
     * seen - and the transfer looked a hundred times slower than it was.  With
     * the FIFO-full interrupt at one byte, which QEMU computes from the FIFO's
     * level rather than from a timer, the same transfer took 4 s.
     *
     * On a board this costs an interrupt per byte while bytes arrive - a few
     * thousand a second during a paste, nothing at a prompt - and a keystroke
     * no longer waits for the line to go quiet.
     */
    (void)ag_port_uart_rx_eager(port);

    s_ports[port].port = port;
    return ag_console_attach(&k_uart_transport, &s_ports[port]);
}

void ag_uart_console_detach(int port)
{
    if (port < 0 || port >= AG_PORT_UART_PORTS) {
        return;
    }
    /* Detach by the same ctx we attached with; the driver is left installed so
     * the caller keeps a working port to read and write directly. */
    ag_console_detach(&s_ports[port]);
}
