/*
 * ArgonOS - XPT2046 resistive touch (.SYS).
 *
 *   drv install a:\xpt2046.sys
 *   dev                        -> touch0  input  XPT2046
 *
 * A finger becomes a pointer event in console cells, which is what the
 * terminal's own mouse reports produce, so anything that already understands a
 * mouse understands this without knowing it exists.
 *
 * The controller is on the *display's* bus.
 *
 * That was not obvious and cost most of a day: this board's 2.8 inch cousin
 * gives the XPT2046 four pins of its own (25/32/39/33), every description of
 * the family says so, and bit-banging those here reads zeros forever.  The
 * 2.4 inch board has fewer pins to spare and hangs the controller off the same
 * three wires as the panel, with a chip select of its own.  What settled it was
 * a probe that ran instead of the operating system (main/probe.c) and tried
 * every wiring in turn - with the system running there were four possible
 * culprits and no way to separate them.
 *
 * Sharing the bus is why this talks through io->spi_xfer rather than toggling
 * pins: those pins belong to the SPI peripheral and to the panel driver.  It
 * is also why io->spi_config exists - the panel runs at 40 MHz and this part
 * stops answering above about two.
 *
 * Build:
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32-elf-gcc \
 *       --include sdk/include -o build/apps/XPT2046.SYS apps/xpt2046/xpt2046.c
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_DRV("XPT2046", "0.2", "argon");

#define T_BUS  2   /* SPI2, the display's bus - pins from BOARD.CFG [spi2]  */
#define T_CS  33
#define T_IRQ 36   /* pen down, active low; input-only pin, external pull-up */
#define T_KHZ 2000 /* the datasheet's ceiling, and the panel's is 40000      */

/*
 * The panel this sits on.  The *grid* is not here, and that is the point.
 *
 * An input driver reports cells, and the kernel turns them back into pixels
 * with the console's own grid: cw = surface_w / cols, ch = surface_h / rows
 * (ag_input_to_pixels).  This driver used to divide by a grid of its own -
 * the panel's forty by thirty of 8x8 - and the two agree only while the
 * console happens to be forty by thirty as well.  With a console of forty by
 * twenty-five the kernel multiplies each row by nine, and the pointer lands
 * below the stylus, further below the further down the glass you touch.
 * Which is what Maxim saw, and what no amount of looking at this driver's own
 * arithmetic would have explained: both halves were self-consistent and they
 * were not talking about the same grid.
 *
 * So the grid is asked for, at every poll, from the console itself.
 */
#define PANEL_W  320
#define PANEL_H  240
#define CELLS_MAX 255

/*
 * Raw readings at the edges of the glass.  A resistive panel is a pair of
 * potentiometers and these are where its ends are; they vary between panels of
 * the same model, so they are a starting point rather than a fact.  Anything
 * outside is clamped rather than dropped: a touch in the last millimetre of
 * the screen is still a touch.
 */
#define RAW_MIN 300
#define RAW_MAX 3800

/* Below this the glass is not being pressed hard enough to trust. */
#define Z_MIN 200

/* Start bit, channel, 12-bit differential mode, power down between reads. */
#define CMD_X  0xd0u
#define CMD_Y  0x90u
#define CMD_Z1 0xb0u
#define CMD_Z2 0xc0u

static const ag_io_api_t *io;

static struct {
    bool     down;
    int16_t  col, row;
    uint32_t samples;
} s_state;

/* ---- the wire ---------------------------------------------------------- */

/*
 * One command and its answer in a single transfer, because chip select has to
 * stay low across both and the SPI layer asserts it per transfer.  The first
 * byte back is the controller's response to the command byte and is discarded;
 * the twelve bits that matter are the top of the next two, one bit late.
 */
static uint16_t read_once(uint8_t cmd)
{
    const uint8_t tx[3] = {cmd, 0x00, 0x00};
    uint8_t       rx[3] = {0, 0, 0};

    if (io->spi_xfer(T_BUS, T_CS, tx, rx, sizeof(tx)) != AG_OK) {
        return 0;
    }
    return (uint16_t)((((uint16_t)rx[1] << 8) | rx[2]) >> 3) & 0x0fffu;
}

/*
 * The median of three, not an average: a resistive panel gives the occasional
 * wild reading as the contact settles, and one of those in an average moves
 * the pointer across the screen.  The middle of three throws it away.
 */
static uint16_t read3(uint8_t cmd)
{
    const uint16_t a = read_once(cmd);
    const uint16_t b = read_once(cmd);
    const uint16_t c = read_once(cmd);

    if ((a <= b && b <= c) || (c <= b && b <= a)) {
        return b;
    }
    if ((b <= a && a <= c) || (c <= a && a <= b)) {
        return a;
    }
    return c;
}

/* Pressure as the datasheet computes it; only "is it touched" is used. */
static uint16_t pressure(uint16_t x, uint16_t z1, uint16_t z2)
{
    if (z1 == 0) {
        return 0;
    }
    const int32_t z =
        ((int32_t)x * ((int32_t)z2 * 1000 / (int32_t)z1 - 1000)) / 4096;
    return (z < 0) ? 0 : (uint16_t)z;
}

/*
 * Which way each axis runs, relative to the picture on the glass.
 *
 * Both of them backwards here, and that is one fact about how this panel was
 * assembled rather than two: the controller's origin is at the corner opposite
 * the one the display driver calls (0,0).  Measured, not derived - a drag left
 * to right along the top came back as column 39 down to 11 on row 28 of 29.
 */
#define FLIP_X 1
#define FLIP_Y 1

/*
 * A raw reading to a cell, on an axis of `n` cells over `span` pixels.
 *
 * The division is by span/n rather than by any fixed cell size, because
 * span/n is the number the kernel multiplies by on the way back.
 */
static int16_t to_cell(uint16_t raw, int span, int n)
{
    int v = (int)raw;
    if (v < RAW_MIN) {
        v = RAW_MIN;
    }
    if (v > RAW_MAX) {
        v = RAW_MAX;
    }
    if (n <= 0) {
        return 0;
    }
    const int px = ((v - RAW_MIN) * span) / (RAW_MAX - RAW_MIN);
    const int cell = (span > n) ? (span / n) : 1;
    int       c = px / cell;
    if (c < 0) {
        c = 0;
    }
    if (c >= n) {
        c = n - 1;
    }
    return (int16_t)c;
}

/* The console's grid, which is the unit this driver reports in. */
static void grid(int *cols, int *rows)
{
    ag_coninfo_t ci = { 0 };
    ag_coninfo(&ci);
    *cols = (ci.cols > 0 && ci.cols <= CELLS_MAX) ? (int)ci.cols : 40;
    *rows = (ci.rows > 0 && ci.rows <= CELLS_MAX) ? (int)ci.rows : 30;
}

/* ---- the class vtable -------------------------------------------------- */

static int32_t touch_poll(ag_handle_t h, ag_event_t *out, uint32_t max)
{
    (void)h;
    if (out == NULL || max == 0) {
        return 0;
    }

    /*
     * The pen line first, and usually only that: it is one GPIO read against
     * four conversions, and this runs a hundred times a second forever.  It is
     * trusted only in the "nothing is happening" direction - a low line still
     * has to be confirmed by pressure, because it also goes low while the
     * controller is converting.
     */
    if (io->gpio_read(T_IRQ) != 0) {
        if (!s_state.down) {
            return 0;
        }
        s_state.down = false;
        out[0].type = AG_EV_POINTER_UP;
        out[0].ptr.x = s_state.col;
        out[0].ptr.y = s_state.row;
        out[0].ptr.buttons = 0;
        return 1;
    }

    const uint16_t z1 = read3(CMD_Z1);
    const uint16_t z2 = read3(CMD_Z2);
    const uint16_t rx = read3(CMD_X);
    const uint16_t ry = read3(CMD_Y);
    s_state.samples++;

    if (pressure(rx, z1, z2) < Z_MIN) {
        return 0;
    }

    /*
     * Which raw axis is the screen's across.
     *
     * The panel is mounted landscape (MADCTL 0x40 in the display driver) and
     * the controller is not, so one of the two is turned relative to the
     * other - but which way is a fact about how the glass was glued on, not
     * something to reason out.  It was reasoned out first, wrongly: a stylus
     * drawn horizontally left a vertical line.
     */
    int cols, rows;
    grid(&cols, &rows);

    int16_t col = to_cell(rx, PANEL_W, cols);
    int16_t row = to_cell(ry, PANEL_H, rows);

#if FLIP_X
    col = (int16_t)(cols - 1 - col);
#endif
#if FLIP_Y
    row = (int16_t)(rows - 1 - row);
#endif

    if (!s_state.down) {
        s_state.down = true;
        out[0].type = AG_EV_POINTER_DOWN;
    } else if (col != s_state.col || row != s_state.row) {
        out[0].type = AG_EV_POINTER_MOVE;
    } else {
        return 0; /* still down, still the same cell: nothing happened */
    }

    out[0].ptr.buttons = 1;
    out[0].ptr.x = col;
    out[0].ptr.y = row;
    out[0].ptr.dx = (int16_t)(col - s_state.col);
    out[0].ptr.dy = (int16_t)(row - s_state.row);

    s_state.col = col;
    s_state.row = row;
    return 1;
}

static const ag_input_ops_t k_input_ops = {
    .size = sizeof(ag_input_ops_t),
    .poll = touch_poll,
};

static const ag_dev_ops_t k_dev_ops = {0};

ag_err_t ag_driver_init(void)
{
    io = ag_api()->io;
    if (io == NULL || !AG_HAS(io, spi_xfer)) {
        return -AG_ENOTSUP;
    }
    if (!AG_HAS(io, spi_config)) {
        /* Without it the panel's 40 MHz would be used and this part would
         * answer noise, which is worse than not loading. */
        ag_printf("XPT2046: this kernel has no io->spi_config (ABI < 0.29)\n");
        return -AG_ENOTSUP;
    }

    ag_err_t err = io->spi_config(T_BUS, T_CS, T_KHZ);
    if (err != AG_OK) {
        ag_printf("XPT2046: spi2 cs %d at %d kHz: %s\n", T_CS, T_KHZ,
                  ag_strerror(err));
        return err;
    }
    err = io->gpio_config(T_IRQ, AG_GPIO_IN);
    if (err != AG_OK) {
        return err;
    }

    /* One conversion, so the pen line is armed: it stays high until the
     * controller has seen a command. */
    (void)read_once(CMD_Z1);

    const ag_dev_add_t desc = {
        .name = "touch0",
        .driver = "XPT2046",
        .cls = AG_DEV_INPUT,
        .ops = &k_dev_ops,
        .class_ops = &k_input_ops,
        .priv = NULL,
    };
    err = ag_dev_add(&desc);
    if (err != AG_OK) {
        return err;
    }

    int cols, rows;
    grid(&cols, &rows);
    ag_printf("XPT2046: spi%d cs %d at %d kHz, pen %d, %dx%d cells\n", T_BUS,
              T_CS, T_KHZ, T_IRQ, cols, rows);
    return AG_OK;
}
