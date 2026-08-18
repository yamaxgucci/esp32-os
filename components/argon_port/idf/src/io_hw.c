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
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_log.h"
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

typedef struct {
    spi_device_handle_t dev;
    int                 dev_cs;
    uint32_t            khz;
    uint8_t            *bounce; /* tx half then rx half, internal and DMA-able */
    bool                up;
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
        return AG_OK;
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

    s_spi[bus].bounce = ag_port_alloc(2u * AG_PORT_SPI_MAX_XFER, AG_MEM_DMA);
    if (s_spi[bus].bounce == NULL) {
        (void)spi_bus_free(AG_PORT_SPI_HOST_OF(bus));
        return -AG_ENOMEM;
    }

    s_spi[bus].khz = khz;
    s_spi[bus].dev_cs = -1;
    s_spi[bus].up = true;
    return AG_OK;
}

static ag_err_t spi_device_for(int bus, int cs, spi_device_handle_t *out)
{
    if (!spi_valid(bus) || !s_spi[bus].up) {
        return -AG_ENODEV;
    }
    if (s_spi[bus].dev != NULL && s_spi[bus].dev_cs == cs) {
        *out = s_spi[bus].dev;
        return AG_OK;
    }
    if (s_spi[bus].dev != NULL) {
        (void)spi_bus_remove_device(s_spi[bus].dev);
        s_spi[bus].dev = NULL;
        s_spi[bus].dev_cs = -1;
    }

    const spi_device_interface_config_t cfg = {
        .clock_speed_hz = (int)(s_spi[bus].khz * 1000u),
        .mode = 0,
        .spics_io_num = cs,
        .queue_size = 1,
    };

    const ag_err_t err = from_esp(
        spi_bus_add_device(AG_PORT_SPI_HOST_OF(bus), &cfg, &s_spi[bus].dev));
    if (err != AG_OK) {
        return err;
    }

    s_spi[bus].dev_cs = cs;
    *out = s_spi[bus].dev;
    return AG_OK;
}

ag_err_t ag_port_spi_xfer(int bus, int cs, const void *tx, void *rx, size_t len)
{
    if (len == 0 || len > AG_PORT_SPI_MAX_XFER) {
        return -AG_EINVAL;
    }

    spi_device_handle_t dev = NULL;
    ag_err_t            err = spi_device_for(bus, cs, &dev);
    if (err != AG_OK) {
        return err;
    }

    /*
     * Through the bounce buffer, always.  The caller's data is in PSRAM and the
     * DMA engine wants internal memory; deciding case by case would mean two
     * paths, one of which is exercised only on the board nobody has.
     */
    uint8_t *const out = s_spi[bus].bounce;
    uint8_t *const in = out + AG_PORT_SPI_MAX_XFER;

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
    return err;
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
