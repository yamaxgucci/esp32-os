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
    s_board.sd.spi_host = 1; /* SPI2_HOST */
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
    if (host >= 1 && host <= 3) {
        s_board.sd.spi_host = (uint8_t)host;
    }

    return AG_OK;
}
