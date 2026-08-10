/*
 * ArgonOS - board description.
 *
 * One firmware image has to come up on boards that wire the same peripherals to
 * different pins.  The description is built in three layers, each overriding
 * the last:
 *
 *   1. generic defaults for the chip,
 *   2. a compiled-in board pack, selected by NVS, efuse or autodetect,
 *   3. BOARD.CFG on internal flash.
 *
 * Note what is NOT in that list: BOARD.CFG on the SD card.  The pins needed to
 * reach the card cannot be read from a file on the card, so anything required
 * to mount removable media has to come from the first two layers or from
 * internal flash.  Everything discovered later - displays, sensors, buses -
 * can be configured from either place.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_BOARD_H
#define ARGON_BOARD_H

#include <argon/abi.h>
#include <argon/cfg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AG_SD_NONE = 0, /* the board has no card slot            */
    AG_SD_SDMMC,    /* native SD host, 1 or 4 data lines     */
    AG_SD_SPI,      /* card on a SPI bus, slower but frees pins */
} ag_sd_kind_t;

/* -1 means "not connected"; every pin field uses that convention. */
#define AG_PIN_NONE (-1)

typedef struct {
    ag_sd_kind_t kind;

    /* SDMMC */
    int16_t clk;
    int16_t cmd;
    int16_t d0;
    int16_t d1;
    int16_t d2;
    int16_t d3;
    uint8_t width; /* 1 or 4 */

    /* SD over SPI */
    int16_t sck;
    int16_t mosi;
    int16_t miso;
    int16_t cs;
    uint8_t spi_host;

    /* Card detect, when the board has one.  Without it, removal is noticed
     * only when a transfer fails. */
    int16_t card_detect;

    uint32_t max_khz;
} ag_board_sd_t;

/*
 * The buses an application or a driver reaches through api->io.
 *
 * There are no default pins, and that is the whole point.  A bus is wired to
 * whatever the board designer wired it to, and driving a pin on a guess is not
 * a failed read - it is an output fighting another output.  An unconfigured bus
 * answers -AG_ENODEV and says which key would configure it.
 *
 * Numbering follows the chip, not an enumeration inside ESP-IDF: `[i2c0]`,
 * `[spi2]`, `[uart1]` mean I2C0, SPI2 and UART1 as the datasheet names them.
 */
#define AG_I2C_BUSES 2  /* I2C0, I2C1                                       */
#define AG_SPI_BUSES 2  /* SPI2, SPI3; SPI1 belongs to the flash            */
#define AG_UART_PORTS 3 /* UART0 is the console                             */

#define AG_SPI_FIRST 2 /* the chip's number of the first usable SPI         */

/*
 * ESP-IDF numbers its SPI hosts from SPI1_HOST = 0, so the chip's SPI2 is host
 * 1.  Everything ArgonOS shows or accepts - BOARD.CFG, the ABI, the `dev`
 * listing - uses the chip's own number, because that is what the schematic
 * says; this macro is the single place the two meet.  Without it the off-by-one
 * lands as "SPI3 works and SPI2 does not", which is not a clue.
 */
#define AG_SPI_HOST_OF(chip) ((chip) - 1)

typedef struct {
    int16_t  sda;
    int16_t  scl;
    uint32_t khz;
    bool     pullups; /* switch on the internal ones; weak, but often enough */
} ag_board_i2c_t;

typedef struct {
    int16_t  sck;
    int16_t  mosi;
    int16_t  miso;
    uint32_t khz;
} ag_board_spi_t;

typedef struct {
    int16_t  tx;
    int16_t  rx;
    uint32_t baud;
} ag_board_uart_t;

typedef struct {
    /*
     * Soft framebuffer by default (QEMU and boards without a panel driver).
     * `driver` is "soft", "none", or a panel name for a future .SYS / static
     * driver.  width/height 0 means the soft default (320x240).
     */
    char     driver[16];
    uint16_t width;
    uint16_t height;
} ag_board_display_t;

/*
 * I2S TX to an external DAC (MAX98357, PCM5102, …).  All pins AG_PIN_NONE
 * means the kernel keeps a discard stub (QEMU / no codec wired).
 */
typedef struct {
    char     driver[12]; /* "auto", "i2s", "stub" */
    int16_t  bclk;
    int16_t  ws;
    int16_t  dout;
    int16_t  mclk; /* optional; AG_PIN_NONE if unused */
    uint32_t rate; /* default sample rate hint; apps may reopen */
} ag_board_audio_t;

typedef struct {
    char              name[24];
    ag_board_sd_t     sd;
    ag_board_i2c_t    i2c[AG_I2C_BUSES];
    ag_board_spi_t    spi[AG_SPI_BUSES];
    ag_board_uart_t   uart[AG_UART_PORTS];
    ag_board_display_t display;
    ag_board_audio_t  audio;
    uint16_t          console_cols;
    uint16_t          console_rows;
} ag_board_t;

/* Applies the generic defaults for the chip, then any compiled-in board pack. */
ag_err_t ag_board_init(void);

/*
 * Applies overrides from a parsed configuration.  Called once internal flash
 * is mounted, which is why it is separate from ag_board_init().
 */
ag_err_t ag_board_apply_config(const ag_cfg_t *cfg);

const ag_board_t *ag_board(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_BOARD_H */
