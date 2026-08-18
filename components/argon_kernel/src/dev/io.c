/*
 * ArgonOS - direct hardware access.
 *
 * Full trust, DOS style: an application may drive a pin or talk to a chip on a
 * bus without asking a driver's permission.  What it may not do is take a pin
 * that something else is already using, because that is not a failure anyone
 * can diagnose - see ioclaim.h for the rule and the reasoning.
 *
 * Buses come up on first use.  A board that never touches I2C pays nothing for
 * it, and - more to the point - a board whose BOARD.CFG names pins that are
 * wrong does not drive them at boot, before anybody has had a chance to look at
 * the console and notice.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dev/io.h"

#include <stdio.h>
#include <string.h>

#include <argon/board.h>
#include <argon/device.h>
#include <argon/ioclaim.h>
#include <argon/log.h>
#include <argon/proc.h>

#include <argon/port/io.h>
#include <argon/port/mem.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/uart.h>

#define AG_IO_UART_RX_BUF 1024

/*
 * A caller with more to send than AG_PORT_SPI_MAX_XFER splits it, and a chip
 * that cannot have its chip select go down between the parts is driven with
 * cs = -1, holding the select itself.  Both are said out loud in the API
 * reference, because a silent limit looks like a driver that works until the
 * day somebody sends a whole frame.
 */

static ag_port_mutex_t s_lock;

/* ---------------------------------------------------------------------- */

static void lock(void)
{
    if (s_lock != NULL) {
        ag_port_mutex_take_recursive(s_lock, AG_PORT_FOREVER);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        ag_port_mutex_give_recursive(s_lock);
    }
}

/* Whoever is calling: a process, or the kernel when nothing is loaded. */
static ag_pid_t caller(void) { return ag_proc_self(); }

static bool valid_pin(int pin)
{
    return pin >= 0 && pin < ag_io_pin_count();
}

/* ---------------------------------------------------------------------- */
/* What the system is already using                                       */
/* ---------------------------------------------------------------------- */

static void reserve_range(int first, int last, const char *why)
{
    for (int pin = first; pin <= last; pin++) {
        if (valid_pin(pin)) {
            (void)ag_io_reserve(pin, why);
        }
    }
}

static void reserve_pin(int pin, const char *why)
{
    if (pin >= 0 && valid_pin(pin)) {
        (void)ag_io_reserve(pin, why);
    }
}

/*
 * Takes a set of pins for a bus, all or none.  All-or-none matters because the
 * alternative is a bus that failed to come up holding half the pins it wanted,
 * and those pins would stay reserved with nothing using them until the board is
 * reset.  Negative entries are "not connected" and are skipped.
 */
static ag_err_t reserve_bus_pins(const int16_t *pins, unsigned count,
                                 const char *why)
{
    for (unsigned i = 0; i < count; i++) {
        if (pins[i] < 0) {
            continue;
        }
        if (!valid_pin(pins[i])) {
            return -AG_ERANGE;
        }
        if (!ag_io_reservable(pins[i])) {
            ag_log(AG_LOG_WARN, "io", "%s cannot have pin %d: it is in use",
                   why, (int)pins[i]);
            return -AG_EBUSY;
        }
    }
    for (unsigned i = 0; i < count; i++) {
        if (pins[i] >= 0) {
            (void)ag_io_reserve(pins[i], why);
        }
    }
    return AG_OK;
}

/*
 * The pins nothing may touch.  Getting this list wrong in the permissive
 * direction means an application can stop the board from reaching its own code:
 * the flash lines are not a peripheral, they are how instructions arrive.
 */
static void reserve_system_pins(void)
{
    /* Flash, PSRAM - whatever this machine reaches its own code through. */
    const ag_port_pin_range_t *ranges = NULL;
    const unsigned             count = ag_port_reserved_pins(&ranges);
    for (unsigned i = 0; i < count; i++) {
        if (ranges[i].why != NULL) {
            reserve_range(ranges[i].first, ranges[i].last, ranges[i].why);
        }
    }

    /* The console's own pins: taking one is a board that goes silent. */
    reserve_pin(AG_PORT_UART_CONSOLE_TX, "console tx");
    reserve_pin(AG_PORT_UART_CONSOLE_RX, "console rx");

    const ag_board_sd_t *sd = &ag_board()->sd;
    if (sd->kind == AG_SD_SDMMC) {
        reserve_pin(sd->clk, "sd clk");
        reserve_pin(sd->cmd, "sd cmd");
        reserve_pin(sd->d0, "sd d0");
        if (sd->width == 4) {
            reserve_pin(sd->d1, "sd d1");
            reserve_pin(sd->d2, "sd d2");
            reserve_pin(sd->d3, "sd d3");
        }
    } else if (sd->kind == AG_SD_SPI) {
        reserve_pin(sd->sck, "sd sck");
        reserve_pin(sd->mosi, "sd mosi");
        reserve_pin(sd->miso, "sd miso");
        reserve_pin(sd->cs, "sd cs");
    }
    if (sd->kind != AG_SD_NONE) {
        reserve_pin(sd->card_detect, "sd detect");
    }
}

/* ---------------------------------------------------------------------- */
/* GPIO                                                                   */
/* ---------------------------------------------------------------------- */

static const char *mode_reason(int mode)
{
    switch (mode) {
    case AG_GPIO_OUT:
    case AG_GPIO_OUT_OD:      return "gpio out";
    case AG_GPIO_IN_PULLUP:
    case AG_GPIO_IN_PULLDOWN:
    case AG_GPIO_IN:
    default:                  return "gpio in";
    }
}

static ag_err_t io_gpio_config(int pin, int mode)
{
    if (!valid_pin(pin)) {
        return -AG_ERANGE;
    }
    if (mode < AG_GPIO_IN || mode > AG_GPIO_IN_PULLDOWN) {
        return -AG_EINVAL;
    }

    lock();
    ag_err_t err = ag_io_claim(pin, caller(), mode_reason(mode));
    if (err != AG_OK) {
        unlock();
        return err;
    }

    err = ag_port_gpio_config(pin, mode);
    if (err != AG_OK) {
        /* The claim goes back: a pin that could not be configured is not
         * held by anybody, and leaving it held would lose it until reboot. */
        (void)ag_io_release(pin, caller());
    }
    unlock();
    return err;
}

static void io_gpio_write(int pin, int level)
{
    lock();
    if (valid_pin(pin) && ag_io_held_by(pin, caller())) {
        ag_port_gpio_write(pin, level);
    }
    unlock();
}

/*
 * Reading is allowed on any pin, held by anyone or nobody.  It changes nothing,
 * and being able to look at a line the system is using is how a wiring problem
 * gets diagnosed rather than guessed at.
 */
static int io_gpio_read(int pin)
{
    if (!valid_pin(pin)) {
        return -AG_ERANGE;
    }
    return ag_port_gpio_read(pin);
}

static ag_err_t io_gpio_isr(int pin, int edge, ag_isr_fn fn, void *arg)
{
    if (!valid_pin(pin) || fn == NULL) {
        return -AG_EINVAL;
    }
    if (edge < AG_EDGE_RISING || edge > AG_EDGE_BOTH) {
        return -AG_EINVAL;
    }

    lock();
    if (!ag_io_held_by(pin, caller())) {
        /* Configure it first: an interrupt on a pin nobody has set up as an
         * input is a request that cannot mean anything yet. */
        unlock();
        return -AG_EPERM;
    }

    const ag_err_t err = ag_port_gpio_isr_attach(pin, edge, fn, arg);
    if (err == AG_OK) {
        (void)ag_io_set_isr(pin, caller(), true);
        ag_log(AG_LOG_DEBUG, "io", "pid %u took the interrupt on pin %d",
               (unsigned)caller(), pin);
    }
    unlock();
    return err;
}

static ag_err_t io_gpio_isr_clear(int pin)
{
    if (!valid_pin(pin)) {
        return -AG_ERANGE;
    }

    lock();
    if (!ag_io_held_by(pin, caller())) {
        unlock();
        return -AG_EPERM;
    }
    ag_port_gpio_isr_detach(pin);
    (void)ag_io_set_isr(pin, caller(), false);
    unlock();
    return AG_OK;
}

/* ---------------------------------------------------------------------- */
/* I2C                                                                    */
/* ---------------------------------------------------------------------- */

typedef struct {
    bool up;
    bool warned; /* said "no pins" once already                            */
} i2c_state_t;

static i2c_state_t s_i2c[AG_I2C_BUSES];

/* Caller holds the lock. */
static ag_err_t i2c_bring_up(int bus)
{
    if (bus < 0 || bus >= AG_I2C_BUSES) {
        return -AG_ERANGE;
    }
    if (s_i2c[bus].up) {
        return AG_OK;
    }

    const ag_board_i2c_t *cfg = &ag_board()->i2c[bus];
    if (cfg->sda < 0 || cfg->scl < 0) {
        /*
         * Not "broken" - undescribed.  The message names the keys, because the
         * person reading it is holding a board whose schematic only they have.
         * Once, though: a bus scan asks a hundred and twelve times, and a
         * hundred and twelve identical lines are not more informative than one.
         */
        if (!s_i2c[bus].warned) {
            s_i2c[bus].warned = true;
            ag_log(AG_LOG_WARN, "io",
                   "i2c%d has no pins; set i2c%d.sda and i2c%d.scl in BOARD.CFG",
                   bus, bus, bus);
        }
        return -AG_ENODEV;
    }

    char why[AG_IO_REASON_MAX];
    snprintf(why, sizeof(why), "i2c%d", bus);

    const int16_t pins[] = {cfg->sda, cfg->scl};
    ag_err_t      err = reserve_bus_pins(pins, 2, why);
    if (err != AG_OK) {
        return err;
    }

    err = ag_port_i2c_open(bus, cfg->sda, cfg->scl, cfg->khz, cfg->pullups);
    if (err != AG_OK) {
        return err;
    }

    s_i2c[bus].up = true;
    ag_log(AG_LOG_INFO, "io", "i2c%d up on sda=%d scl=%d at %u kHz", bus,
           cfg->sda, cfg->scl, (unsigned)cfg->khz);
    return AG_OK;
}

static ag_err_t io_i2c_write(int bus, uint8_t addr, const void *buf, size_t len,
                             uint32_t timeout_ms)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }

    lock();
    ag_err_t err = i2c_bring_up(bus);
    if (err == AG_OK) {
        err = ag_port_i2c_write(bus, addr, buf, len, timeout_ms);
    }
    unlock();
    return err;
}

static ag_err_t io_i2c_read(int bus, uint8_t addr, void *buf, size_t len,
                            uint32_t timeout_ms)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }

    lock();
    ag_err_t err = i2c_bring_up(bus);
    if (err == AG_OK) {
        err = ag_port_i2c_read(bus, addr, buf, len, timeout_ms);
    }
    unlock();
    return err;
}

static ag_err_t io_i2c_wrrd(int bus, uint8_t addr, const void *wbuf,
                            size_t wlen, void *rbuf, size_t rlen,
                            uint32_t timeout_ms)
{
    if (wbuf == NULL || wlen == 0 || rbuf == NULL || rlen == 0) {
        return -AG_EINVAL;
    }

    lock();
    ag_err_t err = i2c_bring_up(bus);
    if (err == AG_OK) {
        err = ag_port_i2c_wrrd(bus, addr, wbuf, wlen, rbuf, rlen, timeout_ms);
    }
    unlock();
    return err;
}

/*
 * Is anything at this address?
 *
 *   AG_OK        something answered;
 *   -AG_ENOENT   the bus is fine and nothing is at that address;
 *   -AG_ENODEV   there is no such bus, or it has no pins in BOARD.CFG;
 *   anything else - the bus itself is not working.
 *
 * Those first two have to be different answers, and finding out why cost a
 * screen full of the same warning: a scan asks about 112 addresses, and if
 * "empty address" and "no such bus" are the same code, scanning a bus that does
 * not exist looks exactly like scanning an empty one.
 */
static ag_err_t io_i2c_probe(int bus, uint8_t addr)
{
    lock();
    ag_err_t err = i2c_bring_up(bus);
    if (err == AG_OK) {
        /* 10 ms: a chip that is there answers in microseconds, and this is
         * multiplied by every address a scan asks about. */
        err = ag_port_i2c_probe(bus, addr, 10);
    }
    unlock();
    return err;
}

/* ---------------------------------------------------------------------- */
/* SPI                                                                    */
/* ---------------------------------------------------------------------- */

typedef struct {
    int  cs;  /* the chip select this bus is set up for, AG_PIN_NONE if none */
    bool up;
} spi_state_t;

static spi_state_t s_spi[AG_SPI_BUSES];

/* The chip's number (2 or 3) is what BOARD.CFG and the ABI both use. */
static int spi_index(int bus) { return bus - AG_SPI_FIRST; }

static ag_err_t spi_bring_up(int bus)
{
    const int idx = spi_index(bus);
    if (idx < 0 || idx >= AG_SPI_BUSES) {
        return -AG_ERANGE;
    }
    if (s_spi[idx].up) {
        return AG_OK;
    }

    const ag_board_spi_t *cfg = &ag_board()->spi[idx];
    if (cfg->sck < 0 || (cfg->mosi < 0 && cfg->miso < 0)) {
        ag_log(AG_LOG_WARN, "io",
               "spi%d has no pins; set spi%d.sck and spi%d.mosi in BOARD.CFG",
               bus, bus, bus);
        return -AG_ENODEV;
    }

    char why[AG_IO_REASON_MAX];
    snprintf(why, sizeof(why), "spi%d", bus);

    const int16_t pins[] = {cfg->sck, cfg->mosi, cfg->miso};
    ag_err_t      claimed = reserve_bus_pins(pins, 3, why);
    if (claimed != AG_OK) {
        return claimed;
    }

    const ag_err_t rc =
        ag_port_spi_open(bus, cfg->sck, cfg->mosi, cfg->miso, cfg->khz);
    if (rc != AG_OK) {
        return rc;
    }

    s_spi[idx].up = true;
    s_spi[idx].cs = AG_PIN_NONE;
    ag_log(AG_LOG_INFO, "io", "spi%d up on sck=%d mosi=%d miso=%d at %u kHz",
           bus, cfg->sck, cfg->mosi, cfg->miso, (unsigned)cfg->khz);
    return AG_OK;
}

/*
 * The chip select is a pin like any other, and the process talking to that chip
 * owns it.  Taken once, when the bus is first pointed at a new select: the port
 * caches the device behind it, and reserving on every transfer would be a lock
 * and a lookup on a path that is otherwise two memcpys.
 */
static ag_err_t spi_take_cs(int bus, int cs)
{
    const int idx = spi_index(bus);

    if (s_spi[idx].cs == cs) {
        return AG_OK;
    }
    if (cs >= 0) {
        char why[AG_IO_REASON_MAX];
        snprintf(why, sizeof(why), "spi%d cs", bus);

        const int16_t  cs_pin = (int16_t)cs;
        const ag_err_t err = reserve_bus_pins(&cs_pin, 1, why);
        if (err != AG_OK) {
            return err;
        }
    }
    s_spi[idx].cs = cs;
    return AG_OK;
}

/*
 * One transfer, both directions at once, which is what SPI is.  `cs` below zero
 * means the caller drives the chip select itself - a chip that needs it held
 * across several transfers cannot let the peripheral toggle it.
 */
static ag_err_t io_spi_xfer(int bus, int cs, const void *tx, void *rx,
                            size_t len)
{
    if (len == 0 || len > AG_PORT_SPI_MAX_XFER) {
        return -AG_EINVAL;
    }
    if (tx == NULL && rx == NULL) {
        return -AG_EINVAL;
    }

    lock();
    ag_err_t err = spi_bring_up(bus);
    if (err == AG_OK) {
        err = spi_take_cs(bus, cs);
    }
    if (err == AG_OK) {
        err = ag_port_spi_xfer(bus, cs, tx, rx, len);
    }
    unlock();
    return err;
}

/* ---------------------------------------------------------------------- */
/* UART                                                                   */
/* ---------------------------------------------------------------------- */

static bool s_uart_up[AG_UART_PORTS];

static ag_err_t uart_bring_up(int port, const ag_port_uart_cfg_t *want)
{
    if (port < 0 || port >= AG_UART_PORTS) {
        return -AG_ERANGE;
    }
    /*
     * UART0 is how the system is talking to whoever is asking.  Handing it out
     * would end the conversation, and the caller would never see the error.
     */
    if (port == AG_PORT_UART_CONSOLE) {
        return -AG_EBUSY;
    }

    const ag_board_uart_t *cfg = &ag_board()->uart[port];
    if (cfg->tx < 0 && cfg->rx < 0) {
        ag_log(AG_LOG_WARN, "io",
               "uart%d has no pins; set uart%d.tx and uart%d.rx in BOARD.CFG",
               port, port, port);
        return -AG_ENODEV;
    }

    ag_port_uart_cfg_t uart_cfg = {
        .baud = cfg->baud,
        .data_bits = 8,
        .parity = 0,
        .stop_bits = 1,
    };
    if (want != NULL) {
        uart_cfg = *want;
    }

    if (!s_uart_up[port]) {
        char why[AG_IO_REASON_MAX];
        snprintf(why, sizeof(why), "uart%d", port);

        const int16_t  pins[] = {cfg->tx, cfg->rx};
        const ag_err_t taken = reserve_bus_pins(pins, 2, why);
        if (taken != AG_OK) {
            return taken;
        }

        const ag_err_t rc = ag_port_uart_open(port, &uart_cfg,
                                              AG_IO_UART_RX_BUF,
                                              AG_IO_UART_RX_BUF);
        if (rc != AG_OK) {
            return rc;
        }
        s_uart_up[port] = true;
    }

    ag_err_t err = ag_port_uart_config(port, &uart_cfg);
    if (err == AG_OK) {
        err = ag_port_uart_pins(port, cfg->tx, cfg->rx);
    }
    return err;
}

static ag_err_t io_uart_config(int port, uint32_t baud, int databits,
                               int parity, int stopbits)
{
    if (baud == 0 || databits < 5 || databits > 8) {
        return -AG_EINVAL;
    }
    if (parity < 0 || parity > 2 || stopbits < 1 || stopbits > 2) {
        return -AG_EINVAL;
    }

    const ag_port_uart_cfg_t cfg = {
        .baud = baud,
        .data_bits = (uint8_t)databits,
        .parity = (uint8_t)parity,
        .stop_bits = (uint8_t)stopbits,
    };

    lock();
    const ag_err_t err = uart_bring_up(port, &cfg);
    unlock();
    return err;
}

static int32_t io_uart_write(int port, const void *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }

    lock();
    ag_err_t err = uart_bring_up(port, NULL);
    int32_t  n = err;
    if (err == AG_OK) {
        n = ag_port_uart_write(port, buf, len);
        if (n < 0) {
            n = -AG_EIO;
        }
    }
    unlock();
    return n;
}

static int32_t io_uart_read(int port, void *buf, size_t len,
                            uint32_t timeout_ms)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }

    lock();
    ag_err_t err = uart_bring_up(port, NULL);
    unlock();
    if (err != AG_OK) {
        return err;
    }

    /*
     * Outside the lock on purpose: a read with a timeout is a wait, and holding
     * the io lock while waiting would stop every other pin and bus in the
     * system for as long as nothing arrives.
     */
    const int32_t n = ag_port_uart_read(port, buf, len, timeout_ms);
    return (n < 0) ? -AG_EIO : n;
}

/* ---------------------------------------------------------------------- */
/* ADC                                                                    */
/* ---------------------------------------------------------------------- */

/*
 * Present only when the port says it has one, and on this port that is a build
 * option that is off by default - the reason is in argon/port/impl/io.h and it
 * is a good one.  With no ADC, io->adc_read is NULL rather than a stub that
 * fails: an application asks with AG_HAS and adapts, which is what the whole
 * feature-probing convention is for.
 */
#if AG_PORT_HAS_ADC

static int32_t io_adc_read(int channel)
{
    if (channel < 0 || channel >= AG_PORT_ADC_CHANNELS) {
        return -AG_ERANGE;
    }

    /* The channel exists in the silicon but reaches no pin on this target. */
    const int pin = AG_PORT_ADC_GPIO(channel);
    if (pin < 0) {
        return -AG_ENOTSUP;
    }

    lock();

    /* The pin belongs to whoever measures on it, so that two processes do not
     * fight over the attenuation setting of one channel. */
    const ag_err_t err = ag_io_claim(pin, caller(), "adc");
    if (err != AG_OK) {
        unlock();
        return err;
    }

    const int32_t raw = ag_port_adc_read(channel);
    unlock();
    return raw;
}

#endif /* AG_PORT_HAS_ADC */

/* ---------------------------------------------------------------------- */
/* PWM                                                                    */
/* ---------------------------------------------------------------------- */

typedef struct {
    int      pin;
    uint8_t  bits;
    bool     used;
} pwm_channel_t;

static pwm_channel_t s_pwm[AG_PORT_PWM_CHANNELS];

static int pwm_find(int pin)
{
    for (int i = 0; i < AG_PORT_PWM_CHANNELS; i++) {
        if (s_pwm[i].used && s_pwm[i].pin == pin) {
            return i;
        }
    }
    return -1;
}

/* Caller holds the lock.  Stops the output and gives the channel back. */
static void pwm_drop(int pin)
{
    const int ch = pwm_find(pin);
    if (ch < 0) {
        return;
    }
    ag_port_pwm_stop(ch);
    memset(&s_pwm[ch], 0, sizeof(s_pwm[ch]));
}

static ag_err_t io_pwm_config(int pin, uint32_t freq_hz, uint8_t bits)
{
    if (!valid_pin(pin) || freq_hz == 0 || bits < 1 || bits > 14) {
        return -AG_EINVAL;
    }

    lock();
    ag_err_t err = ag_io_claim(pin, caller(), "pwm");
    if (err != AG_OK) {
        unlock();
        return err;
    }

    int ch = pwm_find(pin);
    if (ch < 0) {
        for (int i = 0; i < AG_PORT_PWM_CHANNELS; i++) {
            if (!s_pwm[i].used) {
                ch = i;
                break;
            }
        }
    }
    if (ch < 0) {
        unlock();
        return -AG_ENFILE; /* every channel is driving something else */
    }

    err = ag_port_pwm_config(ch, pin, freq_hz, bits);
    if (err == AG_OK) {
        s_pwm[ch].used = true;
        s_pwm[ch].pin = pin;
        s_pwm[ch].bits = bits;
    } else {
        (void)ag_io_release(pin, caller());
    }
    unlock();
    return err;
}

static ag_err_t io_pwm_set(int pin, uint32_t duty)
{
    lock();
    if (!ag_io_held_by(pin, caller())) {
        unlock();
        return -AG_EPERM;
    }

    const int ch = pwm_find(pin);
    if (ch < 0) {
        unlock();
        return -AG_ENODEV; /* configure it first */
    }

    const uint32_t max = (1u << s_pwm[ch].bits) - 1u;
    if (duty > max) {
        duty = max;
    }

    const ag_err_t err = ag_port_pwm_set(ch, duty);
    unlock();
    return err;
}

/* ---------------------------------------------------------------------- */
/* Reclaim                                                                */
/* ---------------------------------------------------------------------- */

/*
 * Everything a dying process was driving, put back.  The interrupt handler
 * first, because its code is about to be freed and an edge on that pin after
 * that is a board that stops with nothing in the journal to say why.
 */
static void release_pin(int pin, bool had_isr, void *ctx)
{
    (void)ctx;

    if (had_isr) {
        ag_port_gpio_isr_detach(pin);
    }
    pwm_drop(pin);
    /* Back to a high-impedance input: an output left driving is a pin fighting
     * whatever is connected to it. */
    ag_port_gpio_reset(pin);
}

uint32_t ag_io_reclaim(ag_pid_t pid)
{
    lock();
    const uint32_t freed = ag_io_release_owner(pid, release_pin, NULL);
    unlock();

    if (freed > 0) {
        ag_log(AG_LOG_DEBUG, "io", "pid %u gave back %u pin(s)",
               (unsigned)pid, (unsigned)freed);
    }
    return freed;
}

/* ---------------------------------------------------------------------- */

const ag_io_api_t ag_io_api_table = {
    .size = sizeof(ag_io_api_t),
    .gpio_config = io_gpio_config,
    .gpio_write = io_gpio_write,
    .gpio_read = io_gpio_read,
    .gpio_isr = io_gpio_isr,
    .gpio_isr_clear = io_gpio_isr_clear,
    .i2c_write = io_i2c_write,
    .i2c_read = io_i2c_read,
    .i2c_wrrd = io_i2c_wrrd,
    .i2c_probe = io_i2c_probe,
    .spi_xfer = io_spi_xfer,
    .uart_write = io_uart_write,
    .uart_read = io_uart_read,
    .uart_config = io_uart_config,
#if AG_PORT_HAS_ADC
    .adc_read = io_adc_read,
#else
    .adc_read = NULL,
#endif
    .pwm_config = io_pwm_config,
    .pwm_set = io_pwm_set,
};

ag_err_t ag_io_init(void)
{
    if (s_lock == NULL) {
        s_lock = ag_port_mutex_new_recursive();
        if (s_lock == NULL) {
            return -AG_ENOMEM;
        }
    }

    const ag_err_t err = ag_io_claims_init(AG_PORT_GPIO_PINS);
    if (err != AG_OK) {
        return err;
    }

    reserve_system_pins();
    ag_log(AG_LOG_INFO, "io", "%d pins, %u reserved by the system",
           ag_io_pin_count(), (unsigned)ag_io_claimed_count());
    return AG_OK;
}

/* ---------------------------------------------------------------------- */
/* The buses as devices                                                   */
/* ---------------------------------------------------------------------- */

/*
 * A bus has nothing to read or write as a whole - the chips on it do - so these
 * carry no read and no write, only an identity.  They exist so that `dev`
 * answers "what does this board have" completely, rather than listing the
 * things that happen to look like files.
 */
static const ag_dev_ops_t k_bus_ops = {0};

static void register_bus(const char *name, const char *driver,
                         ag_dev_class_t cls)
{
    const ag_dev_desc_t desc = {
        .name = name,
        .driver = driver,
        .cls = cls,
        .ops = &k_bus_ops,
    };
    (void)ag_dev_register(&desc, NULL);
}

void ag_io_register_devices(void)
{
    static const char *const k_i2c_names[AG_I2C_BUSES] = {"i2c0", "i2c1"};
    static const char *const k_spi_names[AG_SPI_BUSES] = {"spi2", "spi3"};
    static const char *const k_uart_names[AG_UART_PORTS] = {"uart0", "uart1",
                                                            "uart2"};

    register_bus("gpio", "soc", AG_DEV_GPIO);

    for (int i = 0; i < AG_I2C_BUSES; i++) {
        const ag_board_i2c_t *cfg = &ag_board()->i2c[i];
        if (cfg->sda >= 0 && cfg->scl >= 0) {
            register_bus(k_i2c_names[i], "i2c", AG_DEV_BUS);
        }
    }
    for (int i = 0; i < AG_SPI_BUSES; i++) {
        const ag_board_spi_t *cfg = &ag_board()->spi[i];
        if (cfg->sck >= 0) {
            register_bus(k_spi_names[i], "spi", AG_DEV_BUS);
        }
    }
    for (int i = 1; i < AG_UART_PORTS; i++) {
        const ag_board_uart_t *cfg = &ag_board()->uart[i];
        if (cfg->tx >= 0 || cfg->rx >= 0) {
            register_bus(k_uart_names[i], "uart", AG_DEV_BUS);
        }
    }
}
