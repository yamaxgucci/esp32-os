/*
 * ArgonOS port: ESP-IDF - 1-Wire bit-banged on one GPIO.
 *
 * The line is driven by switching the pin's direction, never by writing a high:
 * output-low pulls it down, input lets the pull-up bring it back.  That is what
 * open-drain means here, and it is why a device can answer on the same wire the
 * master just spoke on.
 *
 * The slots are a few microseconds wide, so each byte runs inside a critical
 * section: a task switch or an interrupt in the middle of a slot would stretch
 * it past the window and corrupt the bit.  A byte is ~600 us and a reset ~1 ms
 * of held-off interrupts - long, but this is a foreground command touching one
 * sensor, not something in the audio path.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_ONEWIRE) && CONFIG_ARGON_ONEWIRE

#include <argon/port/onewire.h>

#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

static portMUX_TYPE s_ow_mux = portMUX_INITIALIZER_UNLOCKED;

/* Release the line to the pull-up (input), or hold it down (output-low). */
static inline void ow_release(int pin)
{
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
}

static inline void ow_low(int pin)
{
    gpio_set_level((gpio_num_t)pin, 0);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
}

int ag_port_ow_reset(int pin)
{
    /* A weak internal pull-up so a bare sensor on a short wire still reads;
     * a real bus wants an external 4.7k.  Idle high before the pulse. */
    gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
    ow_release(pin);
    esp_rom_delay_us(10);

    int present;
    portENTER_CRITICAL(&s_ow_mux);
    ow_low(pin);
    esp_rom_delay_us(480);
    ow_release(pin);
    esp_rom_delay_us(70);
    present = (gpio_get_level((gpio_num_t)pin) == 0) ? 1 : 0;
    portEXIT_CRITICAL(&s_ow_mux);

    esp_rom_delay_us(410); /* the rest of the presence window */
    return present;
}

static void ow_write_bit(int pin, int bit)
{
    if (bit) {
        ow_low(pin);
        esp_rom_delay_us(6);
        ow_release(pin);
        esp_rom_delay_us(64);
    } else {
        ow_low(pin);
        esp_rom_delay_us(60);
        ow_release(pin);
        esp_rom_delay_us(10);
    }
}

static int ow_read_bit(int pin)
{
    int bit;
    ow_low(pin);
    esp_rom_delay_us(6);
    ow_release(pin);
    esp_rom_delay_us(9);
    bit = gpio_get_level((gpio_num_t)pin);
    esp_rom_delay_us(55);
    return bit;
}

void ag_port_ow_write(int pin, const uint8_t *buf, int len)
{
    for (int i = 0; i < len; i++) {
        const uint8_t b = buf[i];
        portENTER_CRITICAL(&s_ow_mux);
        for (int bit = 0; bit < 8; bit++) {
            ow_write_bit(pin, (b >> bit) & 1);
        }
        portEXIT_CRITICAL(&s_ow_mux);
    }
}

void ag_port_ow_read(int pin, uint8_t *buf, int len)
{
    for (int i = 0; i < len; i++) {
        uint8_t b = 0;
        portENTER_CRITICAL(&s_ow_mux);
        for (int bit = 0; bit < 8; bit++) {
            if (ow_read_bit(pin)) {
                b |= (uint8_t)(1u << bit);
            }
        }
        portEXIT_CRITICAL(&s_ow_mux);
        buf[i] = b;
    }
}

#endif /* CONFIG_ARGON_ONEWIRE */
