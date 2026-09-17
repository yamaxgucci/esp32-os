/*
 * ArgonOS - CST816D capacitive touch driver (.SYS).
 *
 *   drv install c:\cst816.sys
 *   dev                       -> touch0  input  CST816
 *
 * The glass of the Waveshare ESP32-S3-Touch-LCD-2, and that board's only
 * pointing device: it has one USB socket and the console is on it, so until
 * something is wired to the header there is nothing else to point with.
 *
 * Written beside apps/xpt2046/xpt2046.c, which is the other touch driver in
 * this tree, and deliberately not written like it.  The XPT2046 is a resistive
 * bridge: four conversions, a pressure calculation, a calibration and a pile of
 * thresholds, because the part reports a voltage and the driver has to decide
 * whether that voltage is a finger.  This part has already decided.  It reports
 * a coordinate and a count of fingers, and everything the other driver does
 * with Z_MIN, Z_MIN_HELD and UP_SETTLE_MS has no counterpart here - copying
 * that shape would be inventing uncertainty the hardware does not have.
 *
 * Build:
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o build/apps/CST816.SYS apps/cst816/cst816.c
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_DRV("CST816", "0.1", "argon");

/*
 * The bus, the address and the interrupt line.
 *
 * Constants rather than settings for the reason the panel drivers give: a
 * driver cannot read BOARD.CFG (api->cfg is NULL), and a wrong guess here
 * drives a pin belonging to something else.  They are written down in
 * boards/esp32s3-touch-lcd-2/BOARD.CFG as well, where a person looking for
 * them will look first.
 */
#define T_BUS   0
#define T_ADDR  0x15
#define T_INT   46
#define T_MS    50      /* I2C timeout: this part answers in microseconds */

/*
 * The panel as the rest of the system sees it: 320x240, landscape.
 *
 * The controller does not know that.  Its coordinates are the glass's own
 * portrait 240x320, origin at the corner the flex comes out of, and turning
 * one into the other is this driver's job - the same rotation the panel driver
 * applies with MADCTL, done the other way round.
 */
#define PANEL_W 320
#define PANEL_H 240

#define TOUCH_W 240     /* the controller's own axes */
#define TOUCH_H 320

/*
 * Which way round, and this is a guess in exactly the way the panel's MADCTL
 * is a guess: no document says which end of the flex the glass was attached
 * by, and the two possibilities are one the other's 180-degree rotation.
 *
 * The rule below pairs with MADCTL 0x60 in the panel driver.  If a touch lands
 * in the right place mirrored - top-left where bottom-right should be - these
 * two are what to change, and nothing else is.
 */
#define SWAP_XY 1
#define FLIP_X  0
#define FLIP_Y  1

/* Registers.  The family is CST816S/D/T and CST820; they differ in the chip
 * id and in gestures nobody here uses. */
#define REG_GESTURE 0x01
#define REG_FINGER  0x02
#define REG_XH      0x03
#define REG_CHIPID  0xA7

static const ag_io_api_t *io;

/* What the last poll saw, so an event is an edge rather than a level. */
static struct {
    bool    down;
    int16_t x, y;
} s_state;

/* Counted rather than guessed at, for the same reason the other touch driver
 * counts: "the screen does not respond" has several causes that look alike
 * from the chair, and they are told apart by which of these moves. */
static struct {
    uint32_t polls;     /* poll called                                     */
    uint32_t quiet;     /* the interrupt line said nothing was happening    */
    uint32_t reads;     /* the part was actually asked                      */
    uint32_t failed;    /* and did not answer                               */
    uint32_t none;      /* answered with no finger on the glass             */
    uint32_t sent;      /* events handed to the kernel                      */
} s_tally;

static bool reg_read(uint8_t reg, void *buf, size_t len)
{
    return io->i2c_wrrd(T_BUS, T_ADDR, &reg, 1, buf, len, T_MS) == AG_OK;
}

static int32_t touch_poll(ag_handle_t h, ag_event_t *out, uint32_t max)
{
    (void)h;
    if (out == NULL || max == 0u) {
        return 0;
    }

    s_tally.polls++;

    /*
     * The interrupt line first, and usually only that.
     *
     * The part pulls it low while a finger is on the glass, so a high line
     * with nothing already down means there is nothing to ask about - one GPIO
     * read instead of an I2C transaction, a hundred times a second, on a bus
     * the IMU also uses.
     *
     * It is trusted in one direction only.  A finger that is already down is
     * read anyway: the line pulses rather than holds on some of this family,
     * and a missed release leaves a button stuck, which is the one failure a
     * pointer must not have.
     */
    if (io->gpio_read(T_INT) != 0 && !s_state.down) {
        s_tally.quiet++;
        return 0;
    }

    uint8_t b[5];   /* gesture, fingers, xh, xl, yh */
    uint8_t y[2];

    s_tally.reads++;
    if (!reg_read(REG_GESTURE, b, sizeof(b)) ||
        !reg_read(0x05, y, sizeof(y))) {
        s_tally.failed++;
        return 0;
    }

    const bool touching = (b[1] & 0x0Fu) != 0u;

    if (!touching) {
        s_tally.none++;
        if (!s_state.down) {
            return 0;
        }
        /* The finger has gone: say so where it was last seen, because a
         * release somewhere else is a click somewhere else. */
        s_state.down = false;
        out[0].type = AG_EV_POINTER_UP;
        out[0].ptr.x = s_state.x;
        out[0].ptr.y = s_state.y;
        out[0].ptr.dx = 0;
        out[0].ptr.dy = 0;
        out[0].ptr.buttons = 0;
        s_tally.sent++;
        return 1;
    }

    /* Twelve bits each, the top four of the high byte being flags. */
    const int rx = (int)(((uint16_t)(b[2] & 0x0Fu) << 8) | b[3]);
    const int ry = (int)(((uint16_t)(b[4] & 0x0Fu) << 8) | y[0]);

    int px = rx;
    int py = ry;

#if SWAP_XY
    {
        const int t = px;
        px = py;
        py = t;
    }
#endif
#if FLIP_X
    px = (SWAP_XY ? TOUCH_H : TOUCH_W) - 1 - px;
#endif
#if FLIP_Y
    py = (SWAP_XY ? TOUCH_W : TOUCH_H) - 1 - py;
#endif

    if (px < 0) {
        px = 0;
    }
    if (py < 0) {
        py = 0;
    }
    if (px >= PANEL_W) {
        px = PANEL_W - 1;
    }
    if (py >= PANEL_H) {
        py = PANEL_H - 1;
    }

    const int16_t nx = (int16_t)px;
    const int16_t ny = (int16_t)py;

    if (!s_state.down) {
        s_state.down = true;
        out[0].type = AG_EV_POINTER_DOWN;
        out[0].ptr.dx = 0;
        out[0].ptr.dy = 0;
    } else if (nx != s_state.x || ny != s_state.y) {
        out[0].type = AG_EV_POINTER_MOVE;
        out[0].ptr.dx = (int16_t)(nx - s_state.x);
        out[0].ptr.dy = (int16_t)(ny - s_state.y);
    } else {
        return 0;   /* still down, still in the same place: nothing happened */
    }

    out[0].ptr.x = nx;
    out[0].ptr.y = ny;
    out[0].ptr.buttons = 1;
    s_state.x = nx;
    s_state.y = ny;
    s_tally.sent++;
    return 1;
}

/*
 * Filled at load time rather than written down here: which units this driver
 * reports is a property of the kernel that loaded it, not of the .SYS, and a
 * .SYS outlives the kernel it was built beside.
 */
static ag_input_ops_t k_input_ops = {
    .size = sizeof(ag_input_ops_t),
    .poll = touch_poll,
    .units = AG_PTR_PIXELS,
    .span_w = PANEL_W,
    .span_h = PANEL_H,
};

static const ag_dev_ops_t k_dev_ops = {0};

ag_err_t ag_driver_init(void)
{
    io = ag_api()->io;
    if (io == NULL || !AG_HAS(io, i2c_wrrd)) {
        return -AG_ENOTSUP;
    }

    ag_err_t err = io->gpio_config(T_INT, AG_GPIO_IN);
    if (err != AG_OK) {
        ag_printf("CST816: gpio %d as input: %s\n", T_INT, ag_strerror(err));
        return err;
    }

    /*
     * Ask the part what it is, before claiming a device name.
     *
     * A touch driver that loads against an empty bus is worse than one that
     * refuses: the board then has a touch0 that never reports, which reads as
     * "the screen is broken" rather than "the wiring is not what the driver
     * thinks".  0xB4/B5/B6 are the CST816S/T/D and 0xB7 the CST820; any of
     * them speaks this register layout.
     */
    uint8_t id = 0;
    uint8_t reg = REG_CHIPID;

    if (io->i2c_wrrd(T_BUS, T_ADDR, &reg, 1, &id, 1, T_MS) != AG_OK) {
        ag_printf("CST816: nothing answers at 0x%02x on i2c%d\n",
                  T_ADDR, T_BUS);
        return -AG_ENODEV;
    }
    if (id < 0xB4u || id > 0xB7u) {
        ag_printf("CST816: 0x%02x on i2c%d answered chip id 0x%02x, which is "
                  "not one of this family (B4/B5/B6/B7)\n", T_ADDR, T_BUS, id);
        return -AG_ENODEV;
    }

    /*
     * Which units the kernel wants.  Before ag_dev_add, because the table is
     * read from the moment the device exists.  See the same note in
     * xpt2046.c: pixels arrived in ABI 0.44.
     */
    ag_sysinfo_t si = {0};

    ag_api()->sys->info(&si);
    if (si.abi_major == 0u && si.abi_minor < 44u) {
        ag_printf("CST816: this kernel (ABI 0.%u) takes pointer positions in "
                  "cells, which this driver does not produce\n",
                  (unsigned)si.abi_minor);
        return -AG_ENOTSUP;
    }

    const ag_dev_add_t desc = {
        .name = "touch0",
        .driver = "CST816",
        .cls = AG_DEV_INPUT,
        .ops = &k_dev_ops,
        .class_ops = &k_input_ops,
        .priv = NULL,
    };

    err = ag_dev_add(&desc);
    if (err != AG_OK) {
        return err;
    }

    ag_printf("CST816: chip 0x%02x at 0x%02x on i2c%d, int %d, %dx%d pixels\n",
              id, T_ADDR, T_BUS, T_INT, PANEL_W, PANEL_H);
    return AG_OK;
}
