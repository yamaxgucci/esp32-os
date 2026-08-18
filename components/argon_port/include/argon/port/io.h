/*
 * ArgonOS port contract - pins and buses.
 *
 * The largest single thing the system asks of a chip, and the one where the
 * split between "policy" and "driver" is worth stating out loud, because
 * everything above this line is policy and none of it is here:
 *
 *   who owns a pin, and what happens to it when that process dies;
 *   which pins a bus is on, and where that came from (BOARD.CFG);
 *   that a bus comes up on first use and costs nothing until then;
 *   that reading a pin is always allowed and driving one is not.
 *
 * All of that is in src/dev/io.c and src/dev/ioclaim.c and stays there.  What is
 * here is the part that touches registers: set this pin to that mode, put these
 * bytes on that bus.  A port implements twenty-odd short functions and inherits
 * the ownership model, the diagnostics and the error mapping for nothing.
 *
 * Modes, edges and error codes are the ABI's own (AG_GPIO_*, AG_EDGE_*, AG_E*),
 * so a port never invents a vocabulary and never returns a foreign error.
 *
 * What a port must supply
 * -----------------------
 *
 *   AG_PORT_GPIO_PINS      how many pins the chip has
 *   AG_PORT_PWM_CHANNELS   how many independent PWM outputs
 *   AG_PORT_SPI_MAX_XFER   the longest single SPI transfer
 *   AG_PORT_HAS_ADC        1 if analogue input is built in this configuration
 *   AG_PORT_ADC_CHANNELS   how many analogue channels
 *   AG_PORT_ADC_GPIO(ch)   the pin channel `ch` measures, or -1 if it reaches
 *                          none here - the caller claims that pin, so a port
 *                          that cannot name it must say -1 rather than guess
 *
 *   unsigned ag_port_reserved_pins(const ag_port_pin_range_t **out)
 *
 *   ag_err_t ag_port_gpio_config(int pin, int mode)
 *   void     ag_port_gpio_write(int pin, int level)
 *   int      ag_port_gpio_read(int pin)
 *   ag_err_t ag_port_gpio_isr_attach(int pin, int edge, ag_isr_fn fn, void *arg)
 *   void     ag_port_gpio_isr_detach(int pin)
 *   void     ag_port_gpio_reset(int pin)
 *
 *   ag_err_t ag_port_i2c_open(int bus, int sda, int scl, uint32_t khz,
 *                             bool pullups)
 *   ag_err_t ag_port_i2c_write / _read / _wrrd / _probe
 *
 *   ag_err_t ag_port_spi_open(int bus, int sck, int mosi, int miso,
 *                             uint32_t khz)
 *   ag_err_t ag_port_spi_xfer(int bus, int cs, const void *tx, void *rx,
 *                             size_t len)
 *
 *   ag_err_t ag_port_pwm_config(int channel, int pin, uint32_t hz, uint8_t bits)
 *   ag_err_t ag_port_pwm_set(int channel, uint32_t duty)
 *   void     ag_port_pwm_stop(int channel)
 *
 *   int32_t  ag_port_adc_read(int channel)
 *
 * Contract, not advice
 * --------------------
 *
 * - An output keeps its input buffer enabled, so that reading a pin says what is
 *   on the line rather than what was written to it.  For push-pull that is a
 *   free confirmation; for open-drain it is the whole point, because the line is
 *   only low when nobody is holding it up - which is how I2C and 1-Wire are read
 *   at all.
 * - ag_port_gpio_reset returns a pin to a high-impedance input.  It is what a
 *   dying process's pins go through, and an output left driving is a pin
 *   fighting whatever is wired to it.
 * - i2c_probe must distinguish "nothing at this address" (-AG_ENOENT) from "no
 *   such bus" (-AG_ENODEV).  A scan asks about 112 addresses, and if those are
 *   the same code then scanning a bus that does not exist looks exactly like
 *   scanning an empty one - which is a real hour of somebody's life.
 * - i2c_probe must also not fill the log.  An empty address times out, and a
 *   driver that reports every timeout as an error buries the answer the scan was
 *   asked for (pitfall 28 in docs/05-status.md).
 * - spi_xfer transfers both directions at once, which is what SPI is.  `cs`
 *   below zero means the caller drives chip select itself: a chip that needs it
 *   held across several transfers cannot let the peripheral toggle it.
 * - Buses are opened with the pins the kernel found in BOARD.CFG.  Opening one
 *   that is already open is not an error.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IO_H
#define ARGON_PORT_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

/*
 * A span of pins the machine itself is using: the flash lines are not a
 * peripheral, they are how instructions arrive.  Getting this list wrong in the
 * permissive direction means an application can stop the board from reaching its
 * own code.
 */
typedef struct {
    int16_t     first;
    int16_t     last; /* inclusive */
    const char *why;  /* "flash", "psram" - shown by the `io` command */
} ag_port_pin_range_t;

unsigned ag_port_reserved_pins(const ag_port_pin_range_t **out);

ag_err_t ag_port_gpio_config(int pin, int mode);
void     ag_port_gpio_write(int pin, int level);
int      ag_port_gpio_read(int pin);
ag_err_t ag_port_gpio_isr_attach(int pin, int edge, ag_isr_fn fn, void *arg);
void     ag_port_gpio_isr_detach(int pin);
void     ag_port_gpio_reset(int pin);

ag_err_t ag_port_i2c_open(int bus, int sda, int scl, uint32_t khz,
                          bool pullups);
ag_err_t ag_port_i2c_write(int bus, uint8_t addr, const void *buf, size_t len,
                           uint32_t timeout_ms);
ag_err_t ag_port_i2c_read(int bus, uint8_t addr, void *buf, size_t len,
                          uint32_t timeout_ms);
ag_err_t ag_port_i2c_wrrd(int bus, uint8_t addr, const void *wbuf, size_t wlen,
                          void *rbuf, size_t rlen, uint32_t timeout_ms);
ag_err_t ag_port_i2c_probe(int bus, uint8_t addr, uint32_t timeout_ms);

ag_err_t ag_port_spi_open(int bus, int sck, int mosi, int miso, uint32_t khz);
ag_err_t ag_port_spi_xfer(int bus, int cs, const void *tx, void *rx,
                          size_t len);

ag_err_t ag_port_pwm_config(int channel, int pin, uint32_t hz, uint8_t bits);
ag_err_t ag_port_pwm_set(int channel, uint32_t duty);
void     ag_port_pwm_stop(int channel);

int32_t ag_port_adc_read(int channel);

#include <argon/port/impl/io.h>

#endif /* ARGON_PORT_IO_H */
