/*
 * ArgonOS - board probe: talk to the hardware before there is a system.
 *
 * Enabled by CONFIG_ARGON_BOARD_PROBE, off by default.  When it is on this
 * runs instead of the kernel: no scheduler to share, no pin table to claim
 * from, no console task doing anything else, no driver that could be at
 * fault.  Just the pins, the UART, and a loop.
 *
 * It exists because a touchscreen that answered nothing had four possible
 * explanations at once - the wiring, the driver, the way the kernel polls it,
 * and the pin claims - and no way to tell them apart.  This removes three of
 * them.  A bring-up tool of this shape is worth keeping: the next unknown
 * peripheral will want the same thing.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_BOARD_PROBE) && CONFIG_ARGON_BOARD_PROBE

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Every wiring this family of boards is known to use for its touch
 * controller.  The 2.8 inch model gives the XPT2046 four pins of its own; the
 * 2.4 inch model has fewer to spare, and the obvious thing for it to do is
 * hang the controller off the display's bus with a chip select of its own.
 * Both are guesses until one of them answers.
 */
typedef struct {
    const char *name;
    int         clk, mosi, miso, cs;
} wiring_t;

static const wiring_t k_wirings[] = {
    {"own pins  ", 25, 32, 39, 33},
    {"lcd bus   ", 14, 13, 12, 33},
    {"lcd bus/25", 14, 13, 12, 25},
    {"sd bus    ", 18, 23, 19, 33},
    {"sd bus/25 ", 18, 23, 19, 25},
};

/*
 * Every chip select on the board, parked high before anything is clocked.
 *
 * Without this the answer is unreadable: the display and the card sit on the
 * same MISO lines as the thing being looked for, and a chip select left
 * floating after reset lets one of them drive the wire.  The first run of this
 * probe read zeros on the display's bus and they were the display's zeros.
 */
static const int k_chip_selects[] = {5, 15, 25, 33};

static void park_chip_selects(void)
{
    for (unsigned i = 0; i < sizeof(k_chip_selects) / sizeof(k_chip_selects[0]);
         i++) {
        gpio_config_t cs = {
            .pin_bit_mask = (1ULL << k_chip_selects[i]),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&cs);
        gpio_set_level((gpio_num_t)k_chip_selects[i], 1);
    }
}

/* Slow on purpose: the controller accepts 2 MHz and the wires are unknown. */
static void tick(int clk, int level)
{
    gpio_set_level((gpio_num_t)clk, level);
    esp_rom_delay_us(2);
}

static uint16_t xfer(const wiring_t *w, uint8_t cmd)
{
    uint16_t value = 0;

    for (int i = 7; i >= 0; i--) {
        gpio_set_level((gpio_num_t)w->mosi, (cmd >> i) & 1);
        tick(w->clk, 1);
        tick(w->clk, 0);
    }
    tick(w->clk, 1);
    tick(w->clk, 0);
    for (int i = 0; i < 12; i++) {
        tick(w->clk, 1);
        value = (uint16_t)((value << 1) | (gpio_get_level((gpio_num_t)w->miso) & 1));
        tick(w->clk, 0);
    }
    return value;
}

static void setup(const wiring_t *w)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << w->clk) | (1ULL << w->mosi) | (1ULL << w->cs),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << w->miso),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in);

    gpio_set_level((gpio_num_t)w->cs, 1);
    gpio_set_level((gpio_num_t)w->clk, 0);
}

static void read_once(const wiring_t *w, uint16_t *x, uint16_t *y, uint16_t *z1)
{
    gpio_set_level((gpio_num_t)w->cs, 0);
    esp_rom_delay_us(5);
    *z1 = xfer(w, 0xb0);
    *x = xfer(w, 0xd0);
    *y = xfer(w, 0x90);
    gpio_set_level((gpio_num_t)w->cs, 1);
}

void ag_board_probe(void)
{
    printf("\n\n=== ArgonOS board probe: touch ===\n");
    printf("press the screen; every wiring is read ten times a second\n");
    printf("pen line (GPIO 36) is read as well, without a pull-up of ours\n\n");

    gpio_config_t pen = {
        .pin_bit_mask = (1ULL << 36),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&pen);
    park_chip_selects();

    for (unsigned round = 0;; round++) {
        for (unsigned i = 0; i < sizeof(k_wirings) / sizeof(k_wirings[0]); i++) {
            const wiring_t *w = &k_wirings[i];
            uint16_t        x = 0, y = 0, z1 = 0;

            park_chip_selects(); /* only this wiring's chip may answer */
            setup(w);
            read_once(w, &x, &y, &z1);
            park_chip_selects();
            printf("%s clk=%2d mosi=%2d miso=%2d cs=%2d -> x=%4u y=%4u z1=%4u\n",
                   w->name, w->clk, w->mosi, w->miso, w->cs, (unsigned)x,
                   (unsigned)y, (unsigned)z1);
        }
        printf("pen(36)=%d   [round %u]\n\n", gpio_get_level(GPIO_NUM_36),
               round);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

#endif /* CONFIG_ARGON_BOARD_PROBE */
