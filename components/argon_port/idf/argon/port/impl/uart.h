/*
 * ArgonOS port: ESP-IDF - serial ports, the constants only.
 *
 * The functions are in src/uart_hw.c: a UART is a driver, not a rename, and
 * inlining it would put ESP-IDF's headers back into every file that says the
 * word "console".
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_UART_H
#define ARGON_PORT_IMPL_UART_H

#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "soc/uart_pins.h"

#define AG_PORT_UART_PIN_KEEP (-1)

#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)

/*
 * This board's only console is the chip's own USB-Serial-JTAG, not a UART -
 * there is no USB-to-UART bridge on it, so UART0 talks to unconnected pins.
 * Model the endpoint as one pseudo-port just past the real UARTs: the console
 * attaches to it like any other port (uart_console.c indexes s_ports by number
 * and validates against AG_PORT_UART_PORTS, both of which now include it), and
 * uart_hw.c routes that one index to the usb_serial_jtag driver.  It owns no
 * GPIOs, so the console pins are none and nothing is reserved for it.
 */
#define AG_PORT_UART_JTAG       SOC_UART_NUM
#define AG_PORT_UART_PORTS      (SOC_UART_NUM + 1)
#define AG_PORT_UART_CONSOLE    AG_PORT_UART_JTAG
#define AG_PORT_UART_CONSOLE_TX (-1)
#define AG_PORT_UART_CONSOLE_RX (-1)

#else /* a real UART is the console */

#define AG_PORT_UART_PORTS SOC_UART_NUM

/* The port the ROM and the boot log talk on: handing it out ends the
 * conversation, and whoever asked would never see the error. */
#ifdef CONFIG_ESP_CONSOLE_UART_NUM
#define AG_PORT_UART_CONSOLE CONFIG_ESP_CONSOLE_UART_NUM
#else
#define AG_PORT_UART_CONSOLE (-1)
#endif

/*
 * The pins the console is actually on.  ESP-IDF only defines the CONFIG_
 * symbols when they were customised; with the defaults it uses U0TXD_GPIO_NUM,
 * and a build that looked only at the CONFIG_ symbols left the pins the system
 * is talking over free for anybody to take - which showed up as an empty pair
 * of rows in `io`, and would have shown up on a board as a console that went
 * silent.
 */
#if defined(CONFIG_ESP_CONSOLE_UART_CUSTOM) && CONFIG_ESP_CONSOLE_UART_TX_GPIO >= 0
#define AG_PORT_UART_CONSOLE_TX CONFIG_ESP_CONSOLE_UART_TX_GPIO
#else
#define AG_PORT_UART_CONSOLE_TX U0TXD_GPIO_NUM
#endif

#if defined(CONFIG_ESP_CONSOLE_UART_CUSTOM) && CONFIG_ESP_CONSOLE_UART_RX_GPIO >= 0
#define AG_PORT_UART_CONSOLE_RX CONFIG_ESP_CONSOLE_UART_RX_GPIO
#else
#define AG_PORT_UART_CONSOLE_RX U0RXD_GPIO_NUM
#endif

#endif /* console kind */

#endif /* ARGON_PORT_IMPL_UART_H */
