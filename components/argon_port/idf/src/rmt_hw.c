/*
 * ArgonOS port: ESP-IDF - RMT driving a WS2812 LED chain.
 *
 * The channel runs at 10 MHz, so one tick is 0.1 us and the WS2812's bit cells
 * fall on whole ticks: a 0 bit is 0.3 us high then 0.9 us low, a 1 bit the other
 * way round.  A bytes-encoder turns each colour byte into eight such cells,
 * most-significant bit first, which is the order the LED latches them.  After
 * the last byte the line is held low past the ~50 us reset window so the strip
 * takes the frame rather than treating the next transmit as a continuation.
 *
 * Opened and closed around the one transmit, like the CAN command: an LED strip
 * is not something the system holds a channel open for between updates.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdkconfig.h"

#if defined(CONFIG_ARGON_RMT) && CONFIG_ARGON_RMT

#include <argon/port/rmt.h>

#include "freertos/FreeRTOS.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_rom_sys.h"

#define WS2812_RES_HZ 10000000u /* 0.1 us per tick */

int ag_port_rmt_ws2812(int pin, const uint8_t *grb, int len)
{
    if (grb == NULL || len <= 0) {
        return -1;
    }

    rmt_channel_handle_t chan = NULL;
    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = pin,
        .mem_block_symbols = 64,
        .resolution_hz = WS2812_RES_HZ,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&tx_cfg, &chan) != ESP_OK) {
        return -1;
    }

    /* 0.3 us / 0.9 us cells; well inside the WS2812B +/-150 ns tolerance. */
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags = {.msb_first = 1},
    };
    rmt_encoder_handle_t enc = NULL;
    if (rmt_new_bytes_encoder(&enc_cfg, &enc) != ESP_OK) {
        rmt_del_channel(chan);
        return -1;
    }

    int rc = 0;
    if (rmt_enable(chan) != ESP_OK) {
        rc = -1;
        goto done;
    }
    rmt_transmit_config_t txc = {.loop_count = 0};
    if (rmt_transmit(chan, enc, grb, (size_t)len, &txc) != ESP_OK ||
        rmt_tx_wait_all_done(chan, 1000) != ESP_OK) {
        rc = -1;
    }
    esp_rom_delay_us(80); /* hold the reset latch */
    rmt_disable(chan);

done:
    rmt_del_encoder(enc);
    rmt_del_channel(chan);
    return rc;
}

#endif /* CONFIG_ARGON_RMT */
