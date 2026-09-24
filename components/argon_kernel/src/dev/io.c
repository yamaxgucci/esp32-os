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
#include <argon/spilink.h>
#include <argon/uartlink.h>

#include <argon/port/io.h>
#include <argon/port/mem.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/uart.h>

/*
 * A kilobyte of receive buffer is a console's worth, and it was a console this
 * was written for.  It is not a link's worth.
 *
 * At four megabaud a kilobyte is two and a half milliseconds - so a reader that
 * stops reading for longer than that loses bytes, and any reader that does
 * something with what it read stops for longer than that.  Found with the video
 * link between the two boards on the desk: the receiver presents each band of
 * pixels to its panel over SPI before asking for the next one, and bytes went
 * missing.  The diagnosis is in the numbers rather than in a guess - the same
 * 757 000 pixels were sent at two megabaud and at four, and the corrupt frames
 * doubled with the rate (2 then 4) instead of staying level.  A bad line gives
 * the same count for the same bytes; an overrun gives the same count for the
 * same *time*, which is what this was.
 *
 * Eight kilobytes is twenty milliseconds at four megabaud, which is longer than
 * anything a driver does between reads.  Transmit stays small on purpose: a
 * short transmit buffer makes a large write block until the wire has taken it,
 * and being paced by the wire is the correct behaviour for whoever is filling
 * it - the alternative is buffering a frame the far end cannot yet draw.
 */
#define AG_IO_UART_RX_BUF 8192
#define AG_IO_UART_TX_BUF 2048
/*
 * A port whose link goes by DMA reads and writes neither ring: the DMA takes
 * the FIFO itself.  The driver stays installed (the line's settings go
 * through it) with the least it accepts - on the C3's 16 Mbaud wire the two
 * rings were 10 KB of a 97 KB link that nothing ever touched.
 */
#define AG_IO_UART_DMA_RX_BUF 256
#define AG_IO_UART_DMA_TX_BUF 0

/* A port up, with the driver's rings this size if it comes up now. */
static ag_err_t uart_bring_up_sized(int port, const ag_port_uart_cfg_t *want,
                                    uint32_t rx_bytes, uint32_t tx_bytes);

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
/*
 * Whose claim a pin operation is checked against.
 *
 * Normally the process doing it.  But a loadable driver claims its pins when it
 * loads - on the kernel's task, because that is who loads modules - and is then
 * called from whatever task happens to need it: the console task for the
 * console, an application's task the moment that application takes the display.
 * Checked against the application, the claim does not match, and io_gpio_write
 * *silently does nothing*.
 *
 * What that looked like: the panel driver's text path worked perfectly (console
 * task, kernel identity) while its pixel path put a whole 320x240 frame into a
 * single 8x8 character cell.  The chip select line is toggled by the SPI
 * peripheral, so the pixel bytes went out; the DC line is a plain GPIO, so the
 * three command bytes that set the address window did not - they went into the
 * panel as pixels, and the frame landed in whatever window the text path had
 * left behind, which was the caret.  Nothing failed, nothing was logged, and
 * the picture was one blinking square inside a line of text.
 *
 * A .SYS is not a process; it is system code the kernel called.  Its claims are
 * the kernel's and so are its writes, whoever asked.
 */
static ag_pid_t caller(void)
{
    return ag_dev_in_driver() ? AG_PID_KERNEL : ag_proc_self();
}

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

/*
 * Refused writes, counted.
 *
 * A permission check with no else branch is not a permission check; it is a
 * silence.  This one cost a day: a driver's writes to its own data/command line
 * were dropped because the claim belonged to the kernel and the call arrived on
 * an application's task, and the only symptom anywhere in the system was a
 * picture in the wrong place.
 *
 * Logging from here is not allowed - the caller may be a driver, holding the
 * device registry, and the console task takes the console first and the
 * registry second, so reaching for the console here closes a ring.  So the
 * refusal is remembered and `io` prints it.  One line at the bottom of a table
 * somebody looks at anyway is worth more than a message nobody can print.
 */
static struct {
    uint32_t count;
    int16_t  pin;
    ag_pid_t pid;
} s_refused = {0, -1, 0};

uint32_t ag_io_refused(int *pin, ag_pid_t *pid)
{
    if (pin != NULL) {
        *pin = s_refused.pin;
    }
    if (pid != NULL) {
        *pid = s_refused.pid;
    }
    return s_refused.count;
}

static void io_gpio_write(int pin, int level)
{
    lock();
    if (valid_pin(pin) && ag_io_held_by(pin, caller())) {
        ag_port_gpio_write(pin, level);
    } else if (valid_pin(pin)) {
        s_refused.count++;
        s_refused.pin = (int16_t)pin;
        s_refused.pid = caller();
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
    /*
     * Which end of the clock the bus came up as.  It cannot change afterwards:
     * the port has a device table and a set of pins on one end and a single
     * transaction pair on the other, and the master and slave entry points are
     * answered from different state.  A caller asking for the other role gets
     * -AG_EBUSY rather than a bus that looks open and transfers nothing.
     */
    bool slave;
} spi_state_t;

static spi_state_t s_spi[AG_SPI_BUSES];

/* The chip's number (2 or 3) is what BOARD.CFG and the ABI both use. */
static int spi_index(int bus) { return bus - AG_SPI_FIRST; }

/*
 * `cs` is the slave's select input and is only read when slave is true: the
 * master takes its selects one at a time, on the transfers that use them (see
 * spi_take_cs), while a slave hands its one select to the driver at init and
 * has no transfer call to carry another.
 */
static ag_err_t spi_bring_up(int bus, bool slave, int cs)
{
    const int idx = spi_index(bus);
    if (idx < 0 || idx >= AG_SPI_BUSES) {
        return -AG_ERANGE;
    }
    if (s_spi[idx].up) {
        if (s_spi[idx].slave != slave) {
            return -AG_EBUSY;
        }
        /*
         * A slave's select is handed to the driver once, at init, so a second
         * open naming a different pin cannot take effect.  Saying so is better
         * than answering "open" to a caller who now believes another pin
         * selects this board.
         */
        if (slave && s_spi[idx].cs != cs) {
            return -AG_EBUSY;
        }
        return AG_OK;
    }
    if (slave && cs < 0) {
        return -AG_EINVAL;
    }

    const ag_board_spi_t *cfg = &ag_board()->spi[idx];
    /*
     * A bus needs a data line.  Whether it needs a clock is the board's
     * answer, not ours: normally it does and leaving sck out is a typo, but a
     * device driven by the shape of the signal rather than by clocked bits is
     * wired to mosi alone - the addressable LED on a development board is
     * three bits of SPI per bit of its own and has no clock input at all.  So
     * a bus with no data line is undescribed, and a bus with no clock is
     * allowed exactly when there is nothing to read back.
     */
    if ((cfg->mosi < 0 && cfg->miso < 0) || (cfg->sck < 0 && cfg->miso >= 0)) {
        ag_log(AG_LOG_WARN, "io",
               "spi%d has no pins; set spi%d.sck and spi%d.mosi in BOARD.CFG",
               bus, bus, bus);
        return -AG_ENODEV;
    }

    char why[AG_IO_REASON_MAX];
    snprintf(why, sizeof(why), "spi%d", bus);

    /* The slave's select is claimed with the rest: nobody else may drive the
     * pin that decides whether this board answers. */
    const int16_t  pins[] = {cfg->sck, cfg->mosi, cfg->miso, (int16_t)cs};
    const unsigned count = slave ? 4u : 3u;
    ag_err_t       claimed = reserve_bus_pins(pins, count, why);
    if (claimed != AG_OK) {
        return claimed;
    }

    const ag_err_t rc =
        slave ? ag_port_spi_slave_open(bus, cfg->sck, cfg->mosi, cfg->miso, cs,
                                       cfg->khz)
              : ag_port_spi_open(bus, cfg->sck, cfg->mosi, cfg->miso, cfg->khz);
    if (rc != AG_OK) {
        return rc;
    }

    /*
     * The bus is up, so it is recorded as up before the drive strength is
     * applied.  The strength is a tuning of a bus that exists, and failing the
     * bring-up over it would leave a bus that is initialized and claimed but
     * that this layer believes is not there - the next caller would try to
     * claim the same pins and be refused for a reason that is not true.
     */
    s_spi[idx].up = true;
    s_spi[idx].slave = slave;
    s_spi[idx].cs = slave ? cs : AG_PIN_NONE;

    if (cfg->drive >= 0) {
        const ag_err_t drv = ag_port_spi_set_drive(bus, cfg->drive);
        if (drv != AG_OK) {
            /*
             * Worth saying out loud rather than swallowing: at this project's
             * own wiring the default strength makes a link look broken in a way
             * nothing else explains, so a board that asked for the weakest
             * setting and did not get it has an unexplained link rather than a
             * bus that failed to open.  See docs/plans/cluster.md.
             */
            ag_log(AG_LOG_WARN, "io", "spi%d drive %d not applied: %d", bus,
                   (int)cfg->drive, (int)drv);
        }
    }

    ag_log(AG_LOG_INFO, "io",
           "spi%d up as %s on sck=%d mosi=%d miso=%d at %u kHz", bus,
           slave ? "slave" : "master", cfg->sck, cfg->mosi, cfg->miso,
           (unsigned)cfg->khz);
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
/*
 * The clock for one chip on a bus that has several.  Bringing the bus up here
 * as well, because a speed set before the bus exists would be forgotten: the
 * bus comes up on first use, and this is a first use.
 */
static ag_err_t io_spi_config(int bus, int cs, uint32_t khz)
{
    if (khz == 0) {
        return -AG_EINVAL;
    }

    lock();
    ag_err_t err = spi_bring_up(bus, false, AG_PIN_NONE);
    if (err == AG_OK) {
        err = spi_take_cs(bus, cs);
    }
    if (err == AG_OK) {
        err = ag_port_spi_set_khz(bus, cs, khz);
    }
    unlock();
    return err;
}

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
    ag_err_t err = spi_bring_up(bus, false, AG_PIN_NONE);
    if (err == AG_OK) {
        err = spi_take_cs(bus, cs);
    }
    if (err == AG_OK) {
        err = ag_port_spi_xfer(bus, cs, tx, rx, len);
    }
    unlock();
    return err;
}

/*
 * The bus as a slave.  Same pins out of BOARD.CFG as the master's, same lazy
 * bring-up and the same ownership: the select is claimed here as well, because
 * whoever drives it decides whether this board answers at all.
 *
 * `khz` is what the caller expects the peer to clock at and is not applied -
 * the master on the other end owns the clock.  It is checked for being a real
 * rate so that a caller who passed zero, which is nobody's clock, hears about
 * it here rather than at the far end of a link that never moves.
 */
static ag_err_t io_spi_slave_open(int bus, int cs, uint32_t khz)
{
    if (cs < 0 || khz == 0) {
        return -AG_EINVAL;
    }

    lock();
    const ag_err_t err = spi_bring_up(bus, true, cs);
    unlock();
    return err;
}

/*
 * One block, both directions at once, ending when the peer lets the select go.
 *
 * The bus has to have been opened as a slave already, and that is not a
 * formality: the select pin came in with that call and there is nowhere on a
 * transfer to put another one, so a transfer with no slave open has nothing to
 * answer to.  -AG_ENODEV is that answer, and it is different from the timeout
 * below - one is a call made in the wrong order, the other a peer that did not
 * clock.
 */
static ag_err_t io_spi_slave_xfer(int bus, const void *tx, void *rx, size_t len,
                                  uint32_t timeout_ms)
{
    if (len == 0 || len > AG_PORT_SPI_MAX_XFER) {
        return -AG_EINVAL;
    }
    if (tx == NULL && rx == NULL) {
        return -AG_EINVAL;
    }

    lock();
    const int idx = spi_index(bus);
    ag_err_t  err;
    if (idx < 0 || idx >= AG_SPI_BUSES) {
        err = -AG_ERANGE;
    } else if (!s_spi[idx].up || !s_spi[idx].slave) {
        err = -AG_ENODEV;
    } else {
        /*
         * The lock is the kernel's io lock and the wait can be as long as the
         * caller asked for.  That is deliberate and the same shape as the
         * master path: the port serialises one transaction at a time on the
         * bus, and this layer serialises the pin bookkeeping that a second
         * caller would otherwise walk while this one is blocked.
         */
        err = ag_port_spi_slave_xfer(bus, tx, rx, len, timeout_ms);
    }
    unlock();
    return err;
}

/* ---------------------------------------------------------------------- */
/* The slave link service                                                 */
/* ---------------------------------------------------------------------- */

/*
 * The other half of the slave story (ABI 0.57).
 *
 * spi_slave_xfer above is one transaction and its caller waits for it, which is
 * all a slave is and not enough to carry a link: between one transfer
 * completing and the next being queued there is nobody listening, and on this
 * wire the peer's select pulse that falls in that window is lost.  So the queue
 * is kept armed by a task of the kernel's, at a priority the kernel chooses -
 * src/net/spilink.c has the measurement and the numbers - and what is handed to
 * the upper layer is bytes.
 *
 * The bus comes up through exactly the path the master takes: pins and clock
 * from BOARD.CFG's spi<bus> section, the drive strength applied in
 * spi_bring_up, the select claimed with the rest.  There is no second pin
 * parser here or there, and that is deliberate - a link whose pins are read
 * twice is a link that can be wired two ways.
 */
static ag_err_t uart_bring_up(int port, const ag_port_uart_cfg_t *cfg);

static int32_t io_uart_dma_write(int port, const void *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }

    /*
     * Same shape as the ordinary write, and the same reason for the lock being
     * where it is: bring-up under it, the transfer outside.  A DMA transfer is
     * even longer than a buffered write - it returns when the last byte has
     * left the wire - so holding the io lock across it would be the same
     * silent board, only for longer.
     */
    lock();
    const ag_err_t err = uart_bring_up(port, NULL);
    unlock();
    if (err != AG_OK) {
        return err;
    }
    return ag_port_uart_dma_write(port, buf, len);
}

static ag_err_t io_uart_tx_wait(int port, uint32_t timeout_ms)
{
    if (!ag_port_uart_is_open(port)) {
        return AG_OK; /* nothing of ours is in a port that is not open */
    }
    return ag_port_uart_tx_wait(port, timeout_ms);
}

static int32_t io_uart_dma_submit(int port, const void *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }
    lock();
    const ag_err_t err = uart_bring_up(port, NULL);
    unlock();
    if (err != AG_OK) {
        return err;
    }
    return ag_port_uart_dma_submit(port, buf, len);
}

static ag_err_t io_uart_dma_drain(int port)
{
    if (!ag_port_uart_is_open(port)) {
        return AG_OK;
    }
    return ag_port_uart_dma_drain(port);
}

static bool io_uart_dma_ready(int port)
{
    lock();
    const ag_err_t err = uart_bring_up(port, NULL);
    unlock();
    if (err != AG_OK) {
        return false;
    }
    return ag_port_uart_dma_ready(port);
}

/*
 * The framed link over a UART (ABI 0.61): the port up at the baud asked for,
 * with BOARD.CFG's pins, exactly as uart_config brings it up, and its DMA -
 * then the link's own tasks own it until close.
 */
static ag_err_t io_uart_link_open(int port, uint32_t baud, uint32_t frame,
                                  uint32_t flags)
{
    if (baud == 0u) {
        return -AG_EINVAL;
    }
    const ag_port_uart_cfg_t cfg = {
        .baud = baud, .data_bits = 8, .parity = 0, .stop_bits = 1};
    /*
     * Through the driver, its receive ring holds what the other end may have
     * in flight - this end's room, three frames - and not the 8 KB a reader
     * with no flow control needs: the link's own task empties it at once.
     */
    const bool     dma = (flags & AG_UARTLINK_DMA) != 0u;
    const uint32_t f = ag_uartlink_frame(frame);
    lock();
    ag_err_t err = dma ? uart_bring_up_sized(port, &cfg, AG_IO_UART_DMA_RX_BUF,
                                             AG_IO_UART_DMA_TX_BUF)
                       : uart_bring_up_sized(port, &cfg, 4u * f,
                                             AG_IO_UART_TX_BUF);
    unlock();
    if (err == AG_OK && dma &&
        !ag_port_uart_dma_ready(port)) {
        err = -AG_ENOTSUP; /* asked for DMA on a port that has none */
    }
    if (err == AG_OK) {
        err = ag_uartlink_open(port, frame, flags);
    }
    if (err != AG_OK) {
        ag_log(AG_LOG_WARN, "io", "uart%d link open failed: %d", port,
               (int)err);
    }
    return err;
}

static ag_err_t io_uart_link_close(int port)
{
    return ag_uartlink_close(port, NULL);
}

static int32_t io_uart_dma_read(int port, void *buf, size_t len,
                                uint32_t timeout_ms)
{
    lock();
    const ag_err_t err = uart_bring_up(port, NULL);
    unlock();
    if (err != AG_OK) {
        return err;
    }
    return ag_port_uart_dma_read(port, buf, len, timeout_ms);
}

static ag_err_t io_spi_link_open2(int bus, int cs, uint32_t khz,
                                  uint32_t frame, uint32_t flags)
{
    if ((flags & AG_SPILINK_MASTER) == 0u) {
        /* the slave end: the same bring-up as the raw link */
        if (cs < 0 || khz == 0) {
            return -AG_EINVAL;
        }
        lock();
        ag_err_t err = spi_bring_up(bus, true, cs);
        if (err == AG_OK) {
            const int idx = spi_index(bus);
            err = ag_spilink_open2(bus, cs, khz, frame, flags,
                                   ag_board()->spi[idx].sck,
                                   ag_board()->spi[idx].mosi,
                                   ag_board()->spi[idx].miso);
        }
        unlock();
        if (err != AG_OK) {
            ag_log(AG_LOG_WARN, "io", "spi%d link open failed: %d", bus,
                   (int)err);
        }
        return err;
    }
    if (cs < 0 || khz == 0) {
        return -AG_EINVAL;
    }
    lock();
    ag_err_t err = spi_bring_up(bus, false, AG_PIN_NONE);
    if (err == AG_OK) {
        err = spi_take_cs(bus, cs);
    }
    if (err == AG_OK) {
        err = ag_port_spi_set_khz(bus, cs, khz);
    }
    if (err == AG_OK) {
        const int idx = spi_index(bus);
        err = ag_spilink_open2(bus, cs, khz, frame, flags,
                               ag_board()->spi[idx].sck,
                               ag_board()->spi[idx].mosi,
                               ag_board()->spi[idx].miso);
    }
    unlock();
    if (err != AG_OK) {
        ag_log(AG_LOG_WARN, "io", "spi%d master link open failed: %d", bus,
               (int)err);
    }
    return err;
}

static ag_err_t io_spi_link_open(int bus, int cs, uint32_t khz, uint32_t block)
{
    if (cs < 0 || khz == 0) {
        return -AG_EINVAL;
    }

    lock();
    ag_err_t err = spi_bring_up(bus, true, cs);
    if (err == AG_OK) {
        /*
         * The pins go with it so the service can build the interface again if
         * a recovery ever finds the port's bus closed - see slave_rebuild in
         * src/net/spilink.c.  They come from the same BOARD.CFG section
         * spi_bring_up just read, so this is not a second reading of the
         * configuration.
         */
        const int idx = spi_index(bus);
        err = ag_spilink_open(bus, cs, khz, block, ag_board()->spi[idx].sck,
                              ag_board()->spi[idx].mosi,
                              ag_board()->spi[idx].miso);
    }
    unlock();

    if (err != AG_OK) {
        /* Said out loud: the caller is a driver that is about to decide whether
         * it can work at all, and -AG_ENOMEM here means it asked for a block
         * whose eight copies do not fit. */
        ag_log(AG_LOG_WARN, "io", "spi%d link open failed: %d", bus, (int)err);
    }
    return err;
}

/*
 * Stops the service, waits for it, and gives this layer's hold on the bus back.
 *
 * The wait is the requirement and not politeness: a driver unloads on other
 * people's stacks, and a service task left running behind a freed image would
 * hold port transactions and its own memory for the life of the boot.  The
 * counters the service kept are printed here, because the service itself may
 * not log - one of the paths that stops it runs with the device registry lock
 * held, and the console lock sits underneath that one.
 *
 * The port has no way to close a slave bus, so the peripheral and the pins it
 * reserved stay exactly as a master bus's do once spi_xfer has used one.  What
 * goes back is this layer's record of the bus being held, which is what a later
 * open has to find false before it brings the bus up again.
 */
static ag_err_t io_spi_link_close(int bus)
{
    ag_spilink_stats_t st;
    memset(&st, 0, sizeof(st));

    lock();
    const ag_err_t err = ag_spilink_close(bus, &st);
    const int      idx = spi_index(bus);
    if (err == AG_OK && idx >= 0 && idx < AG_SPI_BUSES && s_spi[idx].up &&
        s_spi[idx].slave) {
        s_spi[idx].up = false;
        s_spi[idx].slave = false;
        s_spi[idx].cs = AG_PIN_NONE;
    }
    unlock();

    if (err == AG_OK) {
        ag_log(AG_LOG_INFO, "io",
               "spi%d link closed: %u blocks clocked, %u carried bytes, "
               "%u unarmed, %u dropped, %u errors (last %d)",
               bus, (unsigned)st.blocks_clocked, (unsigned)st.blocks_data,
               (unsigned)st.blocks_unarmed, (unsigned)st.bytes_dropped,
               (unsigned)st.errors, (int)st.last_error);
        if (st.blocks_unarmed > 0u) {
            /* The invariant broke: a block was completed with nothing of ours
             * armed, so something arrived while this end was not listening. */
            ag_log(AG_LOG_WARN, "io",
                   "spi%d link: %u block(s) completed with nothing armed", bus,
                   (unsigned)st.blocks_unarmed);
        }
    } else if (st.task_stuck) {
        ag_log(AG_LOG_ERROR, "io",
               "spi%d link: the service task did not stop; the bus stays open",
               bus);
    }
    return err;
}

/* Bytes in and out of the rings the service keeps.  Neither takes the io lock:
 * a read waits, and the rings have a lock of their own. */
static int32_t io_spi_link_write(int bus, const void *buf, size_t len)
{
    return ag_spilink_write(bus, buf, len);
}

static int32_t io_spi_link_read(int bus, void *buf, size_t len,
                                uint32_t timeout_ms)
{
    return ag_spilink_read(bus, buf, len, timeout_ms);
}

/*
 * The counters, while the link is alive.
 *
 * Not a convenience.  The close line was the instrument this service was built
 * with, and the first hardware round in which the slave wedged printed nothing
 * at all, because the board never got far enough to close the link.  A driver
 * can read these from its own loop and the shell can ask; the one rate-limited
 * line about a fault is printed by spilink.c from here, and not from the
 * service task, which must never log - a device revoke stops it while holding
 * the device registry lock, and the console lock sits under that one.
 */
static ag_err_t io_spi_link_stats(int bus, ag_spilink_stats_t *out)
{
    return ag_spilink_stats(bus, out);
}

/* ---------------------------------------------------------------------- */
/* UART                                                                   */
/* ---------------------------------------------------------------------- */

static bool s_uart_up[AG_UART_PORTS];

bool ag_io_uart_claimed(int port)
{
    if (port < 0 || port >= AG_UART_PORTS) {
        return false;
    }
    return s_uart_up[port];
}

/*
 * What was last applied to each port, and why it is worth remembering.
 *
 * ag_port_uart_config reprograms the divider and the line, and on the original
 * ESP32 and the S3 it first waits for the transmit shift register to empty.
 * Neither is free and neither is harmless in the middle of a stream.  Calling
 * it from every read and every write - which is what happened here - had two
 * costs: the wait landed between the bytes of a transfer, and a baud rate the
 * caller had set with uart_config was undone by its own next write, because a
 * write brings the port up with `want` NULL and NULL means "the rate in
 * BOARD.CFG".  A port that snaps back to 115200 the moment you use it is not a
 * port you can measure a wire with.
 *
 * So the rate is applied when it is asked for and when it changes, and an
 * ordinary read or write takes the port as it stands.
 */
static ag_port_uart_cfg_t s_uart_cfg[AG_UART_PORTS];

static bool uart_cfg_same(const ag_port_uart_cfg_t *a,
                          const ag_port_uart_cfg_t *b)
{
    return a->baud == b->baud && a->data_bits == b->data_bits &&
           a->parity == b->parity && a->stop_bits == b->stop_bits;
}

static ag_err_t uart_bring_up(int port, const ag_port_uart_cfg_t *want)
{
    return uart_bring_up_sized(port, want, AG_IO_UART_RX_BUF,
                               AG_IO_UART_TX_BUF);
}

static ag_err_t uart_bring_up_sized(int port, const ag_port_uart_cfg_t *want,
                                    uint32_t rx_bytes, uint32_t tx_bytes)
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

        /* The port applies uart_cfg itself as the last thing open() does, so
         * there is nothing to apply again here. */
        const ag_err_t rc =
            ag_port_uart_open(port, &uart_cfg, rx_bytes, tx_bytes);
        if (rc != AG_OK) {
            return rc;
        }
        ag_err_t pe = ag_port_uart_pins(port, cfg->tx, cfg->rx);
        if (pe != AG_OK) {
            return pe;
        }
        s_uart_up[port] = true;
        s_uart_cfg[port] = uart_cfg;

        /* Pins come from BOARD.CFG and do not change while the port is up. */
        return ag_port_uart_pins(port, cfg->tx, cfg->rx);
    }

    /* Up already: a read or a write (want == NULL) takes it as it stands. */
    if (want == NULL || uart_cfg_same(&s_uart_cfg[port], &uart_cfg)) {
        return AG_OK;
    }

    const ag_err_t err = ag_port_uart_config(port, &uart_cfg);
    if (err == AG_OK) {
        s_uart_cfg[port] = uart_cfg;
    }
    return AG_OK;
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
    .spi_config = io_spi_config,
    .spi_slave_open = io_spi_slave_open,
    .spi_slave_xfer = io_spi_slave_xfer,
    .spi_link_open = io_spi_link_open,
    .uart_tx_wait = io_uart_tx_wait,
    .uart_dma_ready = io_uart_dma_ready,
    .uart_dma_write = io_uart_dma_write,
    .uart_dma_submit = io_uart_dma_submit,
    .uart_dma_drain = io_uart_dma_drain,
    .uart_dma_read = io_uart_dma_read,
    .spi_link_close = io_spi_link_close,
    .spi_link_write = io_spi_link_write,
    .spi_link_read = io_spi_link_read,
    .spi_link_stats = io_spi_link_stats,
    .spi_link_open2 = io_spi_link_open2,
    .spi_link_write_wait = ag_spilink_write_wait,
    .spi_link_send = ag_spilink_send,
    .spi_link_recv = ag_spilink_recv,
    .uart_link_open = io_uart_link_open,
    .uart_link_close = io_uart_link_close,
    .uart_link_stats = ag_uartlink_stats,
    .uart_link_write = ag_uartlink_write,
    .uart_link_write_wait = ag_uartlink_write_wait,
    .uart_link_read = ag_uartlink_read,
    .uart_link_send = ag_uartlink_send,
    .uart_link_recv = ag_uartlink_recv,
    .spi_link_send2 = ag_spilink_send2,
    .spi_link_recv2 = ag_spilink_recv2,
    .uart_link_send2 = ag_uartlink_send2,
    .uart_link_recv2 = ag_uartlink_recv2,
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
    /* Skipping the CONSOLE's port rather than port zero, for the reason
     * written up at the configuration loop in boot/board.c: on a board whose
     * console is the chip's own USB, UART0 is an ordinary free port and a
     * board that configured it would not have seen it listed in `dev`. */
    for (int i = 0; i < AG_UART_PORTS; i++) {
        if (i == AG_PORT_UART_CONSOLE) {
            continue;
        }
        const ag_board_uart_t *cfg = &ag_board()->uart[i];
        if (cfg->tx >= 0 || cfg->rx >= 0) {
            register_bus(k_uart_names[i], "uart", AG_DEV_BUS);
        }
    }
}
