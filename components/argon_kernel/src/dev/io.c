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
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dev/io.h"

#include <stdio.h>
#include <string.h>

#include <argon/board.h>
#include <argon/device.h>
#include <argon/ioclaim.h>
#include <argon/log.h>
#include <argon/proc.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#if CONFIG_ARGON_ENABLE_ADC
#include "esp_adc/adc_oneshot.h"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/gpio_types.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "soc/uart_pins.h"

#define AG_IO_PWM_CHANNELS 8
#define AG_IO_UART_RX_BUF 1024

/*
 * The longest single SPI transfer.  It is the size of the bounce buffer, and
 * the bounce buffer exists because an application's data lives in PSRAM while
 * the DMA engine wants internal memory - so every transfer is copied through
 * one, and the limit is what one costs.
 *
 * A caller with more to send splits it, and a chip that cannot have its chip
 * select go down between the parts is driven with cs = -1, holding the select
 * itself.  Both are said out loud in the API reference, because a silent limit
 * looks like a driver that works until the day somebody sends a whole frame.
 */
#define AG_IO_SPI_MAX_XFER 1024

/* ADC1 channel N is GPIO N+1 on the S3; other targets differ and say so. */
#if CONFIG_IDF_TARGET_ESP32S3
#define AG_IO_ADC1_FIRST_GPIO 1
#else
#define AG_IO_ADC1_FIRST_GPIO (-1)
#endif

static SemaphoreHandle_t s_lock;
static bool              s_isr_service;

/* ---------------------------------------------------------------------- */

static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

/* Whoever is calling: a process, or the kernel when nothing is loaded. */
static ag_pid_t caller(void) { return ag_proc_self(); }

static bool valid_pin(int pin)
{
    return pin >= 0 && pin < ag_io_pin_count();
}

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
#if CONFIG_IDF_TARGET_ESP32S3
    /* SPI0/1 to the flash chip: 26 SPICS1, 27 SPIHD, 28 SPIWP, 29 SPICS0,
     * 30 SPICLK, 31 SPIQ, 32 SPID. */
    reserve_range(26, 32, "flash");
#if CONFIG_SPIRAM_MODE_OCT
    /* Octal PSRAM takes four more data lines and its own chip select. */
    reserve_range(33, 37, "psram");
#endif
#endif

    /*
     * The console's own pins.  ESP-IDF only defines the CONFIG_ symbols when
     * the pins were customised; with the defaults it uses U0TXD_GPIO_NUM, and
     * a build that looked only at the CONFIG_ symbols left the pins the system
     * is talking over free for anybody to take - which showed up as an empty
     * pair of rows in `io` and would have shown up on a board as a console that
     * went silent.
     */
    int console_tx = U0TXD_GPIO_NUM;
    int console_rx = U0RXD_GPIO_NUM;
#if defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    if (CONFIG_ESP_CONSOLE_UART_TX_GPIO >= 0) {
        console_tx = CONFIG_ESP_CONSOLE_UART_TX_GPIO;
    }
    if (CONFIG_ESP_CONSOLE_UART_RX_GPIO >= 0) {
        console_rx = CONFIG_ESP_CONSOLE_UART_RX_GPIO;
    }
#endif
    reserve_pin(console_tx, "console tx");
    reserve_pin(console_rx, "console rx");

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
        break;
    }

    lock();
    ag_err_t err = ag_io_claim(pin, caller(), mode_reason(mode));
    if (err != AG_OK) {
        unlock();
        return err;
    }

    err = from_esp(gpio_config(&cfg));
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
        (void)gpio_set_level((gpio_num_t)pin, (level != 0) ? 1 : 0);
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
    return gpio_get_level((gpio_num_t)pin);
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

    if (!s_isr_service) {
        const esp_err_t installed = gpio_install_isr_service(0);
        if (installed != ESP_OK && installed != ESP_ERR_INVALID_STATE) {
            unlock();
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
    (void)gpio_isr_handler_remove((gpio_num_t)pin);
    (void)gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_DISABLE);
    (void)ag_io_set_isr(pin, caller(), false);
    unlock();
    return AG_OK;
}

/* ---------------------------------------------------------------------- */
/* I2C                                                                    */
/* ---------------------------------------------------------------------- */

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint8_t                 dev_addr;
    bool                    up;
    bool                    warned; /* said "no pins" once already          */
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

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = bus,
        .sda_io_num = (gpio_num_t)cfg->sda,
        .scl_io_num = (gpio_num_t)cfg->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = cfg->pullups},
    };

    err = from_esp(i2c_new_master_bus(&bus_cfg, &s_i2c[bus].bus));
    if (err != AG_OK) {
        return err;
    }

    s_i2c[bus].up = true;
    ag_log(AG_LOG_INFO, "io", "i2c%d up on sda=%d scl=%d at %u kHz", bus,
           cfg->sda, cfg->scl, (unsigned)cfg->khz);
    return AG_OK;
}

/*
 * One cached device handle per bus.  The new ESP-IDF driver wants a handle per
 * address, and this API is per transaction, so something has to bridge the two:
 * a cache of one is enough because a driver talks to its own chip in a run, and
 * changing address costs one allocation rather than one per byte.
 */
static ag_err_t i2c_device(int bus, uint8_t addr, i2c_master_dev_handle_t *out)
{
    ag_err_t err = i2c_bring_up(bus);
    if (err != AG_OK) {
        return err;
    }

    if (s_i2c[bus].dev != NULL && s_i2c[bus].dev_addr == addr) {
        *out = s_i2c[bus].dev;
        return AG_OK;
    }
    if (s_i2c[bus].dev != NULL) {
        (void)i2c_master_bus_rm_device(s_i2c[bus].dev);
        s_i2c[bus].dev = NULL;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = ag_board()->i2c[bus].khz * 1000u,
    };
    err = from_esp(
        i2c_master_bus_add_device(s_i2c[bus].bus, &dev_cfg, &s_i2c[bus].dev));
    if (err != AG_OK) {
        return err;
    }

    s_i2c[bus].dev_addr = addr;
    *out = s_i2c[bus].dev;
    return AG_OK;
}

static ag_err_t io_i2c_write(int bus, uint8_t addr, const void *buf, size_t len,
                             uint32_t timeout_ms)
{
    if (buf == NULL || len == 0) {
        return -AG_EINVAL;
    }

    lock();
    i2c_master_dev_handle_t dev = NULL;
    ag_err_t                err = i2c_device(bus, addr, &dev);
    if (err == AG_OK) {
        err = from_esp(i2c_master_transmit(dev, buf, len, (int)timeout_ms));
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
    i2c_master_dev_handle_t dev = NULL;
    ag_err_t                err = i2c_device(bus, addr, &dev);
    if (err == AG_OK) {
        err = from_esp(i2c_master_receive(dev, buf, len, (int)timeout_ms));
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
    i2c_master_dev_handle_t dev = NULL;
    ag_err_t                err = i2c_device(bus, addr, &dev);
    if (err == AG_OK) {
        err = from_esp(i2c_master_transmit_receive(dev, wbuf, wlen, rbuf, rlen,
                                                   (int)timeout_ms));
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
        /*
         * Silence the driver for the duration.  An address with nothing on it
         * times out, and the driver logs that as an error - which is right when
         * a driver is talking to a chip it expects to be there, and wrong a
         * hundred and twelve times over when the whole point is to find out
         * which addresses are empty.  A scan of an empty bus filled the screen
         * with ESP-IDF error text and buried its own result.
         */
        const esp_log_level_t was = esp_log_level_get("i2c.master");
        esp_log_level_set("i2c.master", ESP_LOG_NONE);

        /* 10 ms: a chip that is there answers in microseconds, and this is
         * multiplied by every address a scan asks about. */
        const esp_err_t rc = i2c_master_probe(s_i2c[bus].bus, addr, 10);

        esp_log_level_set("i2c.master", was);

        err = (rc == ESP_OK) ? AG_OK
                             : ((rc == ESP_ERR_NOT_FOUND || rc == ESP_ERR_TIMEOUT)
                                    ? -AG_ENOENT
                                    : from_esp(rc));
    }
    unlock();
    return err;
}

/* ---------------------------------------------------------------------- */
/* SPI                                                                    */
/* ---------------------------------------------------------------------- */

typedef struct {
    spi_device_handle_t dev;
    int                 dev_cs;
    uint8_t            *bounce; /* tx half then rx half, internal and DMA-able */
    bool                up;
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

    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = cfg->mosi,
        .miso_io_num = cfg->miso,
        .sclk_io_num = cfg->sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = AG_IO_SPI_MAX_XFER,
    };

    const esp_err_t rc = spi_bus_initialize(AG_SPI_HOST_OF(bus), &bus_cfg,
                                            SPI_DMA_CH_AUTO);
    /* Somebody else may already own it - the card on SPI, for one. */
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        return from_esp(rc);
    }

    s_spi[idx].bounce = heap_caps_malloc(2u * AG_IO_SPI_MAX_XFER,
                                         MALLOC_CAP_DMA);
    if (s_spi[idx].bounce == NULL) {
        (void)spi_bus_free(AG_SPI_HOST_OF(bus));
        return -AG_ENOMEM;
    }

    s_spi[idx].up = true;
    s_spi[idx].dev_cs = AG_PIN_NONE;
    ag_log(AG_LOG_INFO, "io", "spi%d up on sck=%d mosi=%d miso=%d at %u kHz",
           bus, cfg->sck, cfg->mosi, cfg->miso, (unsigned)cfg->khz);
    return AG_OK;
}

static ag_err_t spi_device_for(int bus, int cs, spi_device_handle_t *out)
{
    ag_err_t err = spi_bring_up(bus);
    if (err != AG_OK) {
        return err;
    }

    const int idx = spi_index(bus);
    if (s_spi[idx].dev != NULL && s_spi[idx].dev_cs == cs) {
        *out = s_spi[idx].dev;
        return AG_OK;
    }
    if (s_spi[idx].dev != NULL) {
        (void)spi_bus_remove_device(s_spi[idx].dev);
        s_spi[idx].dev = NULL;
        s_spi[idx].dev_cs = AG_PIN_NONE;
    }

    if (cs >= 0) {
        char why[AG_IO_REASON_MAX];
        snprintf(why, sizeof(why), "spi%d cs", bus);

        const int16_t cs_pin = (int16_t)cs;
        err = reserve_bus_pins(&cs_pin, 1, why);
        if (err != AG_OK) {
            return err;
        }
    }

    const spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = (int)(ag_board()->spi[idx].khz * 1000u),
        .mode = 0,
        .spics_io_num = cs,
        .queue_size = 1,
    };

    err = from_esp(spi_bus_add_device(AG_SPI_HOST_OF(bus), &dev_cfg,
                                      &s_spi[idx].dev));
    if (err != AG_OK) {
        return err;
    }

    s_spi[idx].dev_cs = cs;
    *out = s_spi[idx].dev;
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
    if (len == 0 || len > AG_IO_SPI_MAX_XFER) {
        return -AG_EINVAL;
    }
    if (tx == NULL && rx == NULL) {
        return -AG_EINVAL;
    }

    lock();
    spi_device_handle_t dev = NULL;
    ag_err_t            err = spi_device_for(bus, cs, &dev);
    if (err != AG_OK) {
        unlock();
        return err;
    }

    /*
     * Through the bounce buffer, always.  The caller's data is in PSRAM and the
     * DMA engine wants internal memory; deciding case by case would mean two
     * paths, one of which is exercised only on the board nobody has.
     */
    uint8_t *const out = s_spi[spi_index(bus)].bounce;
    uint8_t *const in = out + AG_IO_SPI_MAX_XFER;

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
    err = from_esp(spi_device_transmit(dev, &t));

    if (err == AG_OK && rx != NULL) {
        memcpy(rx, in, len);
    }
    unlock();
    return err;
}

/* ---------------------------------------------------------------------- */
/* UART                                                                   */
/* ---------------------------------------------------------------------- */

static bool s_uart_up[AG_UART_PORTS];

static ag_err_t uart_bring_up(int port, const uart_config_t *want)
{
    if (port < 0 || port >= AG_UART_PORTS) {
        return -AG_ERANGE;
    }
    /*
     * UART0 is how the system is talking to whoever is asking.  Handing it out
     * would end the conversation, and the caller would never see the error.
     */
    if (port == CONFIG_ESP_CONSOLE_UART_NUM) {
        return -AG_EBUSY;
    }

    const ag_board_uart_t *cfg = &ag_board()->uart[port];
    if (cfg->tx < 0 && cfg->rx < 0) {
        ag_log(AG_LOG_WARN, "io",
               "uart%d has no pins; set uart%d.tx and uart%d.rx in BOARD.CFG",
               port, port, port);
        return -AG_ENODEV;
    }

    uart_config_t uart_cfg = {
        .baud_rate = (int)cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
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

        const esp_err_t rc = uart_driver_install(port, AG_IO_UART_RX_BUF,
                                                 AG_IO_UART_RX_BUF, 0, NULL, 0);
        if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
            return from_esp(rc);
        }
        s_uart_up[port] = true;
    }

    ag_err_t err = from_esp(uart_param_config(port, &uart_cfg));
    if (err == AG_OK) {
        err = from_esp(uart_set_pin(port, cfg->tx, cfg->rx, UART_PIN_NO_CHANGE,
                                    UART_PIN_NO_CHANGE));
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

    uart_config_t cfg = {
        .baud_rate = (int)baud,
        .data_bits = (uart_word_length_t)(UART_DATA_5_BITS + (databits - 5)),
        .parity = (parity == 0)   ? UART_PARITY_DISABLE
                  : (parity == 1) ? UART_PARITY_ODD
                                  : UART_PARITY_EVEN,
        .stop_bits = (stopbits == 1) ? UART_STOP_BITS_1 : UART_STOP_BITS_2,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
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
        n = (int32_t)uart_write_bytes(port, buf, len);
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
    const int n = uart_read_bytes(port, buf, len, pdMS_TO_TICKS(timeout_ms));
    return (n < 0) ? -AG_EIO : (int32_t)n;
}

/* ---------------------------------------------------------------------- */
/* ADC                                                                    */
/* ---------------------------------------------------------------------- */

/*
 * Off unless CONFIG_ARGON_ENABLE_ADC, and the reason is in the Kconfig help:
 * linking esp_adc runs a constructor before app_main that stops the boot under
 * QEMU.  With the option off, io->adc_read is NULL rather than a stub that
 * fails - an application asks with AG_HAS and adapts, which is what the whole
 * feature-probing convention is for.
 */
#if CONFIG_ARGON_ENABLE_ADC

static adc_oneshot_unit_handle_t s_adc1;
static bool                      s_adc_chan[SOC_ADC_MAX_CHANNEL_NUM];

static int32_t io_adc_read(int channel)
{
    if (AG_IO_ADC1_FIRST_GPIO < 0) {
        return -AG_ENOTSUP;
    }
    if (channel < 0 || channel >= SOC_ADC_MAX_CHANNEL_NUM) {
        return -AG_ERANGE;
    }

    lock();

    if (s_adc1 == NULL) {
        const adc_oneshot_unit_init_cfg_t init = {
            .unit_id = ADC_UNIT_1,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        const ag_err_t err = from_esp(adc_oneshot_new_unit(&init, &s_adc1));
        if (err != AG_OK) {
            unlock();
            return err;
        }
    }

    /* The pin belongs to whoever measures on it, so that two processes do not
     * fight over the attenuation setting of one channel. */
    const int    pin = AG_IO_ADC1_FIRST_GPIO + channel;
    ag_err_t err = ag_io_claim(pin, caller(), "adc");
    if (err != AG_OK) {
        unlock();
        return err;
    }

    if (!s_adc_chan[channel]) {
        const adc_oneshot_chan_cfg_t chan = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            /* The widest range: measuring a 3.3 V rail at 0 dB reads full
             * scale and says nothing, which is the mistake everybody makes. */
            .atten = ADC_ATTEN_DB_12,
        };
        err = from_esp(
            adc_oneshot_config_channel(s_adc1, (adc_channel_t)channel, &chan));
        if (err != AG_OK) {
            unlock();
            return err;
        }
        s_adc_chan[channel] = true;
    }

    int      raw = 0;
    ag_err_t rc = from_esp(
        adc_oneshot_read(s_adc1, (adc_channel_t)channel, &raw));
    unlock();
    return (rc == AG_OK) ? (int32_t)raw : rc;
}

#endif /* CONFIG_ARGON_ENABLE_ADC */

/* ---------------------------------------------------------------------- */
/* PWM                                                                    */
/* ---------------------------------------------------------------------- */

typedef struct {
    int      pin;
    uint8_t  bits;
    bool     used;
} pwm_channel_t;

static pwm_channel_t s_pwm[AG_IO_PWM_CHANNELS];

static int pwm_find(int pin)
{
    for (int i = 0; i < AG_IO_PWM_CHANNELS; i++) {
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
    (void)ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, 0);
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
        for (int i = 0; i < AG_IO_PWM_CHANNELS; i++) {
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

    /*
     * One timer per channel, so that two pins can run at different rates.  The
     * chip has four; beyond that, channels share and the last frequency wins -
     * which is a limit worth knowing rather than hiding.
     */
    const ledc_timer_t     timer = (ledc_timer_t)(ch % LEDC_TIMER_MAX);
    const ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = timer,
        .duty_resolution = (ledc_timer_bit_t)bits,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    err = from_esp(ledc_timer_config(&tcfg));

    if (err == AG_OK) {
        const ledc_channel_config_t ccfg = {
            .gpio_num = pin,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)ch,
            .timer_sel = timer,
            .duty = 0,
            .hpoint = 0,
        };
        err = from_esp(ledc_channel_config(&ccfg));
    }

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

    ag_err_t err = from_esp(
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, duty));
    if (err == AG_OK) {
        err = from_esp(ledc_update_duty(LEDC_LOW_SPEED_MODE,
                                        (ledc_channel_t)ch));
    }
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
        (void)gpio_isr_handler_remove((gpio_num_t)pin);
        (void)gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_DISABLE);
    }
    pwm_drop(pin);
    /* Back to a high-impedance input: an output left driving is a pin fighting
     * whatever is connected to it. */
    (void)gpio_reset_pin((gpio_num_t)pin);
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
#if CONFIG_ARGON_ENABLE_ADC
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
        s_lock = xSemaphoreCreateRecursiveMutex();
        if (s_lock == NULL) {
            return -AG_ENOMEM;
        }
    }

    const ag_err_t err = ag_io_claims_init(SOC_GPIO_PIN_COUNT);
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
