/*
 * ArgonOS - board description.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/board.h>

#include <stdio.h>
#include <string.h>

#include <argon/path.h>

#include "sdkconfig.h"

static ag_board_t s_board;

/*
 * Generic defaults.  The SD pins are the assignment the original ESP32 used for
 * slot 1 and that most later boards copied; it is a starting point, not a fact
 * about any particular board, which is what BOARD.CFG is for.
 */
static void apply_generic_defaults(void)
{
    memset(&s_board, 0, sizeof(s_board));
    strcpy(s_board.name, "generic");

    s_board.console_cols = 80;
    s_board.console_rows = 25;

    strcpy(s_board.display.driver, "soft");
    s_board.display.width = 320;
    s_board.display.height = 240;

    s_board.sd.kind = AG_SD_SDMMC;
    s_board.sd.clk = 14;
    s_board.sd.cmd = 15;
    s_board.sd.d0 = 2;
    s_board.sd.d1 = 4;
    s_board.sd.d2 = 12;
    s_board.sd.d3 = 13;
    s_board.sd.width = 1; /* one line works on every wiring; four needs all of them */
    s_board.sd.card_detect = AG_PIN_NONE;
    s_board.sd.max_khz = 20000;

    s_board.sd.sck = 12;
    s_board.sd.mosi = 11;
    s_board.sd.miso = 13;
    s_board.sd.cs = 10;
    s_board.sd.spi_host = 2; /* SPI2, as the datasheet numbers it */

    /*
     * Buses start unwired, and they stay that way until BOARD.CFG says
     * otherwise.  Guessing here would mean driving pins on a board nobody has
     * described - see the note in board.h.
     */
    for (int i = 0; i < AG_I2C_BUSES; i++) {
        s_board.i2c[i].sda = AG_PIN_NONE;
        s_board.i2c[i].scl = AG_PIN_NONE;
        s_board.i2c[i].khz = 100;
        s_board.i2c[i].pullups = true;
    }
    for (int i = 0; i < AG_SPI_BUSES; i++) {
        s_board.spi[i].sck = AG_PIN_NONE;
        s_board.spi[i].mosi = AG_PIN_NONE;
        s_board.spi[i].miso = AG_PIN_NONE;
        s_board.spi[i].khz = 1000;
    }
    for (int i = 0; i < AG_UART_PORTS; i++) {
        s_board.uart[i].tx = AG_PIN_NONE;
        s_board.uart[i].rx = AG_PIN_NONE;
        s_board.uart[i].baud = 115200;
    }
}

ag_err_t ag_board_init(void)
{
    apply_generic_defaults();
    /*
     * Compiled-in board packs land here, chosen by NVS, efuse or autodetect.
     * Until there are any, every board is the generic one plus BOARD.CFG.
     */
    return AG_OK;
}

const ag_board_t *ag_board(void) { return &s_board; }

/* ---------------------------------------------------------------------- */

static int16_t cfg_pin(const ag_cfg_t *cfg, const char *key, int16_t fallback)
{
    const int32_t v = ag_cfg_get_int(cfg, key, fallback);
    if (v < -1 || v > 63) {
        return fallback; /* not a pin number; keep what we had */
    }
    return (int16_t)v;
}

/*
 * The bus sections, named after the chip's own numbering: [i2c0], [spi2],
 * [uart1].  Keys are read into a buffer rather than written out one at a time
 * because there are three families of them and the difference between the
 * families is two field names.
 */
static uint32_t cfg_rate(const ag_cfg_t *cfg, const char *key, uint32_t now,
                         uint32_t max)
{
    const int32_t v = ag_cfg_get_int(cfg, key, (int32_t)now);
    if (v <= 0 || (uint32_t)v > max) {
        return now;
    }
    return (uint32_t)v;
}

static void apply_bus_config(const ag_cfg_t *cfg)
{
    char key[32];

    for (int i = 0; i < AG_I2C_BUSES; i++) {
        ag_board_i2c_t *bus = &s_board.i2c[i];

        snprintf(key, sizeof(key), "i2c%d.sda", i);
        bus->sda = cfg_pin(cfg, key, bus->sda);
        snprintf(key, sizeof(key), "i2c%d.scl", i);
        bus->scl = cfg_pin(cfg, key, bus->scl);
        snprintf(key, sizeof(key), "i2c%d.khz", i);
        bus->khz = cfg_rate(cfg, key, bus->khz, 1000);
        snprintf(key, sizeof(key), "i2c%d.pullups", i);
        bus->pullups = ag_cfg_get_bool(cfg, key, bus->pullups);
    }

    for (int i = 0; i < AG_SPI_BUSES; i++) {
        ag_board_spi_t *bus = &s_board.spi[i];
        const int       chip = AG_SPI_FIRST + i;

        snprintf(key, sizeof(key), "spi%d.sck", chip);
        bus->sck = cfg_pin(cfg, key, bus->sck);
        snprintf(key, sizeof(key), "spi%d.mosi", chip);
        bus->mosi = cfg_pin(cfg, key, bus->mosi);
        snprintf(key, sizeof(key), "spi%d.miso", chip);
        bus->miso = cfg_pin(cfg, key, bus->miso);
        snprintf(key, sizeof(key), "spi%d.khz", chip);
        bus->khz = cfg_rate(cfg, key, bus->khz, 80000);
    }

    /* UART0 is the console and is not configurable here: the pins it uses are
     * the ones the system is talking to you over. */
    for (int i = 1; i < AG_UART_PORTS; i++) {
        ag_board_uart_t *port = &s_board.uart[i];

        snprintf(key, sizeof(key), "uart%d.tx", i);
        port->tx = cfg_pin(cfg, key, port->tx);
        snprintf(key, sizeof(key), "uart%d.rx", i);
        port->rx = cfg_pin(cfg, key, port->rx);
        snprintf(key, sizeof(key), "uart%d.baud", i);
        port->baud = cfg_rate(cfg, key, port->baud, 5000000);
    }
}

ag_err_t ag_board_apply_config(const ag_cfg_t *cfg)
{
    if (cfg == NULL) {
        return -AG_EINVAL;
    }

    const char *name = ag_cfg_get(cfg, "board.name", NULL);
    if (name != NULL && name[0] != '\0') {
        snprintf(s_board.name, sizeof(s_board.name), "%s", name);
    }

    const char *kind = ag_cfg_get(cfg, "sd.interface", NULL);
    if (kind != NULL) {
        if (ag_path_icmp(kind, "sdmmc") == 0) {
            s_board.sd.kind = AG_SD_SDMMC;
        } else if (ag_path_icmp(kind, "spi") == 0) {
            s_board.sd.kind = AG_SD_SPI;
        } else if (ag_path_icmp(kind, "none") == 0) {
            s_board.sd.kind = AG_SD_NONE;
        }
    }

    s_board.sd.clk = cfg_pin(cfg, "sd.clk", s_board.sd.clk);
    s_board.sd.cmd = cfg_pin(cfg, "sd.cmd", s_board.sd.cmd);
    s_board.sd.d0 = cfg_pin(cfg, "sd.d0", s_board.sd.d0);
    s_board.sd.d1 = cfg_pin(cfg, "sd.d1", s_board.sd.d1);
    s_board.sd.d2 = cfg_pin(cfg, "sd.d2", s_board.sd.d2);
    s_board.sd.d3 = cfg_pin(cfg, "sd.d3", s_board.sd.d3);
    s_board.sd.sck = cfg_pin(cfg, "sd.sck", s_board.sd.sck);
    s_board.sd.mosi = cfg_pin(cfg, "sd.mosi", s_board.sd.mosi);
    s_board.sd.miso = cfg_pin(cfg, "sd.miso", s_board.sd.miso);
    s_board.sd.cs = cfg_pin(cfg, "sd.cs", s_board.sd.cs);
    s_board.sd.card_detect = cfg_pin(cfg, "sd.card_detect",
                                     s_board.sd.card_detect);

    const int32_t width = ag_cfg_get_int(cfg, "sd.width", s_board.sd.width);
    if (width == 1 || width == 4) {
        s_board.sd.width = (uint8_t)width;
    }

    const int32_t khz = ag_cfg_get_int(cfg, "sd.max_khz",
                                       (int32_t)s_board.sd.max_khz);
    if (khz > 0 && khz <= 80000) {
        s_board.sd.max_khz = (uint32_t)khz;
    }

    const int32_t host = ag_cfg_get_int(cfg, "sd.spi_host",
                                        s_board.sd.spi_host);
    if (host >= AG_SPI_FIRST && host < AG_SPI_FIRST + AG_SPI_BUSES) {
        s_board.sd.spi_host = (uint8_t)host;
    }

    apply_bus_config(cfg);

    const char *disp = ag_cfg_get(cfg, "display.driver", NULL);
    if (disp != NULL && disp[0] != '\0') {
        snprintf(s_board.display.driver, sizeof(s_board.display.driver), "%s",
                 disp);
    }
    const int32_t dw = ag_cfg_get_int(cfg, "display.width",
                                      (int32_t)s_board.display.width);
    if (dw >= 0 && dw <= 800) {
        s_board.display.width = (uint16_t)dw;
    }
    const int32_t dh = ag_cfg_get_int(cfg, "display.height",
                                      (int32_t)s_board.display.height);
    if (dh >= 0 && dh <= 480) {
        s_board.display.height = (uint16_t)dh;
    }

    return AG_OK;
}
