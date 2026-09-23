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
 *   ag_err_t ag_port_spi_set_khz(int bus, int cs, uint32_t khz)
 *       The speed for one chip on the bus, overriding the bus default.  A bus
 *       has one set of wires and several chips, and they rarely agree: a panel
 *       takes 40 MHz and the touch controller beside it on the same three
 *       wires will not answer above two.
 *   ag_err_t ag_port_spi_xfer(int bus, int cs, const void *tx, void *rx,
 *                             size_t len)
 *
 *   ag_err_t ag_port_spi_slave_open(int bus, int sck, int mosi, int miso,
 *                                   int cs, uint32_t khz)
 *   ag_err_t ag_port_spi_slave_xfer(int bus, const void *tx, void *rx,
 *                                   size_t len, uint32_t timeout_ms)
 *       The same three wires from the other end.  A slave does not hold the
 *       clock: the peer clocks it, and `cs` is the select INPUT that peer
 *       drives rather than a pin this end toggles.  `khz` is what the caller
 *       expects the peer to run at - it is recorded and not applied, because
 *       there is no clock here to set - and the pins are the bus's, as for the
 *       master.  slave_xfer therefore blocks until the peer has clocked the
 *       block or `timeout_ms` has passed, and a timeout is reported as one - a
 *       slave that returned success for a block nobody clocked would hand its
 *       caller a frame assembled from nothing.  It is one slot of the same pool
 *       arm and reap use, so it neither sees nor disturbs a pipelined caller's
 *       blocks, and nothing above it has to change because the pool exists.
 *   ag_err_t ag_port_spi_slave_arm(int bus, const void *tx, void *rx,
 *                                  size_t len)
 *   ag_err_t ag_port_spi_slave_reap(int bus, int *slot, uint32_t *got, void **rx,
 *                                   uint32_t timeout_ms)
 *       The one-at-a-time transfer split in two, so that several blocks are on
 *       the wire at once.  arm queues one block and returns at once - it does
 *       not wait for the peer, and -AG_EAGAIN when every slot is busy is
 *       back-pressure rather than failure, answered by reaping.  reap waits for
 *       the next block the peer has clocked, names the slot whose buffers it
 *       used, and reports in `got` the bytes actually clocked - which is the
 *       number a one-at-a-time transfer reads and throws away.
 *
 *       Why the pair exists is worth stating where it is implemented: with one
 *       block armed at a time, the moment between collecting it and arming the
 *       next has nothing listening, and a select pulse that falls inside it is
 *       lost in silence.  A master that clocks back to back - this project's
 *       own link clocks a block every 305 us - outruns that moment, and the
 *       sender cannot tell, because it is clocking into a chip that is not
 *       listening.  Several armed blocks close the gap in hardware: the
 *       driver's interrupt picks up the next armed block on the select pulse
 *       itself, so there is no software in the loop and no window to lose.
 *   ag_err_t ag_port_spi_slave_reset(int bus)
 *       Give the pool back when the caller has nothing left to reap.
 *
 *       A transaction the driver has already handed to the hardware cannot be
 *       cancelled - ESP-IDF has no call for it - so a peer that stops clocking
 *       leaves armed blocks that never complete and are never reaped, and a
 *       pool with every slot in that state answers -AG_EAGAIN for ever.  This
 *       is the way out: the interface is thrown away and built again from the
 *       same pins, the same select and the same depth, and every slot is
 *       released.
 *
 *       THE GUARANTEE: once it returns AG_OK, every slot of the pool is free
 *       and arm takes a block on the very next call - whatever the peer did or
 *       did not clock, and without waiting for it.  The work is local: no
 *       clock, no peer, nothing that blocks on the wire, so it is bounded by
 *       the port's own teardown and re-initialisation.  Bytes that were in
 *       flight are gone, and the protocol above has to resynchronise; what is
 *       guaranteed here is the pool, not the wire.
 *
 *       -AG_EBUSY when another caller is in the way: a transfer of the
 *       one-at-a-time kind on this bus, or a reaper inside the driver's result
 *       call - freeing the queues under a blocked receiver is not something to
 *       do to a caller, so it is refused rather than waited for.  A reap that
 *       is merely waiting its turn is not in the way and is safe.  -AG_ENODEV
 *       if the bus was never opened, -AG_EINVAL if it is the end that holds the
 *       clock.  If the interface cannot be built again the bus is left closed -
 *       arm then answers -AG_ENODEV - and opening it as a slave once more is
 *       what brings it back.
 *
 *       For anybody else on the bus the rebuild is invisible: arm sees the pool
 *       either before it or after it - answered -AG_EAGAIN if it arrives in the
 *       middle, like a full pool - and a reap that is waiting its turn finds a
 *       fresh interface and nothing of the blocks that were abandoned.
 *   ag_err_t ag_port_spi_set_drive(int bus, int cap)
 *       Drive strength of the bus's pins, 0..3 = GPIO_DRIVE_CAP_n, -1 = leave
 *       the chip's default alone.  It is a bus setting and not a nicety: on
 *       this project's own wiring the weakest setting is the difference between
 *       a link that carries frames and one that looks broken - see the section
 *       on pad drive in docs/plans/cluster.md.
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
 * - A bus is one end or the other for its whole life, and the two are not
 *   interchangeable: a slave bus has no device table and no chip selects to
 *   toggle, a master bus has nothing to be clocked by.  A call that does not
 *   match how the bus was opened is refused - -AG_EBUSY for an open that
 *   disagrees with the role already taken, -AG_EINVAL for a transfer - rather
 *   than answered with a transfer that reaches no chip.
 * - spi_slave_xfer is full duplex like spi_xfer, so `tx` and `rx` are the same
 *   length and `len` is at most AG_PORT_SPI_MAX_XFER.  The peer ends the block
 *   by releasing the select, so `len` is a maximum and not a promise: whatever
 *   the peer did not clock is written back as ZEROS, never as the tail of an
 *   earlier block.  A caller may therefore hand the whole buffer on without
 *   knowing the length that arrived - it reads its own frame out of a
 *   zero-padded block - and a caller that must know what the peer sent has to
 *   frame it itself.  On a timeout nothing is written to `rx` at all: bytes
 *   nobody clocked are not a frame.
 * - Arm and reap are the same transfer with the waiting taken out.  The buffers
 *   a block is armed with belong to the port until that block is reaped, and a
 *   timeout does not give them back: the peer may clock the block later, and
 *   when it does the port collects it and hands it to the caller that reaps
 *   that slot - bytes the peer really sent, into the memory they were armed
 *   with.  So a timed-out arm is not a lost block, it is a block still on the
 *   wire, and its caller must reap the slot before touching either buffer
 *   again.  A pool with every slot armed and none reaped stops arming, which is
 *   the caller's problem to keep up with - and slave_reset is how a caller that
 *   has lost that argument gets its slots back, at the price of whatever was on
 *   the wire.
 * - reap names the slot, and the slot is what says which armed block the bytes
 *   belong to; blocks come back in the order they were armed.  The port hands
 *   each completed block to whoever reaps, so one reaper per bus is the
 *   arrangement - two would take each other's blocks, and the slot number is
 *   what would tell them so.
 * - arm uses the caller's memory as the DMA engine's buffer, which is why
 *   several armed blocks cost nothing: no copy between arming and the wire, and
 *   none between the wire and the reap.  It therefore refuses memory the engine
 *   cannot reach, a receive buffer that does not start on a word boundary, and
 *   a receive length that is not a whole number of words - the engine writes
 *   words, and a buffer three bytes short of the block handed to it would be
 *   written past its end.  A caller that cannot promise any of that uses
 *   spi_slave_xfer, which copies through buffers the port owns.
 * - One block at a time leaves a window in which nothing is listening: a select
 *   pulse that falls entirely inside it is lost - silently, and without
 *   corrupting, delaying or duplicating the block after it.  Arming several
 *   blocks ahead removes the window rather than shortening it, because the next
 *   block is already in the driver when the select pulse arrives.  The window
 *   still exists for a caller that arms one block at a time, which is what
 *   spi_slave_xfer is, and it is the caller's choice to make.
 * - `tx` NULL means the block carries zeros, on all three slave calls: a peer
 *   that clocks a slave has to be answered with something, and "nothing
 *   decided" is not one of the answers.
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
ag_err_t ag_port_spi_set_khz(int bus, int cs, uint32_t khz);
ag_err_t ag_port_spi_xfer(int bus, int cs, const void *tx, void *rx,
                          size_t len);
/*
 * A master transfer of the caller's own DMA-capable, word-aligned buffers, in
 * one transaction, with the caller's task BLOCKED rather than polling for the
 * length of it.  For a service that clocks frame after frame: spi_xfer copies
 * a long duplex transfer through staging and spins in the polling call while
 * it crosses, which for 4 KB at 20 MHz is 1.6 ms of a core doing nothing.
 */
ag_err_t ag_port_spi_xfer_dma(int bus, int cs, const void *tx, void *rx,
                              size_t len);

ag_err_t ag_port_spi_slave_open(int bus, int sck, int mosi, int miso, int cs,
                                uint32_t khz);
ag_err_t ag_port_spi_slave_xfer(int bus, const void *tx, void *rx, size_t len,
                                uint32_t timeout_ms);
ag_err_t ag_port_spi_slave_arm(int bus, const void *tx, void *rx, size_t len);
ag_err_t ag_port_spi_slave_reap(int bus, int *slot, uint32_t *got, void **rx,
                                uint32_t timeout_ms);
ag_err_t ag_port_spi_slave_reset(int bus);
ag_err_t ag_port_spi_set_drive(int bus, int cap);

ag_err_t ag_port_pwm_config(int channel, int pin, uint32_t hz, uint8_t bits);
ag_err_t ag_port_pwm_set(int channel, uint32_t duty);
void     ag_port_pwm_stop(int channel);

int32_t ag_port_adc_read(int channel);

#include <argon/port/impl/io.h>

#endif /* ARGON_PORT_IO_H */
