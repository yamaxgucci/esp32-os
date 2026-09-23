/*
 * ArgonOS port: ESP-IDF - pins and buses.
 *
 * Registers and nothing else.  Who owns a pin, where a bus gets its pins from,
 * and what happens to both when a process dies are all in src/dev/io.c, which
 * is the same code on every port.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/io.h>

#include <string.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h" /* xTaskGetTickCount: the deadline a reap works to */
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include "esp_log.h"
#include "esp_memory_utils.h"

/*
 * The copy buffer for callers whose memory the DMA engine cannot read.
 *
 * A kilobyte each way, and deliberately not the same as
 * AG_PORT_SPI_MAX_XFER: making them equal meant that raising the transfer
 * limit - which is what a display wants - also raised the fixed cost of every
 * bus that comes up, on a board where eight kilobytes is real money.
 */
#define AG_PORT_SPI_BOUNCE 1024
#include "hal/gpio_types.h"

#if AG_PORT_HAS_ADC
#include "esp_adc/adc_oneshot.h"
#endif

#include <argon/port/mem.h>

#define AG_PORT_I2C_BUSES SOC_I2C_NUM
/* Indexed by the chip's own bus number, which is what the caller passes. */
#define AG_PORT_SPI_SLOTS (SOC_SPI_PERIPH_NUM + 1)
#define AG_PORT_SPI_HOST_OF(chip) ((chip) - 1)

/*
 * Every ESP-IDF failure becomes one of ours, because an application cannot do
 * anything with an esp_err_t and the ABI promises it will never see one.
 */
static ag_err_t from_esp(esp_err_t err)
{
    switch (err) {
    case ESP_OK:                 return AG_OK;
    case ESP_ERR_INVALID_ARG:    return -AG_EINVAL;
    case ESP_ERR_INVALID_STATE:  return -AG_EBUSY;
    case ESP_ERR_NO_MEM:         return -AG_ENOMEM;
    case ESP_ERR_TIMEOUT:        return -AG_ETIMEDOUT;
    case ESP_ERR_NOT_FOUND:      return -AG_ENODEV;
    case ESP_ERR_NOT_SUPPORTED:  return -AG_ENOTSUP;
    default:                     return -AG_EIO;
    }
}

/* ---------------------------------------------------------------------- */
/* What the machine itself is using                                       */
/* ---------------------------------------------------------------------- */

unsigned ag_port_reserved_pins(const ag_port_pin_range_t **out)
{
    static const ag_port_pin_range_t k_ranges[] = {
#if CONFIG_IDF_TARGET_ESP32S3
        /* SPI0/1 to the flash chip: 26 SPICS1, 27 SPIHD, 28 SPIWP, 29 SPICS0,
         * 30 SPICLK, 31 SPIQ, 32 SPID. */
        {26, 32, "flash"},
#if CONFIG_SPIRAM_MODE_OCT
        /* Octal PSRAM takes four more data lines and its own chip select. */
        {33, 37, "psram"},
#endif
#elif CONFIG_IDF_TARGET_ESP32
        /*
         * SPI0/1 to the flash chip: 6 SPICLK, 7 SPIQ, 8 SPID, 9 SPIHD,
         * 10 SPIWP, 11 SPICS0.  On a WROOM module these are inside the can and
         * a single output configured on one of them stops the chip fetching
         * instructions - the failure looks like a board that died halfway
         * through printing a line, with nothing in the log to say why.
         *
         * Boards using the same module with octal PSRAM take 16 and 17 as well;
         * this project has no such board, so nothing here pretends to know.
         */
        {6, 11, "flash"},
#elif CONFIG_IDF_TARGET_ESP32C6
        /*
         * SPI0/1 to the flash chip: 24 SPICS0, 25 SPIQ, 26 SPIWP, 27 VDD_SPI,
         * 28 SPIHD, 29 SPICLK, 30 SPID (soc/spi_pins.h).  27 is not a signal
         * at all - it is the regulator output the flash is powered from - and
         * it is inside the range on purpose: driving it is the same dead board
         * as driving the clock.
         */
        {24, 30, "flash"},
        /*
         * The USB pins, and on this board they are the console: there is no
         * bridge chip, the Type-C socket goes to the part itself, and taking
         * either of these pins ends the conversation with no way to say so.
         * Named here rather than left to the console's own two lines because
         * they are the machine's whatever the console is set to.
         */
        {12, 13, "usb"},
#else
        {0, -1, NULL}, /* nothing known for this target */
#endif
    };

    *out = k_ranges;
    return (unsigned)(sizeof(k_ranges) / sizeof(k_ranges[0]));
}

/* ---------------------------------------------------------------------- */
/* GPIO                                                                   */
/* ---------------------------------------------------------------------- */

static bool s_isr_service;

ag_err_t ag_port_gpio_config(int pin, int mode)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << (unsigned)pin,
        .intr_type = GPIO_INTR_DISABLE,
    };

    switch (mode) {
    case AG_GPIO_IN:
        cfg.mode = GPIO_MODE_INPUT;
        break;
    /*
     * Outputs keep their input buffer on, so that reading a pin tells you what
     * is on the line.  For a push-pull output that is a free confirmation of
     * what was written; for an open-drain one it is the whole point, because
     * the line is only low when nobody is holding it up - which is how I2C and
     * 1-Wire are read at all.
     */
    case AG_GPIO_OUT:
        cfg.mode = GPIO_MODE_INPUT_OUTPUT;
        break;
    case AG_GPIO_OUT_OD:
        cfg.mode = GPIO_MODE_INPUT_OUTPUT_OD;
        break;
    case AG_GPIO_IN_PULLUP:
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        break;
    case AG_GPIO_IN_PULLDOWN:
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
        break;
    default:
        return -AG_EINVAL;
    }

    return from_esp(gpio_config(&cfg));
}

void ag_port_gpio_write(int pin, int level)
{
    (void)gpio_set_level((gpio_num_t)pin, (level != 0) ? 1 : 0);
}

int ag_port_gpio_read(int pin) { return gpio_get_level((gpio_num_t)pin); }

ag_err_t ag_port_gpio_isr_attach(int pin, int edge, ag_isr_fn fn, void *arg)
{
    if (!s_isr_service) {
        const esp_err_t installed = gpio_install_isr_service(0);
        if (installed != ESP_OK && installed != ESP_ERR_INVALID_STATE) {
            return from_esp(installed);
        }
        s_isr_service = true;
    }

    gpio_int_type_t type = GPIO_INTR_ANYEDGE;
    if (edge == AG_EDGE_RISING) {
        type = GPIO_INTR_POSEDGE;
    } else if (edge == AG_EDGE_FALLING) {
        type = GPIO_INTR_NEGEDGE;
    }

    ag_err_t err = from_esp(gpio_set_intr_type((gpio_num_t)pin, type));
    if (err == AG_OK) {
        err = from_esp(gpio_isr_handler_add((gpio_num_t)pin, fn, arg));
    }
    return err;
}

void ag_port_gpio_isr_detach(int pin)
{
    (void)gpio_isr_handler_remove((gpio_num_t)pin);
    (void)gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_DISABLE);
}

void ag_port_gpio_reset(int pin) { (void)gpio_reset_pin((gpio_num_t)pin); }

/* ---------------------------------------------------------------------- */
/* I2C                                                                    */
/* ---------------------------------------------------------------------- */

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint32_t                khz;
    uint8_t                 dev_addr;
    bool                    up;
} i2c_state_t;

static i2c_state_t s_i2c[AG_PORT_I2C_BUSES];

ag_err_t ag_port_i2c_open(int bus, int sda, int scl, uint32_t khz, bool pullups)
{
    if (bus < 0 || bus >= AG_PORT_I2C_BUSES) {
        return -AG_ERANGE;
    }
    if (s_i2c[bus].up) {
        return AG_OK;
    }

    const i2c_master_bus_config_t cfg = {
        .i2c_port = bus,
        .sda_io_num = (gpio_num_t)sda,
        .scl_io_num = (gpio_num_t)scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = pullups},
    };

    const ag_err_t err = from_esp(i2c_new_master_bus(&cfg, &s_i2c[bus].bus));
    if (err != AG_OK) {
        return err;
    }

    s_i2c[bus].khz = khz;
    s_i2c[bus].up = true;
    return AG_OK;
}

/*
 * One cached device handle per bus.  The ESP-IDF driver wants a handle per
 * address and this interface is per transaction, so something has to bridge the
 * two: a cache of one is enough because a driver talks to its own chip in a
 * run, and changing address costs one allocation rather than one per byte.
 */
static ag_err_t i2c_device(int bus, uint8_t addr, i2c_master_dev_handle_t *out)
{
    if (bus < 0 || bus >= AG_PORT_I2C_BUSES || !s_i2c[bus].up) {
        return -AG_ENODEV;
    }

    if (s_i2c[bus].dev != NULL && s_i2c[bus].dev_addr == addr) {
        *out = s_i2c[bus].dev;
        return AG_OK;
    }
    if (s_i2c[bus].dev != NULL) {
        (void)i2c_master_bus_rm_device(s_i2c[bus].dev);
        s_i2c[bus].dev = NULL;
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = s_i2c[bus].khz * 1000u,
    };
    const ag_err_t err = from_esp(
        i2c_master_bus_add_device(s_i2c[bus].bus, &cfg, &s_i2c[bus].dev));
    if (err != AG_OK) {
        return err;
    }

    s_i2c[bus].dev_addr = addr;
    *out = s_i2c[bus].dev;
    return AG_OK;
}

ag_err_t ag_port_i2c_write(int bus, uint8_t addr, const void *buf, size_t len,
                           uint32_t timeout_ms)
{
    i2c_master_dev_handle_t dev = NULL;
    const ag_err_t          err = i2c_device(bus, addr, &dev);
    return (err != AG_OK)
               ? err
               : from_esp(i2c_master_transmit(dev, buf, len, (int)timeout_ms));
}

ag_err_t ag_port_i2c_read(int bus, uint8_t addr, void *buf, size_t len,
                          uint32_t timeout_ms)
{
    i2c_master_dev_handle_t dev = NULL;
    const ag_err_t          err = i2c_device(bus, addr, &dev);
    return (err != AG_OK)
               ? err
               : from_esp(i2c_master_receive(dev, buf, len, (int)timeout_ms));
}

ag_err_t ag_port_i2c_wrrd(int bus, uint8_t addr, const void *wbuf, size_t wlen,
                          void *rbuf, size_t rlen, uint32_t timeout_ms)
{
    i2c_master_dev_handle_t dev = NULL;
    const ag_err_t          err = i2c_device(bus, addr, &dev);
    return (err != AG_OK) ? err
                          : from_esp(i2c_master_transmit_receive(
                                dev, wbuf, wlen, rbuf, rlen, (int)timeout_ms));
}

ag_err_t ag_port_i2c_probe(int bus, uint8_t addr, uint32_t timeout_ms)
{
    if (bus < 0 || bus >= AG_PORT_I2C_BUSES || !s_i2c[bus].up) {
        return -AG_ENODEV;
    }

    /*
     * Silence the driver for the duration.  An address with nothing on it times
     * out, and the driver logs that as an error - which is right when a driver
     * is talking to a chip it expects to be there, and wrong a hundred and
     * twelve times over when the whole point is to find out which addresses are
     * empty.  A scan of an empty bus filled the screen with ESP-IDF error text
     * and buried its own result.
     */
    const esp_log_level_t was = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    const esp_err_t rc = i2c_master_probe(s_i2c[bus].bus, addr,
                                          (int)timeout_ms);

    esp_log_level_set("i2c.master", was);

    if (rc == ESP_OK) {
        return AG_OK;
    }
    /* "Nothing here" is not "the bus is broken", and must not read as one. */
    return (rc == ESP_ERR_NOT_FOUND || rc == ESP_ERR_TIMEOUT) ? -AG_ENOENT
                                                              : from_esp(rc);
}

/* ---------------------------------------------------------------------- */
/* SPI                                                                    */
/* ---------------------------------------------------------------------- */

/*
 * Several chips on one bus, each with its own speed.
 *
 * There used to be one device per bus, swapped whenever the chip select
 * changed.  That is fine while a bus has one chip on it and wrong the moment
 * it has two, which is the normal case: this board hangs a 320x240 panel and a
 * touch controller off the same three wires, and the panel wants 40 MHz while
 * the controller will not answer above about two.  One speed for the bus means
 * either a display that takes a second a frame or a touchscreen that returns
 * noise.
 */
#define AG_PORT_SPI_DEVS 4

/*
 * How many slave transactions the driver may hold, which is also the size of
 * the port's pool of them.
 *
 * Not the one that would do for a call-at-a-time port.  The master on the other
 * end clocks a burst of blocks back to back, and the slave driver only serves
 * the next select pulse if a transaction is already queued: with a queue of one,
 * the second block arrives while the driver is still handing the first to
 * software, that pulse is missed, and those bytes are gone with no error
 * anywhere - a link that reports success and delivers garbage.  A queue of a
 * few lets the driver's interrupt pick up the next block on its own while the
 * caller is still copying the previous one, which is the whole reason the queue
 * exists.
 *
 * The pool is this number deep for the same reason, and it is what actually
 * keeps the queue fed: arming is a call that returns at once, so a caller can
 * have three blocks waiting in the driver while it works on a fourth, and the
 * window in which nothing is armed is not there to lose a select pulse in.
 * Measured on this project's own link before that: a master clocking a block
 * every 305 us had two blocks in three dropped, silently, because the slave was
 * between calls.
 */
#define AG_PORT_SPI_SLAVE_QUEUE 4

/*
 * Slot 0 is spi_slave_xfer's, for the life of the bus.
 *
 * It is the one slot that carries through the port's own staging buffers, and
 * keeping it out of the pipelined pool is what lets a caller of the old
 * one-at-a-time call and a task arming several blocks ahead share the bus:
 * neither can take the other's slot, and the pool never has to tell a caller
 * that the block it just armed came back through somebody else's buffers.  The
 * driver's queue is four deep, so the pipelined pair gets the other three.
 */
#define AG_PORT_SPI_SLAVE_XFER_SLOT 0

typedef struct {
    spi_device_handle_t dev;
    int                 cs;
    uint32_t            khz;
} spi_dev_t;

/*
 * One armed block: the descriptor the driver is handed, and the caller's
 * buffers it points at.
 *
 * The descriptors live here, in the bus, and not on a calling task's stack, and
 * that is not tidiness.  spi_slave_queue_trans stores a POINTER to the
 * descriptor it is given, and the driver's interrupt writes trans_len through
 * that pointer when the transaction ends (spi_slave.c: cur_trans.trans->trans_len
 * = spi_slave_hal_get_rcv_bitlen(hal)).  For a block nobody clocks, "when the
 * transaction ends" is whenever the peer gets round to it - seconds or minutes
 * after the call that queued it has given up and returned - so a descriptor on
 * a stack that has since been reused gets those four bytes written into
 * whatever is living there by then: another function's locals, a saved register,
 * or the return address above them.  One descriptor per slot, in the bus, is
 * the only arrangement with no such window in it.
 *
 * The buffer POINTERS are here for the same reason, one step out: the DMA
 * engine may still write the caller's receive buffer long after the caller
 * stopped waiting, so the port remembers whose memory each armed block borrowed
 * and does not let go of it until that block is reaped.  See the note on the
 * pool in argon/port/io.h.
 */
typedef struct {
    spi_slave_transaction_t desc;
    void       *rx;     /* the caller's, and where the wire's bytes go     */
    size_t      len;    /* bytes the caller framed                         */
    uint32_t    got;    /* bytes the peer clocked, once it is collected    */
    bool        armed;  /* queued with the driver, not yet collected       */
    bool        done;   /* collected, waiting for its owner to reap it     */
    bool        staged; /* carries through sbuf: spi_slave_xfer's own slot */
} spi_slot_t;

typedef struct {
    spi_dev_t devs[AG_PORT_SPI_DEVS];
    uint32_t  khz; /* the bus default, for a chip nobody set a speed for */
    uint8_t  *bounce; /* tx half then rx half, internal and DMA-able      */
    /*
     * One transfer at a time on this bus, whoever asks.
     *
     * A bus is shared hardware and this port keeps shared state for it: one
     * bounce buffer, and a table of devices built on demand.  Two tasks reach
     * both.  On the CYD they do it constantly: an application pushes pixels
     * to the panel from its own thread while the console task reads the touch
     * controller, and the two sit on SPI2 with a chip select each.
     *
     * Unserialised, a touch reading and a panel command share the bounce
     * buffer, and the panel is handed the touch's bytes: a window command
     * that is not the one that was meant, and a band of pixels landing
     * somewhere else on the glass.  That is what the dots on Maxim's screen
     * were - random places, gone at the next repaint - and why they appeared
     * when touch went pixel-precise: four times the traffic, four times the
     * chances to collide.
     *
     * Recursive, because a transfer too big for the bounce buffer chops
     * itself into transfers that come back through the front door - and
     * because dropping chip select between the halves of one command is
     * exactly what must not happen.
     */
    SemaphoreHandle_t lock;
    bool      up;
    /*
     * Which end of the clock this bus is, and it is fixed for the bus's life.
     *
     * The two roles do not share anything below this line: a master bus has a
     * device table, a bounce buffer and a speed; a slave bus is a pool of
     * armed blocks, each holding the memory the driver keeps until the peer
     * drops the select.  Letting one role's call through on the other's bus
     * would not transfer the wrong bytes, it would transfer none - so both are
     * refused where the mode is known.
     */
    bool      slave;
    int       sck, mosi, miso; /* the pins, for the drive strength later */
    int       slave_cs; /* the select the slave driver was handed */
    /*
     * The slave's buffers: transmit half then receive half, each
     * AG_PORT_SPI_MAX_XFER and both DMA-capable.
     *
     * Allocated once, at open, and never per call.  The slave driver keeps the
     * pointers from the moment a transaction is queued until the peer drops
     * the select, which is whenever the peer decides - so a buffer that went
     * out of scope, or was reused by the next call while that one is still
     * queued, is read by the DMA engine after the fact.  One pair for the life
     * of the bus is the only arrangement with no such window in it.
     *
     * This pair belongs to spi_slave_xfer, and to it alone: the pipelined pair
     * arms the caller's own memory instead, so that several blocks can be on
     * the wire at once without several copies of these.  A caller that cannot
     * hand the DMA engine its own memory uses xfer, which is why these are here
     * at all.
     */
    uint8_t  *sbuf;
    /*
     * The armed blocks - see spi_slot_t above - the short queue of collected
     * ones waiting to be handed over, and the two locks that keep them
     * straight.
     *
     * The pool lock is taken for the moment it takes to claim or hand over a
     * slot, and never across a wait: arming has to get in while a reaper is
     * waiting for the peer, or the queue would run dry exactly when the peer is
     * clocking at full rate.  The waiter lock is the opposite - it is held
     * across the wait in spi_slave_get_trans_result, because that call hands
     * back whichever block completed first rather than the one the caller asked
     * for, and two callers in it at once would each receive a block the other
     * armed.  With one waiter at a time, a block that arrives for somebody else
     * is put away in its slot for its owner.
     */
    spi_slot_t        slot[AG_PORT_SPI_SLAVE_QUEUE];
    uint8_t           done_q[AG_PORT_SPI_SLAVE_QUEUE]; /* collected, oldest first */
    uint8_t           done_n;
    SemaphoreHandle_t pool;
    SemaphoreHandle_t waiter;
    /*
     * What a block with no transmit buffer clocks.
     *
     * A slave is answered by the peer's clock and has to put something on the
     * line, so "tx is NULL" has always meant zeros here rather than "the line
     * is not driven".  The pool arms the caller's memory directly, and there is
     * no caller memory in this case, so it needs a page of its own: one for the
     * bus, allocated the first time somebody arms without a buffer - most
     * callers of a slave always have something to say, and the boards that
     * never do should not pay for it at open.
     */
    uint8_t  *zero;
    /*
     * Where a long MASTER write is staged so it can cross in one transaction.
     * Made the first time a bus sends something longer than the bounce buffer
     * and kept; a bus that never does pays nothing.  See the note in
     * spi_xfer_locked.
     */
    uint8_t  *staging;
    /*
     * The receive half for a LONG DUPLEX transfer.  Separate from the pair
     * above because that one is the write ping-pong and both halves of it can
     * be in flight; this is used inside one synchronous call and nowhere else.
     */
    uint8_t  *staging_rx;
    /*
     * The other half of the staging pair, and the transaction in flight.
     *
     * A long write is QUEUED rather than polled: the engine clocks it out of
     * one half while the caller fills the other, so the time on the wire and
     * the time spent building the next frame overlap instead of adding up.
     * Measured on the screen path at 40 MHz: 1.55 ms of the 2.55 ms a band
     * took was the CPU sitting in spi_device_polling_transmit doing nothing.
     *
     * Anything else that touches the bus reaps this first - see spi_drain.
     */
    spi_transaction_t tx_trans[2];
    int               tx_which;
    bool              tx_busy;
} spi_state_t;

static spi_state_t s_spi[AG_PORT_SPI_SLOTS];

static bool spi_valid(int bus)
{
    return bus > 0 && bus < AG_PORT_SPI_SLOTS;
}

ag_err_t ag_port_spi_open(int bus, int sck, int mosi, int miso, uint32_t khz)
{
    if (!spi_valid(bus)) {
        return -AG_ERANGE;
    }
    if (s_spi[bus].up) {
        /* Already open, and as the other end of the clock: the two roles are
         * not two settings of one bus, so this is a conflict and not a no-op. */
        return s_spi[bus].slave ? -AG_EBUSY : AG_OK;
    }

    const spi_bus_config_t cfg = {
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .sclk_io_num = sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = AG_PORT_SPI_MAX_XFER,
    };

    const esp_err_t rc = spi_bus_initialize(AG_PORT_SPI_HOST_OF(bus), &cfg,
                                            SPI_DMA_CH_AUTO);
    /* Somebody else may already own it - the card on SPI, for one. */
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        return from_esp(rc);
    }

    s_spi[bus].lock = xSemaphoreCreateRecursiveMutex();
    if (s_spi[bus].lock == NULL) {
        (void)spi_bus_free(AG_PORT_SPI_HOST_OF(bus));
        return -AG_ENOMEM;
    }
    s_spi[bus].bounce = ag_port_alloc(2u * AG_PORT_SPI_BOUNCE, AG_MEM_DMA);
    if (s_spi[bus].bounce == NULL) {
        vSemaphoreDelete(s_spi[bus].lock);
        s_spi[bus].lock = NULL;
        (void)spi_bus_free(AG_PORT_SPI_HOST_OF(bus));
        return -AG_ENOMEM;
    }

    s_spi[bus].khz = khz;
    s_spi[bus].sck = sck;
    s_spi[bus].mosi = mosi;
    s_spi[bus].miso = miso;
    s_spi[bus].slave = false;
    for (int i = 0; i < AG_PORT_SPI_DEVS; i++) {
        s_spi[bus].devs[i].cs = -1;
    }
    s_spi[bus].up = true;
    return AG_OK;
}

/*
 * The slave interface, built or rebuilt.
 *
 * One place knows how this interface is made, so that a reset cannot drift from
 * an open: the same pins, the same select INPUT, the same depth.  A slave has
 * no clock of its own and no device table - `cs` is a pin the peer drives, and
 * the whole interface is "put these bytes here, tell me when the peer has
 * clocked them" - and spi_slave_initialize claims the peripheral itself, which
 * is why a bus already up as a master fails at open with -AG_EBUSY instead of
 * being silently taken over.
 */
static ag_err_t spi_slave_if_init(int bus, int sck, int mosi, int miso, int cs)
{
    const spi_bus_config_t cfg = {
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .sclk_io_num = sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = AG_PORT_SPI_MAX_XFER,
    };
    const spi_slave_interface_config_t scfg = {
        .mode = 0,
        .spics_io_num = cs,
        .queue_size = AG_PORT_SPI_SLAVE_QUEUE,
        .flags = 0,
    };

    return from_esp(spi_slave_initialize(AG_PORT_SPI_HOST_OF(bus), &cfg, &scfg,
                                        SPI_DMA_CH_AUTO));
}

/*
 * The other end of the same wire.
 */
ag_err_t ag_port_spi_slave_open(int bus, int sck, int mosi, int miso, int cs,
                                uint32_t khz)
{
    if (!spi_valid(bus)) {
        return -AG_ERANGE;
    }
    /* A slave is told which pin selects it; there is no transfer call that
     * could carry one later, the driver wants it at init. */
    if (cs < 0) {
        return -AG_EINVAL;
    }
    if (s_spi[bus].up) {
        if (!s_spi[bus].slave) {
            return -AG_EBUSY;
        }
        /* The driver was handed this select at init and holds it; a second
         * open naming another pin would be answered "open" while the board
         * still answers on the first one. */
        return (s_spi[bus].slave_cs == cs) ? AG_OK : -AG_EBUSY;
    }

    const ag_err_t err = spi_slave_if_init(bus, sck, mosi, miso, cs);
    if (err != AG_OK) {
        return err;
    }

    /*
     * The port's own resources - the locks and the pool - are
     * made here if they are not there yet, and kept if they are.  Nothing
     * frees them but the end of the program, and that is deliberate: a reset
     * throws the DRIVER's interface away and builds it again, while these are
     * the bus's own and outlive it.  So an open that follows a reset that
     * could not rebuild the interface finds them waiting rather than making a
     * second pair of buffers - and a bus that keeps failing to come back does
     * not cost sixteen kilobytes an attempt.
     */
    const bool had_lock = (s_spi[bus].lock != NULL);
    const bool had_pool = (s_spi[bus].pool != NULL);
    const bool had_waiter = (s_spi[bus].waiter != NULL);

    if (!had_lock) {
        s_spi[bus].lock = xSemaphoreCreateRecursiveMutex();
    }
    if (s_spi[bus].lock == NULL) {
        (void)spi_slave_free(AG_PORT_SPI_HOST_OF(bus));
        return -AG_ENOMEM;
    }
    if (!had_pool) {
        s_spi[bus].pool = xSemaphoreCreateMutex();
    }
    if (!had_waiter) {
        s_spi[bus].waiter = xSemaphoreCreateMutex();
    }
    if (s_spi[bus].pool == NULL || s_spi[bus].waiter == NULL) {
        if (!had_waiter && s_spi[bus].waiter != NULL) {
            vSemaphoreDelete(s_spi[bus].waiter);
            s_spi[bus].waiter = NULL;
        }
        if (!had_pool && s_spi[bus].pool != NULL) {
            vSemaphoreDelete(s_spi[bus].pool);
            s_spi[bus].pool = NULL;
        }
        if (!had_lock) {
            vSemaphoreDelete(s_spi[bus].lock);
            s_spi[bus].lock = NULL;
        }
        (void)spi_slave_free(AG_PORT_SPI_HOST_OF(bus));
        return -AG_ENOMEM;
    }

    /* Nothing else in the pool has to be created: the descriptors are members
     * of the bus's own state and are alive for as long as it is. */
    for (int i = 0; i < AG_PORT_SPI_SLAVE_QUEUE; i++) {
        s_spi[bus].slot[i].armed = false;
        s_spi[bus].slot[i].done = false;
        s_spi[bus].slot[i].staged = (i == AG_PORT_SPI_SLAVE_XFER_SLOT);
        s_spi[bus].slot[i].got = 0;
    }
    s_spi[bus].done_n = 0;

    s_spi[bus].khz = khz;
    s_spi[bus].sck = sck;
    s_spi[bus].mosi = mosi;
    s_spi[bus].miso = miso;
    s_spi[bus].slave = true;
    s_spi[bus].slave_cs = cs;
    s_spi[bus].up = true;
    return AG_OK;
}

static ag_err_t spi_set_khz_locked(int bus, int cs, uint32_t khz);
static ag_err_t spi_drain(int bus, spi_device_handle_t dev);

ag_err_t ag_port_spi_set_khz(int bus, int cs, uint32_t khz)
{
    if (!spi_valid(bus)) {
        return -AG_ERANGE;
    }
    if (!s_spi[bus].up || s_spi[bus].lock == NULL) {
        return -AG_ENODEV;
    }
    /* There is no speed to set on a slave bus: the peer owns the clock. */
    if (s_spi[bus].slave) {
        return -AG_EINVAL;
    }
    /* The device table is the bus's, and this rebuilds entries in it while
     * another task may be half way through a transfer on one of them. */
    xSemaphoreTakeRecursive(s_spi[bus].lock, portMAX_DELAY);
    const ag_err_t err = spi_set_khz_locked(bus, cs, khz);
    xSemaphoreGiveRecursive(s_spi[bus].lock);
    return err;
}

static ag_err_t spi_set_khz_locked(int bus, int cs, uint32_t khz)
{
    for (int i = 0; i < AG_PORT_SPI_DEVS; i++) {
        spi_dev_t *d = &s_spi[bus].devs[i];
        if (d->cs != cs) {
            continue;
        }
        if (d->khz == khz) {
            return AG_OK;
        }
        /* Speed is fixed when a device is created, so change it by throwing
         * the device away; the next transfer builds one at the new speed. */
        if (d->dev != NULL) {
            /* A queued write still belongs to this device. */
            (void)spi_drain(bus, d->dev);
            (void)spi_bus_remove_device(d->dev);
        }
        d->dev = NULL;
        d->cs = -1;
        break;
    }

    for (int i = 0; i < AG_PORT_SPI_DEVS; i++) {
        spi_dev_t *d = &s_spi[bus].devs[i];
        if (d->cs < 0 && d->dev == NULL) {
            d->cs = cs;
            d->khz = khz;
            return AG_OK;
        }
    }
    return -AG_ENFILE;
}

static ag_err_t spi_device_for(int bus, int cs, spi_device_handle_t *out)
{
    if (!spi_valid(bus) || !s_spi[bus].up) {
        return -AG_ENODEV;
    }

    spi_dev_t *free_slot = NULL;
    for (int i = 0; i < AG_PORT_SPI_DEVS; i++) {
        spi_dev_t *d = &s_spi[bus].devs[i];
        if (d->cs == cs) {
            if (d->dev != NULL) {
                *out = d->dev;
                return AG_OK;
            }
            free_slot = d; /* speed was set for it, device not built yet */
            break;
        }
        if (free_slot == NULL && d->cs < 0 && d->dev == NULL) {
            free_slot = d;
        }
    }
    if (free_slot == NULL) {
        return -AG_ENFILE;
    }

    const uint32_t khz = (free_slot->khz != 0) ? free_slot->khz
                                               : s_spi[bus].khz;
    const spi_device_interface_config_t cfg = {
        .clock_speed_hz = (int)(khz * 1000u),
        .mode = 0,
        .spics_io_num = cs,
        /* One in flight is all this port ever queues - see the
         * staging pair in spi_state_t - but it must be allowed. */
        .queue_size = 2,
    };

    const ag_err_t err = from_esp(
        spi_bus_add_device(AG_PORT_SPI_HOST_OF(bus), &cfg, &free_slot->dev));
    if (err != AG_OK) {
        return err;
    }

    free_slot->cs = cs;
    free_slot->khz = khz;
    *out = free_slot->dev;
    return AG_OK;
}

static ag_err_t spi_xfer_locked(int bus, int cs, const void *tx, void *rx,
                                size_t len);

ag_err_t ag_port_spi_xfer(int bus, int cs, const void *tx, void *rx, size_t len)
{
    if (len == 0 || len > AG_PORT_SPI_MAX_XFER) {
        return -AG_EINVAL;
    }
    if (!spi_valid(bus) || !s_spi[bus].up || s_spi[bus].lock == NULL) {
        return -AG_ENODEV;
    }
    /* A master transfer on a slave bus is not a slow transfer, it is no
     * transfer at all: there is no device table and no clock to drive. */
    if (s_spi[bus].slave) {
        return -AG_EINVAL;
    }

    /*
     * Waiting for the bus is not an error to report upwards: the other holder
     * is a device driver in the middle of a transfer, and it will finish.  A
     * timeout here would turn a busy bus into a driver failure.
     */
    xSemaphoreTakeRecursive(s_spi[bus].lock, portMAX_DELAY);
    const ag_err_t err = spi_xfer_locked(bus, cs, tx, rx, len);
    xSemaphoreGiveRecursive(s_spi[bus].lock);
    return err;
}

static ag_err_t spi_drain(int bus, spi_device_handle_t dev);

/*
 * The caller's buffers straight to the engine, and the caller asleep while it
 * clocks.  No bounce, no staging and no polling: both buffers are the DMA's
 * for the length of the call, which is why both must be memory it can reach,
 * word-aligned.  spi_device_transmit queues the transaction and waits on its
 * completion interrupt, so a 4 KB frame at 20 MHz costs the core the setup
 * and the wake-up rather than the 1.6 ms of the wire.
 */
ag_err_t ag_port_spi_xfer_dma(int bus, int cs, const void *tx, void *rx,
                              size_t len)
{
    if (len == 0 || len > AG_PORT_SPI_MAX_XFER || tx == NULL || rx == NULL) {
        return -AG_EINVAL;
    }
    if (!esp_ptr_dma_capable(tx) || !esp_ptr_dma_capable(rx) ||
        ((uintptr_t)tx & 3u) != 0u || ((uintptr_t)rx & 3u) != 0u) {
        return -AG_EINVAL;
    }
    if (!spi_valid(bus) || !s_spi[bus].up || s_spi[bus].lock == NULL) {
        return -AG_ENODEV;
    }
    if (s_spi[bus].slave) {
        return -AG_EINVAL;
    }

    xSemaphoreTakeRecursive(s_spi[bus].lock, portMAX_DELAY);
    spi_device_handle_t dev = NULL;
    ag_err_t            err = spi_device_for(bus, cs, &dev);
    if (err == AG_OK) {
        err = spi_drain(bus, dev);
    }
    if (err == AG_OK) {
        spi_transaction_t t = {
            .length = len * 8u,
            .tx_buffer = tx,
            .rx_buffer = rx,
        };
        err = from_esp(spi_device_transmit(dev, &t));
    }
    xSemaphoreGiveRecursive(s_spi[bus].lock);
    return err;
}

/*
 * Wait for a queued long write to finish.  Every other path through the bus
 * calls this first: a transaction still in flight owns one half of the staging
 * pair and the device, and starting anything else on top of it is how a driver
 * that queues gets the reputation of being unreliable.
 */
static ag_err_t spi_drain(int bus, spi_device_handle_t dev)
{
    if (!s_spi[bus].tx_busy) {
        return AG_OK;
    }
    spi_transaction_t *done = NULL;
    const esp_err_t    ae =
        spi_device_get_trans_result(dev, &done, portMAX_DELAY);
    s_spi[bus].tx_busy = false;
    return from_esp(ae);
}

static ag_err_t spi_xfer_locked(int bus, int cs, const void *tx, void *rx,
                                size_t len)
{
    spi_device_handle_t dev = NULL;
    ag_err_t            err = spi_device_for(bus, cs, &dev);
    if (err != AG_OK) {
        return err;
    }

    const bool long_write = (rx == NULL && tx != NULL &&
                             len > AG_PORT_SPI_BOUNCE &&
                             len <= AG_PORT_SPI_MAX_XFER);
    if (!long_write) {
        const ag_err_t d = spi_drain(bus, dev);
        if (d != AG_OK) {
            return d;
        }
    }

    /*
     * Straight from the caller's buffer when the DMA engine can read it.
     *
     * The bounce buffer exists for memory DMA cannot reach - PSRAM, or a stack
     * in some ports - and copying through it unconditionally was the simple
     * choice.  It also capped every transfer at the bounce size, which on the
     * display path meant three transfers where one would do, and a transfer
     * costs the same setup whatever it carries.
     *
     * A write-only transfer from DMA-capable memory needs neither the copy nor
     * the cap.  Anything else still goes through the bounce, in pieces.
     */
    if (!long_write && rx == NULL && tx != NULL && esp_ptr_dma_capable(tx)) {
        spi_transaction_t direct = {
            .length = len * 8u,
            .tx_buffer = tx,
            .rx_buffer = NULL,
        };
        return from_esp(spi_device_polling_transmit(dev, &direct));
    }

    if (len > AG_PORT_SPI_BOUNCE) {
        /*
         * Has to be copied, and the copy used to go a bounce-buffer at a time.
         * Only for a write: chopping a transfer that also reads would drop
         * chip select between the halves, and a chip being read mid-command
         * would not survive that.
         */
        if (rx != NULL) {
            /*
             * A LONG TRANSFER THAT ALSO READS, and it used to be refused
             * outright.  The refusal was not a hardware limit: it was that the
             * only way to send something long was to cut it into bounce-sized
             * pieces, and cutting a transfer that reads drops chip select in
             * the middle of a chip's answer.  Nothing has to be cut any more.
             *
             * WHO NEEDED IT.  The cluster link is a full-duplex byte stream -
             * every block it clocks carries bytes in both directions - so it
             * was held at the bounce buffer's 512-byte block whatever the
             * wire could do, and its measured ceiling was about 500
             * transactions a second AT ANY CLOCK: 21 037 B/s at 5 MHz and
             * 15 852 B/s at 20 MHz.
             *
             * Synchronous, unlike the write below: the caller wants the bytes
             * that came back, so there is nothing to overlap with.
             */
            const ag_err_t d = spi_drain(bus, dev);
            if (d != AG_OK) {
                return d;
            }
            if (s_spi[bus].staging == NULL) {
                s_spi[bus].staging = ag_port_alloc_aligned(
                    4, 2u * AG_PORT_SPI_MAX_XFER, AG_MEM_DMA);
            }
            if (s_spi[bus].staging_rx == NULL) {
                s_spi[bus].staging_rx = ag_port_alloc_aligned(
                    4, AG_PORT_SPI_MAX_XFER, AG_MEM_DMA);
            }
            if (s_spi[bus].staging == NULL || s_spi[bus].staging_rx == NULL) {
                return -AG_ENOMEM;
            }
            if (tx != NULL) {
                memcpy(s_spi[bus].staging, tx, len);
            } else {
                memset(s_spi[bus].staging, 0, len);
            }
            spi_transaction_t both = {
                .length = len * 8u,
                .tx_buffer = s_spi[bus].staging,
                .rx_buffer = s_spi[bus].staging_rx,
            };
            const esp_err_t ae = spi_device_polling_transmit(dev, &both);
            if (ae != ESP_OK) {
                return from_esp(ae);
            }
            memcpy(rx, s_spi[bus].staging_rx, len);
            return AG_OK;
        }

        /*
         * ONE TRANSACTION, AND IT IS WORTH SIXTEEN KILOBYTES.  Cutting the
         * write into bounce-sized pieces cost a transaction per kilobyte, and
         * a transaction is not free at either end: the far side must ARM a
         * block for each one.  Measured on the boards at 40 MHz, a 5194-byte
         * frame became 5.3 transactions and the receiving service threw away
         * 24 MB of what arrived because it could not drain that many blocks -
         * 11.8 MB of picture for 72 MB of wire.
         *
         * So a bus that actually sends something long gets a staging buffer
         * the size of the longest transfer, made once and kept.  A bus that
         * never does pays nothing, which is why this is not simply a bigger
         * AG_PORT_SPI_BOUNCE: the panel buses on every other board would pay
         * for it too.
         */
        if (s_spi[bus].staging == NULL) {
            s_spi[bus].staging =
                ag_port_alloc_aligned(4, 2u * AG_PORT_SPI_MAX_XFER, AG_MEM_DMA);
        }
        if (s_spi[bus].staging != NULL) {
            /*
             * Reap the previous one FIRST - it owns the half that is about to
             * be filled - and only then copy and queue.  The call returns with
             * the bytes still going out, which is the whole point: the caller
             * builds the next frame during them.
             */
            const ag_err_t d = spi_drain(bus, dev);
            if (d != AG_OK) {
                return d;
            }
            const int      w = s_spi[bus].tx_which;
            uint8_t *const half =
                s_spi[bus].staging + (size_t)w * AG_PORT_SPI_MAX_XFER;
            memcpy(half, tx, len);
            s_spi[bus].tx_trans[w] = (spi_transaction_t){
                .length = len * 8u,
                .tx_buffer = half,
                .rx_buffer = NULL,
            };
            const esp_err_t ae = spi_device_queue_trans(
                dev, &s_spi[bus].tx_trans[w], portMAX_DELAY);
            if (ae != ESP_OK) {
                return from_esp(ae);
            }
            s_spi[bus].tx_busy = true;
            s_spi[bus].tx_which = w ^ 1;
            return AG_OK;
        }
        /*
         * Chip select must NOT come up between the pieces.  It used to, and
         * that made the dicing visible on the wire to anyone who frames on it:
         * a peer taking chip select as the frame boundary saw one 5 KB block
         * arrive as six.  So the bus is held for the whole run and every piece
         * but the last carries CS_KEEP_ACTIVE, which leaves the line down
         * across the gap.  The caller sees one transfer because the far end
         * does too.
         */
        esp_err_t ae = spi_device_acquire_bus(dev, portMAX_DELAY);
        if (ae != ESP_OK) {
            return from_esp(ae);
        }
        const uint8_t *p = (const uint8_t *)tx;
        size_t         left = len;
        while (left > 0) {
            const size_t n = (left > AG_PORT_SPI_BOUNCE) ? AG_PORT_SPI_BOUNCE
                                                         : left;
            memcpy(s_spi[bus].bounce, p, n);
            spi_transaction_t piece = {
                .flags = (n < left) ? SPI_TRANS_CS_KEEP_ACTIVE : 0u,
                .length = n * 8u,
                .tx_buffer = s_spi[bus].bounce,
                .rx_buffer = NULL,
            };
            ae = spi_device_polling_transmit(dev, &piece);
            if (ae != ESP_OK) {
                spi_device_release_bus(dev);
                return from_esp(ae);
            }
            p += n;
            left -= n;
        }
        spi_device_release_bus(dev);
        return AG_OK;
    }

    uint8_t *const out = s_spi[bus].bounce;
    uint8_t *const in = out + AG_PORT_SPI_BOUNCE;

    if (tx != NULL) {
        memcpy(out, tx, len);
    } else {
        memset(out, 0, len);
    }

    spi_transaction_t t = {
        .length = len * 8u,
        .tx_buffer = out,
        .rx_buffer = (rx != NULL) ? in : NULL,
    };
    /*
     * Polled, not queued.
     *
     * spi_device_transmit hands the transfer to the driver's queue, waits on a
     * semaphore and is woken by the interrupt at the end.  That is the right
     * shape for a transfer long enough to be worth sleeping through, and the
     * wrong one for the traffic that actually goes over this bus: a panel being
     * fed a frame in kilobyte pieces.  Measured on the display path, the queued
     * version cost about two hundred and twenty microseconds of setup per
     * transfer against two hundred of clock - so half the time on the wire was
     * not on the wire at all.
     *
     * Polling spins for the duration instead, which is exactly what a caller
     * pushing pixels wants: it has nothing else to do until they are sent, and
     * the bus is held for less time overall.
     */
    err = from_esp(spi_device_polling_transmit(dev, &t));

    if (err == AG_OK && rx != NULL) {
        memcpy(rx, in, len);
    }
    return err;
}

/*
 * Milliseconds to scheduler ticks, without wrapping round to no wait at all.
 * pdMS_TO_TICKS overflows for a large enough millisecond count, and a caller
 * asking for a whole minute of patience should get it.
 */
static TickType_t spi_ticks(uint32_t timeout_ms)
{
    const uint32_t most = (uint32_t)(portMAX_DELAY / portTICK_PERIOD_MS);
    return (timeout_ms >= most) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
}

/*
 * What is left of a caller's patience.
 *
 * Measured from the tick the call started, not accumulated wait by wait: one
 * call can wait several times over - xfer drains a stale block, then waits for
 * its own, and a reap that receives somebody else's block waits again - and
 * handing each of those the whole budget would turn a one-second deadline into
 * however many seconds it takes to feel like giving up.
 */
static TickType_t spi_left(TickType_t start, TickType_t budget)
{
    if (budget == portMAX_DELAY) {
        return portMAX_DELAY; /* "as long as it takes" */
    }
    const TickType_t spent = xTaskGetTickCount() - start;
    return (spent >= budget) ? 0 : budget - spent;
}

/* The slot a descriptor belongs to, or -1.  The descriptors are members of the
 * bus's state (see spi_slot_t), so the pointer the driver hands back names the
 * slot it came from and nothing has to be searched for in the descriptor. */
static int spi_slot_of(const spi_state_t *st,
                       const spi_slave_transaction_t *desc)
{
    for (int i = 0; i < AG_PORT_SPI_SLAVE_QUEUE; i++) {
        if (&st->slot[i].desc == desc) {
            return i;
        }
    }
    return -1;
}

/*
 * Put a collected block away in its slot, ready for its owner's reap, and only
 * then: a slot is not free again until the caller that armed it has been told,
 * because the memory it borrowed is still its caller's.
 *
 * Called with the pool lock held, from the one caller that received the block
 * from the driver - which is the block's owner when it reaped its own slot, and
 * somebody else when a reaper and spi_slave_xfer share the bus.
 */
static void spi_slot_collect(spi_state_t *st, int i, uint32_t trans_len)
{
    spi_slot_t *const sl = &st->slot[i];

    /*
     * `trans_len` is bits the peer actually clocked, and it is the number the
     * one-at-a-time call read and threw away: the caller of a slave transfer
     * has to frame for itself, and "how much of this block is really a frame"
     * is not something the port can answer for it.  Rounded up to whole bytes,
     * because the count is a whole number of bytes on the wire.
     */
    uint32_t got = (trans_len + 7u) / 8u;
    if (got > sl->len) {
        got = sl->len;
    }

    if (sl->staged) {
        /*
         * spi_slave_xfer's block.  The wire's bytes are in the bus's staging
         * half because the caller's memory is not necessarily one the DMA
         * engine could have written, so they are copied to the caller now, and
         * what the peer did not clock is zeros rather than the tail of the
         * block before it.
         */
        if (sl->rx != NULL) {
            memcpy(sl->rx, st->sbuf + AG_PORT_SPI_MAX_XFER, got);
            if (got < sl->len) {
                memset((uint8_t *)sl->rx + got, 0, sl->len - got);
            }
        }
    } else if (sl->rx != NULL && got < sl->len) {
        /*
         * An armed block, whose receive buffer the engine wrote directly.  A
         * block the peer cut short leaves the rest of that buffer as zeros, for
         * the same reason as above: a caller reads its own frame out of a
         * zero-padded block without having to know what the peer managed.
         */
        memset((uint8_t *)sl->rx + got, 0, sl->len - got);
    }

    sl->got = got;
    sl->armed = false;
    if (!sl->done) {
        sl->done = true;
        st->done_q[st->done_n++] = (uint8_t)i;
    }
}

/*
 * Hand one collected block to a caller.  `want` is the slot this caller is
 * waiting for, or -1 for the oldest block of the pipelined pool: slot 0 belongs
 * to spi_slave_xfer and is never handed to a reaper, and a reaper's blocks are
 * never handed to spi_slave_xfer.  Called with the pool lock held.
 */
static bool spi_slot_take(spi_state_t *st, int want, int *out, uint32_t *got)
{
    for (int i = 0; i < st->done_n; i++) {
        const int s = st->done_q[i];
        if ((want >= 0) ? (s != want)
                        : (s == AG_PORT_SPI_SLAVE_XFER_SLOT)) {
            continue;
        }
        for (int k = i; k + 1 < st->done_n; k++) {
            st->done_q[k] = st->done_q[k + 1];
        }
        st->done_n--;
        st->slot[s].done = false;
        if (got != NULL) {
            *got = st->slot[s].got;
        }
        *out = s;
        return true;
    }
    return false;
}

/*
 * Wait for the peer to clock one more block, and put it away in its slot.
 *
 * This is the call that blocks, and it is the only place in this port that
 * waits for anything.  There is no task here and none is wanted: a reaper is
 * whatever the layer above runs, and arming - which is what keeps the driver's
 * queue full while this waits - is a call that returns at once.
 *
 * One caller at a time is allowed in the driver's result call.  It hands back
 * whichever block completed first and not the one the caller asked for, so a
 * caller that receives somebody else's block puts it away for its owner and
 * waits again for its own; that is what lets a reaper and spi_slave_xfer share
 * one bus without either reading the other's blocks.  The pool lock is dropped
 * across the wait for the same reason it exists: a slot has to be claimable
 * while a block is in flight, or nothing would be armed while the peer clocks.
 */
static ag_err_t spi_slave_await(int bus, int want, TickType_t start,
                                TickType_t budget, int *out, uint32_t *got)
{
    const spi_host_device_t host = AG_PORT_SPI_HOST_OF(bus);
    spi_state_t *const      st = &s_spi[bus];

    for (;;) {
        /* Anything already collected goes first: a block that finished a
         * moment ago must not be handed over behind one that finished later. */
        xSemaphoreTake(st->pool, portMAX_DELAY);
        const bool ready = spi_slot_take(st, want, out, got);
        xSemaphoreGive(st->pool);
        if (ready) {
            return AG_OK;
        }

        const TickType_t left = spi_left(start, budget);
        if (xSemaphoreTake(st->waiter, left) != pdTRUE) {
            return -AG_ETIMEDOUT; /* another caller is waiting with the time */
        }
        spi_slave_transaction_t *desc = NULL;
        const esp_err_t rc = spi_slave_get_trans_result(host, &desc, left);
        xSemaphoreGive(st->waiter);

        if (rc != ESP_OK) {
            /*
             * Nothing completed in the time left.  One last look before saying
             * so, because a block that finished while another caller held the
             * wait was put away by that caller and not by this one.
             */
            xSemaphoreTake(st->pool, portMAX_DELAY);
            const bool late = spi_slot_take(st, want, out, got);
            xSemaphoreGive(st->pool);
            return late ? AG_OK : from_esp(rc);
        }

        xSemaphoreTake(st->pool, portMAX_DELAY);
        const int i = spi_slot_of(st, desc);
        if (i >= 0) {
            spi_slot_collect(st, i, desc->trans_len);
        }
        xSemaphoreGive(st->pool);
        /*
         * And round again.  A block armed out of order - or one whose slot is
         * not the one being waited for - simply leaves this caller waiting for
         * its own, and a block that IS its own is handed over by the look at
         * the top.  A descriptor this port did not arm belongs to no slot and
         * is dropped rather than mistaken for the block that was asked for.
         */
    }
}

static ag_err_t spi_slave_xfer_locked(int bus, const void *tx, void *rx,
                                      size_t len, TickType_t ticks);

ag_err_t ag_port_spi_slave_xfer(int bus, const void *tx, void *rx, size_t len,
                                uint32_t timeout_ms)
{
    if (len == 0 || len > AG_PORT_SPI_MAX_XFER) {
        return -AG_EINVAL;
    }
    if (tx == NULL && rx == NULL) {
        return -AG_EINVAL;
    }
    if (!spi_valid(bus) || !s_spi[bus].up || s_spi[bus].lock == NULL) {
        return -AG_ENODEV;
    }
    if (!s_spi[bus].slave) {
        return -AG_EINVAL;
    }

    /*
     * The caller's patience bounds the wait for the bus as well as the wait for
     * the clock.  A slave wait can be seconds - that is what waiting for a peer
     * is - and sitting on the bus lock for that long while another task wants
     * the bus would turn this caller's deadline into everyone's.
     */
    const TickType_t ticks = spi_ticks(timeout_ms);
    if (xSemaphoreTakeRecursive(s_spi[bus].lock, ticks) != pdTRUE) {
        return -AG_ETIMEDOUT;
    }
    const ag_err_t err = spi_slave_xfer_locked(bus, tx, rx, len, ticks);
    xSemaphoreGiveRecursive(s_spi[bus].lock);
    return err;
}

/*
 * The one-at-a-time transfer, built on the pool and on nothing else.
 *
 * It arms slot 0 and reaps slot 0, through the same calls the pipelined pair
 * uses, so there is exactly one place that knows how a block is queued and one
 * that knows how a completed block is matched to the buffers it borrowed.  What
 * makes it the slow path is not the calls but the waiting: it waits for its own
 * block before arming the next, and the moment in between is the window this
 * whole file is about.  A caller that needs the wire kept fed arms several
 * blocks ahead instead, and leaves this one alone.
 */
static ag_err_t spi_slave_xfer_locked(int bus, const void *tx, void *rx,
                                      size_t len, TickType_t ticks)
{
    const spi_host_device_t host = AG_PORT_SPI_HOST_OF(bus);
    spi_state_t *const      st = &s_spi[bus];
    const TickType_t        start = xTaskGetTickCount();
    int                     slot = -1;

    /*
     * Whatever timed out last time is still in the driver.  Collect it before
     * the staging buffers are touched: it is about to be clocked into them, and
     * a copy of the previous call's bytes arriving in the middle of this one
     * would be a frame assembled from two blocks.  What it carried is dropped -
     * its caller was already told the transfer failed.
     *
     * The consequence is one block of resynchronisation, and the layer above
     * has to know it: after a timeout, the peer's next select pulse completes
     * this stale transaction rather than the block being asked for now, so the
     * block after that is the first one this caller will see.  A protocol that
     * counts blocks - a link with sequence numbers, say - has to resynchronise
     * after a timeout rather than assume the next block it sends is the next
     * one answered.
     */
    xSemaphoreTake(st->pool, portMAX_DELAY);
    const bool stale = st->slot[AG_PORT_SPI_SLAVE_XFER_SLOT].armed ||
                       st->slot[AG_PORT_SPI_SLAVE_XFER_SLOT].done;
    xSemaphoreGive(st->pool);
    if (stale) {
        const ag_err_t err = spi_slave_await(bus, AG_PORT_SPI_SLAVE_XFER_SLOT,
                                             start, ticks, &slot, NULL);
        if (err != AG_OK) {
            return err;
        }
    }

    /*
     * Made the first time somebody actually transfers one at a time, not at
     * open: 16 KB of DMA memory that the pipelined pair never touches, held
     * from the first open for the life of the boot.  On the C3 that was the
     * difference between an SPI link with 4 KB frames opening and not.
     */
    if (st->sbuf == NULL) {
        st->sbuf = ag_port_alloc_aligned(4, 2u * AG_PORT_SPI_MAX_XFER,
                                         AG_MEM_DMA);
        if (st->sbuf == NULL) {
            return -AG_ENOMEM;
        }
    }
    uint8_t *const out = st->sbuf;

    /*
     * Length in bits, rounded up to a whole word.
     *
     * With DMA the driver requires the buffer address and the byte count to be
     * aligned together, and the count here is whatever the caller framed, which
     * is not always a multiple of four.  Asking for up to three bytes more than
     * the caller wants costs nothing: the buffers are the port's own, the peer
     * ends the block by releasing the select, and the driver reports how much
     * actually arrived.  Those bytes are zeros rather than whatever the last
     * block left there, so a peer that does clock them reads something that was
     * decided rather than the tail of an unrelated frame.  An armed block
     * cannot do this - it borrows the caller's memory, and three bytes past the
     * end of that is somebody else's - which is why arm asks for a whole number
     * of words instead.
     */
    const size_t plen = (len + 3u) & ~(size_t)3u;

    if (tx != NULL) {
        memcpy(out, tx, len);
    } else {
        memset(out, 0, len);
    }
    if (plen > len) {
        memset(out + len, 0, plen - len);
    }

    /*
     * The descriptor is the slot's and not this frame's: the driver keeps a
     * pointer to it and writes trans_len through it in its interrupt, which can
     * happen long after this call has given up.  See the note on spi_slot_t.
     */
    xSemaphoreTake(st->pool, portMAX_DELAY);
    spi_slot_t *const sl = &st->slot[AG_PORT_SPI_SLAVE_XFER_SLOT];
    sl->rx = rx;
    sl->len = len;
    sl->got = 0;
    sl->staged = true;
    sl->armed = true;
    sl->desc = (spi_slave_transaction_t){
        /* The buffers are aligned for the common case; on a target that wants
         * more than word alignment the driver copies rather than refusing. */
        .flags = SPI_SLAVE_TRANS_DMA_BUFFER_ALIGN_AUTO,
        .length = plen * 8u,
        .tx_buffer = out,
        .rx_buffer = (rx != NULL) ? (out + AG_PORT_SPI_MAX_XFER) : NULL,
    };
    xSemaphoreGive(st->pool);

    const esp_err_t rc = spi_slave_queue_trans(host, &sl->desc, ticks);
    if (rc != ESP_OK) {
        xSemaphoreTake(st->pool, portMAX_DELAY);
        sl->armed = false;
        xSemaphoreGive(st->pool);
        return from_esp(rc);
    }

    /*
     * Wait for this block's own select pulse.  On a timeout the slot stays
     * armed: the peer may clock it yet, the next call collects it and drops it,
     * and nothing is written to the caller's `rx` in the meantime - bytes
     * nobody clocked are not a frame.
     */
    return spi_slave_await(bus, AG_PORT_SPI_SLAVE_XFER_SLOT, start, ticks, &slot,
                           NULL);
}

/*
 * Arm one block and return at once.
 *
 * This is the call that closes the window, and the reason it is separate from
 * the wait: a caller can have three blocks in the driver and a fourth in its
 * hands, so the select pulse that arrives while it is working finds a
 * transaction already queued and is served by the driver's interrupt with no
 * software in the loop.  Waiting here would put the caller back where it was -
 * one block armed at a time, and the gap between two of them exactly as wide as
 * the caller's own processing.
 *
 * The pool is three blocks deep, not four: slot 0 is spi_slave_xfer's.
 */
ag_err_t ag_port_spi_slave_arm(int bus, const void *tx, void *rx, size_t len)
{
    if (len == 0 || len > AG_PORT_SPI_MAX_XFER) {
        return -AG_EINVAL;
    }
    if (tx == NULL && rx == NULL) {
        return -AG_EINVAL;
    }
    if (!spi_valid(bus) || !s_spi[bus].up || s_spi[bus].lock == NULL) {
        return -AG_ENODEV;
    }
    spi_state_t *const st = &s_spi[bus];
    if (!st->slave) {
        return -AG_EINVAL;
    }

    /*
     * The caller's own memory goes to the DMA engine, and the engine's terms
     * are checked here rather than discovered on the wire: memory it cannot
     * reach is a transfer that never starts, and a receive buffer it would
     * write a whole word past the end of is a corruption of whatever lives
     * there.  A caller that cannot promise either uses spi_slave_xfer, which
     * copies through buffers this port owns.
     */
    if (tx != NULL && !esp_ptr_dma_capable(tx)) {
        return -AG_EINVAL;
    }
    if (rx != NULL) {
        if (!esp_ptr_dma_capable(rx) || !esp_ptr_word_aligned(rx)) {
            return -AG_EINVAL;
        }
        if ((len & 3u) != 0u) {
            return -AG_EINVAL; /* the engine writes words, so the block is one */
        }
    }

    if (xSemaphoreTake(st->pool, 0) != pdTRUE) {
        return -AG_EAGAIN; /* another caller is mid-arm; come back in a moment */
    }

    if (tx == NULL) {
        /*
         * No transmit buffer means the block carries zeros - the one-at-a-time
         * call has always done that, and a slave has to put something on the
         * line for a peer that clocks it.  There is no caller memory to borrow
         * for this, so the bus keeps a page of its own, once.
         */
        if (st->zero == NULL) {
            st->zero = ag_port_alloc_aligned(4, AG_PORT_SPI_MAX_XFER,
                                             AG_MEM_DMA);
            if (st->zero != NULL) {
                memset(st->zero, 0, AG_PORT_SPI_MAX_XFER);
            }
        }
        if (st->zero == NULL) {
            xSemaphoreGive(st->pool);
            return -AG_ENOMEM;
        }
        tx = st->zero;
    }

    /*
     * A slot is free when it is neither armed with the driver nor holding
     * collected bytes for a caller that has not reaped it.  The second half is
     * the part that matters: the memory is still borrowed until then, whatever
     * the driver has finished with it.
     */
    int idx = -1;
    for (int i = AG_PORT_SPI_SLAVE_XFER_SLOT + 1; i < AG_PORT_SPI_SLAVE_QUEUE;
         i++) {
        if (!st->slot[i].armed && !st->slot[i].done) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        xSemaphoreGive(st->pool);
        /* Back-pressure, not failure: every slot is either on the wire or
         * waiting to be reaped, and the caller's next move is to reap. */
        return -AG_EAGAIN;
    }

    spi_slot_t *const sl = &st->slot[idx];
    sl->rx = rx;
    sl->len = len;
    sl->got = 0;
    sl->staged = false;
    sl->armed = true; /* claimed before the lock goes, so nobody else takes it */
    sl->desc = (spi_slave_transaction_t){
        .flags = SPI_SLAVE_TRANS_DMA_BUFFER_ALIGN_AUTO,
        .length = len * 8u,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    /*
     * Queued while the pool lock is still held, so that this cannot land in the
     * middle of anything that is rebuilding the interface - the pool lock is
     * what a reset takes to hold the driver still, and it is only ever held for
     * a moment, including here: a queue call with no wait does not block on
     * anything but the driver's own queue, which the pool is as deep as.
     */
    const esp_err_t rc = spi_slave_queue_trans(AG_PORT_SPI_HOST_OF(bus),
                                               &sl->desc, 0);
    if (rc != ESP_OK) {
        sl->armed = false; /* the slot never reached the wire */
    }
    xSemaphoreGive(st->pool);

    if (rc != ESP_OK) {
        return (rc == ESP_ERR_TIMEOUT) ? -AG_EAGAIN : from_esp(rc);
    }
    return AG_OK;
}

/*
 * Wait for the next block the peer has clocked, and say which one it was.
 *
 * The slot number is the whole answer for a pipelined caller: it armed several
 * blocks, each out of its own memory, and the number says which of them the
 * bytes belong to.  `got` says how much of it the peer clocked, which a caller
 * that frames for itself needs and a caller that does not can ignore.
 *
 * A timeout is reported as one and means nothing was clocked in time; it does
 * not cancel the arming.  The block is still in the driver, the peer may clock
 * it yet, and a later reap collects it and hands it over - with the bytes the
 * peer really sent, because each slot kept its own buffers.  What the caller
 * must not do is touch those buffers again, or arm that slot's memory, until it
 * has reaped that block.
 */
ag_err_t ag_port_spi_slave_reap(int bus, int *slot, uint32_t *got, void **rx,
                                uint32_t timeout_ms)
{
    if (!spi_valid(bus) || !s_spi[bus].up || s_spi[bus].lock == NULL) {
        return -AG_ENODEV;
    }
    const spi_state_t *const st = &s_spi[bus];
    if (!st->slave) {
        return -AG_EINVAL;
    }

    int             s = -1;
    uint32_t        n = 0;
    const ag_err_t  err = spi_slave_await(bus, -1, xTaskGetTickCount(),
                                          spi_ticks(timeout_ms), &s, &n);
    if (err != AG_OK) {
        return err;
    }

    if (slot != NULL) {
        *slot = s;
    }
    if (got != NULL) {
        *got = n;
    }
    /* which of the caller's buffers the peer's bytes are in: the one thing
     * that names the block without assuming the order they come back in */
    if (rx != NULL) {
        *rx = (s >= 0 && s < AG_PORT_SPI_SLAVE_QUEUE) ? st->slot[s].rx : NULL;
    }
    return AG_OK;
}

/*
 * Give the pool back when the caller has nothing left to reap.
 *
 * A transaction the driver has handed to the hardware cannot be cancelled:
 * ESP-IDF has no call for it, resetting the queue would leave the mounted one
 * pointing at memory the caller is about to reuse, and the mounted one is
 * exactly what a peer that stopped clocking leaves behind.  So a link whose
 * peer goes away ends with every slot armed and none reapable, arm answers
 * -AG_EAGAIN for ever, and the board that runs the service on top of this port
 * stops answering commands - measured on the bench as five sessions in a row
 * where every command timed out at 30 s and only a reflash cleared it.
 *
 * The way out is to throw the INTERFACE away and build it again: free() deletes
 * the driver's queues, frees the DMA channels and their descriptors, disconnects
 * the bus pins and switches the peripheral's clock off, so after it returns
 * nothing in the chip can reach the buffers the abandoned blocks borrowed.  The
 * slots are released only at that point and not before - releasing them earlier
 * would hand the caller memory the engine could still write to.  Building the
 * interface again is the same call an open makes, from the same terms, so a bus
 * that comes out of this is a bus that was just opened.
 *
 * THE GUARANTEE, and it is the reason this call exists: once it returns AG_OK
 * every slot is free and arm takes a block on the very next call, whatever the
 * peer did or did not clock, without waiting for it.  Nothing here waits on the
 * wire - the work is the port's own teardown and initialisation, which is
 * microseconds to a few milliseconds - so a caller can recover inside its own
 * timeout rather than at the next reflash.
 *
 * The one thing this cannot ask for is the peer's attention: ESP-IDF requires
 * that no transfer is in progress when the slave driver is freed, and its free
 * deletes the queues before it frees the interrupt, so a peer that clocks a
 * block in that window can still make the driver's interrupt run against a
 * deleted queue.  In the case this call is for - the peer has stopped - nothing
 * completes, nothing is pending, and the teardown is clean.
 */
ag_err_t ag_port_spi_slave_reset(int bus)
{
    if (!spi_valid(bus)) {
        return -AG_ERANGE;
    }
    spi_state_t *const st = &s_spi[bus];
    /*
     * The port's own resources come and go together - an open makes all four
     * or cleans up after itself - so any one of them missing means this bus was
     * never opened as anything, and there is no pool to give back.
     */
    if (st->lock == NULL || st->pool == NULL || st->waiter == NULL) {
        return -AG_ENODEV;
    }
    if (!st->slave) {
        return -AG_EINVAL; /* this bus is the end that holds the clock */
    }

    /*
     * Two callers must not be in the way, and both are refused rather than
     * waited for.  A transfer of the one-at-a-time kind owns the bus for the
     * length of its call.  A reaper inside the driver's result call is worse
     * than in the way: the queues it is waiting on are about to be deleted
     * under it, which is a use-after-free and not a wait, so the answer is
     * -AG_EBUSY and the caller's own task is the one that can get it out of
     * there.  A reap that is merely waiting its turn holds nothing: it is
     * serialised behind the very lock this call takes, and finds a fresh
     * interface when its turn comes.
     */
    if (xSemaphoreTakeRecursive(st->lock, 0) != pdTRUE) {
        return -AG_EBUSY;
    }
    if (xSemaphoreTake(st->waiter, 0) != pdTRUE) {
        xSemaphoreGiveRecursive(st->lock);
        return -AG_EBUSY;
    }

    /*
     * The pool lock is held across the whole rebuild, and that is what makes
     * the guarantee: an arm sees the pool either before this or after it,
     * never a half-released slot or a half-built interface.  What it costs is
     * bounded - nothing under this lock waits for the peer, or for anything
     * that waits for the peer - and an arm that arrives meanwhile is answered
     * -AG_EAGAIN and comes back, exactly as it is when the pool is full.
     */
    xSemaphoreTake(st->pool, portMAX_DELAY);

    const spi_host_device_t host = AG_PORT_SPI_HOST_OF(bus);

    (void)spi_slave_free(host);

    for (int i = 0; i < AG_PORT_SPI_SLAVE_QUEUE; i++) {
        spi_slot_t *const sl = &st->slot[i];
        sl->rx = NULL;
        sl->len = 0;
        sl->got = 0;
        sl->armed = false;
        sl->done = false;
        /* The descriptor is the slot's for the life of the bus and the driver
         * may have written trans_len through it on its way out; a block armed
         * from here on builds its own, so nothing of the old one survives. */
        memset(&sl->desc, 0, sizeof(sl->desc));
    }
    st->done_n = 0;

    const ag_err_t err = spi_slave_if_init(bus, st->sck, st->mosi, st->miso,
                                           st->slave_cs);
    if (err == AG_OK) {
        /*
         * The pad drive strength is not part of the interface and is not lost
         * with it: it lives in the pad's own register, gpio_reset_pin clears
         * pulls and the pin function rather than the drive field, and the SPI
         * driver never writes one.  That matters because on this project's
         * wiring it is the setting the link cannot work at all without - see
         * ag_port_spi_set_drive - so a reset that quietly put the pads back to
         * the chip's default would turn a recoverable link into a broken one.
         */
        st->up = true;
    } else {
        /* No interface: say so rather than let arm find out from the driver.
         * The port's own resources are still here, so opening the bus as a
         * slave again rebuilds it without making a second set. */
        st->up = false;
    }

    xSemaphoreGive(st->pool);
    xSemaphoreGive(st->waiter);
    xSemaphoreGiveRecursive(st->lock);
    return err;
}

/*
 * Pad drive strength, and why it is part of the bus rather than a pin setting
 * somebody applies when a link misbehaves.
 *
 * At the chip's default strength this project's own SPI link between two boards
 * returned a few clean frames in four thousand and nothing about it looked like
 * a drive problem; at the weakest setting it returned four thousand in four
 * thousand, at every rate measured.  The failure mode is what makes the setting
 * belong here: it looks like a wire, a connector, a bad frame or a slow peer,
 * and none of those is the cause.  See docs/plans/cluster.md.
 */
ag_err_t ag_port_spi_set_drive(int bus, int cap)
{
    if (!spi_valid(bus)) {
        return -AG_ERANGE;
    }
    if (cap < -1 || cap > 3) {
        return -AG_EINVAL;
    }
    if (!s_spi[bus].up) {
        return -AG_ENODEV;
    }
    if (cap < 0) {
        return AG_OK; /* leave the chip's default alone */
    }

    const int pins[3] = {s_spi[bus].sck, s_spi[bus].mosi, s_spi[bus].miso};
    for (int i = 0; i < 3; i++) {
        const int pin = pins[i];
        if (pin < 0) {
            continue;
        }
        const esp_err_t rc = gpio_set_drive_capability(pin,
                                                       (gpio_drive_cap_t)cap);
        /*
         * A line the peripheral only reads has no drive to set - the master's
         * miso is exactly that - and IDF says so with INVALID_ARG.  Skipping it
         * is not a failure of the request; the clock and the data-out are the
         * lines that matter, and they are the ones that were measured.
         */
        if (rc == ESP_ERR_INVALID_ARG && !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
            continue;
        }
        if (rc != ESP_OK) {
            return from_esp(rc);
        }
    }
    return AG_OK;
}

/* ---------------------------------------------------------------------- */
/* PWM                                                                    */
/* ---------------------------------------------------------------------- */

ag_err_t ag_port_pwm_config(int channel, int pin, uint32_t hz, uint8_t bits)
{
    if (channel < 0 || channel >= AG_PORT_PWM_CHANNELS) {
        return -AG_ERANGE;
    }

    /*
     * One timer per channel, so that two pins can run at different rates.  The
     * chip has four; beyond that, channels share and the last frequency wins -
     * which is a limit worth knowing rather than hiding.
     */
    const ledc_timer_t        timer = (ledc_timer_t)(channel % LEDC_TIMER_MAX);
    const ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = timer,
        .duty_resolution = (ledc_timer_bit_t)bits,
        .freq_hz = hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    ag_err_t err = from_esp(ledc_timer_config(&tcfg));
    if (err == AG_OK) {
        const ledc_channel_config_t ccfg = {
            .gpio_num = pin,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)channel,
            .timer_sel = timer,
            .duty = 0,
            .hpoint = 0,
        };
        err = from_esp(ledc_channel_config(&ccfg));
    }
    return err;
}

ag_err_t ag_port_pwm_set(int channel, uint32_t duty)
{
    if (channel < 0 || channel >= AG_PORT_PWM_CHANNELS) {
        return -AG_ERANGE;
    }

    ag_err_t err = from_esp(
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel, duty));
    if (err == AG_OK) {
        err = from_esp(
            ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel));
    }
    return err;
}

void ag_port_pwm_stop(int channel)
{
    if (channel >= 0 && channel < AG_PORT_PWM_CHANNELS) {
        (void)ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel, 0);
    }
}

/* ---------------------------------------------------------------------- */
/* ADC                                                                    */
/* ---------------------------------------------------------------------- */

#if AG_PORT_HAS_ADC

static adc_oneshot_unit_handle_t s_adc1;
static bool                      s_adc_chan[AG_PORT_ADC_CHANNELS];

int32_t ag_port_adc_read(int channel)
{
    if (channel < 0 || channel >= AG_PORT_ADC_CHANNELS) {
        return -AG_ERANGE;
    }
    if (AG_PORT_ADC_GPIO(channel) < 0) {
        return -AG_ENOTSUP;
    }

    if (s_adc1 == NULL) {
        const adc_oneshot_unit_init_cfg_t init = {
            .unit_id = ADC_UNIT_1,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        const ag_err_t err = from_esp(adc_oneshot_new_unit(&init, &s_adc1));
        if (err != AG_OK) {
            return err;
        }
    }

    if (!s_adc_chan[channel]) {
        const adc_oneshot_chan_cfg_t chan = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            /* The widest range: measuring a 3.3 V rail at 0 dB reads full
             * scale and says nothing, which is the mistake everybody makes. */
            .atten = ADC_ATTEN_DB_12,
        };
        const ag_err_t err = from_esp(
            adc_oneshot_config_channel(s_adc1, (adc_channel_t)channel, &chan));
        if (err != AG_OK) {
            return err;
        }
        s_adc_chan[channel] = true;
    }

    int            raw = 0;
    const ag_err_t rc =
        from_esp(adc_oneshot_read(s_adc1, (adc_channel_t)channel, &raw));
    return (rc == AG_OK) ? (int32_t)raw : rc;
}

#else

int32_t ag_port_adc_read(int channel)
{
    (void)channel;
    return -AG_ENOTSUP;
}

#endif /* AG_PORT_HAS_ADC */
