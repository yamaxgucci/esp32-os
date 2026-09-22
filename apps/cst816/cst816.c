/*
 * ArgonOS - CST816D capacitive touch (.SYS).
 *
 *   drv install a:\cst816.sys
 *   dev                        -> touch0  input  CST816
 *
 * The second touch controller in this tree and the first capacitive one.  The
 * CYD's XPT2046 (apps/xpt2046/xpt2046.c) is a resistive panel on the display's
 * own SPI bus, and it has to be asked - through a pen line, a pressure reading
 * and a median of three - whether a finger is even there.  This one is a chip
 * with an opinion: it counts fingers, tracks them, digitises them, and answers
 * on I2C.  Nothing about the wire transfers.  What transfers is the contract,
 * and that is deliberate, so the two drivers are interchangeable from the
 * kernel up.
 *
 * THE CONTRACT.  An AG_DEV_INPUT device, published from ag_driver_init with
 * ag_dev_add() as touch0, whose class_ops is an ag_input_ops_t (ABI 0.28) with
 * a poll() entry.  The kernel asks a driver like this from its own console
 * tick - a .SYS owns no task, which is the whole reason a class of
 * "devices that have to be asked" exists - and what poll() returns goes into
 * the same event queue the terminal's mouse reports feed, so nothing above can
 * tell a finger from a mouse.  poll() fills at most `max` events and returns
 * how many; zero is the normal answer and has to be cheap, because this runs
 * ten times a second forever.
 *
 * Coordinates are pixels of the panel (AG_PTR_PIXELS with span_w/span_h, ABI
 * 0.44), not console cells.  A capacitive digitizer resolves the pixel under
 * the finger, and rounding that to a cell throws away eight of every nine of
 * them; the two bugs that came of getting this wrong are written out at length
 * in apps/xpt2046/xpt2046.c and need not be repeated.  There is no cell path
 * here because this driver is new: every kernel that can load it already knows
 * what a pixel is.
 *
 * THE CHIP, and the register map, which is worth being exact about because the
 * prompt to write this named a register that does not exist:
 *
 *   0x01  gesture id            the chip's own swipe/tap classification
 *   0x02  finger count          then the coordinates: five bytes in all
 *   0x03  x high (low nibble)     0x04  x low
 *   0x05  y high (low nibble)     0x06  y low
 *   0xA7  chip id              0xB4 CST816S, 0xB5 CST816T, 0xB6 CST816D,
 *                              0xB7 CST820
 *   0xA9  firmware version
 *
 * 0x00 is not a version register, whatever it looks like: in this family it is
 * the event/status byte.  Checked against two independent ports of the family
 * rather than taken on trust - Zephyr's drivers/input/input_cst8xx.c (which
 * names all four ids) and Espressif's own esp_lcd_touch_cst816s (DATA_START_REG
 * 0x02, CHIP_ID_REG 0xA7) - and both read the point as one five byte burst from
 * 0x02, which is what this does.  One transfer and not five reads is not a
 * micro-optimisation: separate reads of the high and low halves can catch the
 * controller mid-update and assemble a coordinate from two different moments,
 * which on a moving finger is a diagonal jump.
 *
 * Nothing here writes a register.  The vendor's driver resets the part and
 * masks interrupts because it drives them from a GPIO; there is no interrupt
 * here (see int below) and this board does not route the touch reset to a GPIO,
 * so the part is read-only as far as this driver is concerned.
 *
 * 0x01 is deliberately not read.  The chip will classify a swipe for us, and
 * the kernel's event vocabulary has no word for one - a gesture reported as a
 * pointer move is something the layer above already derives correctly, and a
 * gesture reported as anything else would be this driver inventing vocabulary
 * that only it speaks.  No more than the contract.
 *
 * THE FRAME, which is the one thing this board makes interesting.  The
 * controller measures in its own frame - on this 2 inch panel, 240 across and
 * 320 down, portrait - and the display driver decides which way round the glass
 * is presented.  If it presents it landscape (every ST7789 driver in this tree
 * programs MADCTL with MV set, which turns the addressing on its side), then a
 * finger's x is the controller's y and the pointer lands a quarter turn away
 * from the finger.  Which quarter, and whether it is mirrored as well, is a
 * fact about which end of the flex the module was soldered by - the very
 * non-derivable fact the display driver's own comment admits - so it is a
 * BOARD.CFG key and not a constant: [touch] rotation, flip_x, flip_y.  Settle
 * it with apps/touch.axe, whose instructions are printed on the screen the
 * finger is on.  A wrong rotation is obvious in one stroke; the two flips are
 * what is left when all four rotations are wrong.  What is NOT a guess is the
 * span reported to the kernel: it is the frame after the turn, so the kernel
 * scales it to the surface whatever the rotation is.
 *
 * Where the numbers above come from: the community port for this board
 * (mrRobot62/esp32-s3-touch-lcd-2, src/display/BoardDisplay.cpp) names sda 48,
 * scl 47, address 0x15, 400 kHz and no interrupt pin, and its own display is
 * portrait with no touch compensation - which is one of the eight answers this
 * key can be, and not evidence about which one this module is.
 *
 * What THIS board's pack needs, though, is not that: it drives the ST7789T3
 * landscape (MADCTL 0x60, a 320x240 console), and WaveShare's own Arduino port
 * for it compensates in bsp_cst816.cpp with, for its rotation 1,
 *
 *   x = y;  y = 240 - 1 - x;
 *
 * which is this file's rotation 3 exactly - x from the controller's y, y from
 * its x, one of them mirrored - and gives a 320x240 span that the kernel scales
 * one to one onto that surface.  So the pack carries rotation = 3.  Note why a
 * quarter turn is not the whole answer: rotation 1 and rotation 3 are the two
 * landscapes, 180 degrees apart, and MADCTL 0x60 is only one of them - so a
 * pointer that lands diagonally opposite the finger wants 1, not 3.  Derived
 * from the vendor's source rather than measured; if neither turn nor that
 * swap matches, the remaining answer is a flip bit.  One stroke of
 * apps/touch.axe settles it, because the demo prints where it thinks the
 * finger was.
 *
 * BOARD.CFG keys, all under [touch], all optional:
 *
 *   bus      = 0        I2C bus number, as in io/bus and [i2c0]
 *   addr     = 0x15     the controller's 7-bit address
 *   width    = 240      the controller's own frame, before any turn
 *   height   = 320      ... and it is one pair, not two: rotation swaps them
 *   rotation = 0        0/1/2/3 quarter turns clockwise: none, 90, 180, 270
 *   flip_x   = false    mirror after the turn
 *   flip_y   = false    ... the two remaining symmetries
 *   int      = -1       GPIO the controller's INT line is on, or -1 for none
 *
 * The two bus PINS are not here, and cannot be: an I2C bus belongs to the
 * kernel - io.c brings it up from ag_board()->i2c[bus], which BOARD.CFG fills
 * from [i2c0] sda/scl - and a driver has no way to wire one.  io->i2c_* take a
 * bus number, not a pair of pins.  So [i2c0] sda/scl is where 48 and 47 go, and
 * this file only names which bus to use.  A bus with no pins in BOARD.CFG
 * answers -AG_ENODEV here, and ag_driver_init says so in as many words rather
 * than publishing a touch device that can never produce an event.
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

/* Read from here: finger count, then x hi/lo and y hi/lo. */
#define REG_DATA   0x02
#define REG_CHIPID 0xa7

#define ID_CST816S 0xb4
#define ID_CST816T 0xb5
#define ID_CST816D 0xb6
#define ID_CST820  0xb7

/*
 * The longest a single register read may take, in milliseconds.
 *
 * A poll runs on the console task, between the screen and the shell, so a read
 * that blocks is a screen that stops.  A chip that is there answers in
 * microseconds at 400 kHz; a chip that is not there NACKs immediately.  This
 * bound only exists so that a bus held down by something else cannot hold the
 * console with it.
 */
#define READ_MS 50u

/*
 * How far a finger must move to count as having moved, in pixels.
 *
 * A capacitive digitizer is quiet when it is still - far quieter than the
 * resistive panel's few pixels of wander - but not perfectly quiet, and in
 * pixels a single count of noise is a MOVE and a redraw and a cursor that
 * twitches.  Two pixels is below what a hand can aim for and above what the
 * glass invents.
 */
#define DEADBAND 2

/*
 * What BOARD.CFG is assumed to say when it says nothing.
 *
 * These are the board this was written for - the Waveshare
 * ESP32-S3-Touch-LCD-2, whose CST816D shares I2C0 with a QMI8658 IMU at address
 * 0x15 - but every one of them is a key, because the next board with this chip
 * on it will differ in at least the address and the rotation.  The rotation
 * default is "none": the controller's own frame, as reported, which is what a
 * reader of this file can reason about, and this board's pack says 3 (above)
 * rather than moving the default under the next board's feet.
 */
#define DEF_BUS    0
#define DEF_ADDR   0x15
#define DEF_W      240
#define DEF_H      320
#define DEF_ROT    0
#define DEF_INT    (-1)

static struct {
    int      bus;
    uint8_t  addr;
    int      rot;   /* 0..3 quarter turns clockwise                        */
    bool     flip_x;
    bool     flip_y;
    int      int_pin; /* -1 when BOARD.CFG names none                      */
    uint16_t raw_w, raw_h;  /* the controller's own frame                  */
    uint16_t span_w, span_h; /* ...after the turn: what is reported to the
                              * kernel, and what it scales to the surface    */
    bool     down;
    int16_t  x, y;  /* the last position reported: the UP carries it       */
} s_dev;

/*
 * Where a poll went, counted, because "it stops sometimes" cannot be chased
 * from the far end.  The same instrument as apps/xpt2046's, and for the same
 * reason: the shell can measure how long it waited between pointer events but
 * not why none came, so the driver counts its own polls and the tally goes to
 * the kernel's journal - not to the console, which is what the screen is
 * showing.
 *
 * A window where nobody touched the glass says nothing, and saying it every
 * three seconds is not free: the journal holds a few dozen lines, so a board
 * left alone for two minutes overwrites the only lines worth having.  Such a
 * window is counted, and the count is carried into the next line that has
 * something in it.
 */
static struct {
    uint32_t polls;    /* touch_poll called                                */
    uint32_t idle;     /* the controller says no finger is down            */
    uint32_t errs;     /* the register read did not answer                 */
    uint32_t still;    /* a finger, read, but inside the dead band         */
    uint32_t sent;     /* an event handed to the kernel                    */
    uint32_t releases; /* touches that ended: one per finger, if all is well */
    uint32_t worst;    /* longest silence WHILE a finger was down, ms      */
    uint32_t quiet_at; /* when the current dry spell started               */
    uint32_t said_at;  /* last time this was written down                  */
    uint32_t asleep;   /* untouched windows counted since it was written   */
} s_tally;

static void tally_tick(bool produced)
{
    const uint32_t now = ag_millis();

    /*
     * Timed only inside a contact.  Timed from one event to the next
     * regardless, a hand lifted for two seconds between strokes is reported as
     * a two second silence, and a report that says that cannot also say
     * anything about a pointer that sticks.
     */
    if (!s_dev.down) {
        s_tally.quiet_at = 0u;
    } else if (produced) {
        const uint32_t dry =
            (s_tally.quiet_at != 0u) ? now - s_tally.quiet_at : 0u;
        if (dry > s_tally.worst) {
            s_tally.worst = dry;
        }
        s_tally.quiet_at = now;
    } else if (s_tally.quiet_at == 0u) {
        s_tally.quiet_at = now;
    }

    if (s_tally.said_at == 0u) {
        s_tally.said_at = now;
        return;
    }
    if (now - s_tally.said_at < 3000u || s_tally.polls == 0u) {
        return;
    }

    if (s_tally.polls == s_tally.idle) {
        s_tally.asleep++;
        s_tally.said_at = now;
        s_tally.polls = 0;
        s_tally.idle = 0;
        return;
    }

    ag_log(AG_LOG_INFO, "CST816",
           "polls %u: idle %u, errs %u, still %u, sent %u; "
           "%u touch(es) released; longest stuck %u ms; "
           "%u quiet window(s) before this",
           (unsigned)s_tally.polls, (unsigned)s_tally.idle,
           (unsigned)s_tally.errs, (unsigned)s_tally.still,
           (unsigned)s_tally.sent, (unsigned)s_tally.releases,
           (unsigned)s_tally.worst, (unsigned)s_tally.asleep);

    s_tally.said_at = now;
    s_tally.asleep = 0;
    s_tally.polls = 0;
    s_tally.idle = 0;
    s_tally.errs = 0;
    s_tally.still = 0;
    s_tally.sent = 0;
    s_tally.releases = 0;
    s_tally.worst = 0;
}

/* ---- the wire ---------------------------------------------------------- */

/*
 * One five byte burst from 0x02 in a single transaction - register, then count
 * and both coordinates - because the bus is shared with the IMU and because two
 * reads would be two moments.  The finger count comes back as the return value,
 * or -1 when nothing answered.
 */
static int read_touch(int *out_x, int *out_y)
{
    const uint8_t reg = REG_DATA;
    uint8_t       buf[5];

    if (ag_i2c_wrrd(s_dev.bus, s_dev.addr, &reg, 1, buf, sizeof(buf),
                    READ_MS) != AG_OK) {
        return -1;
    }

    /*
     * The high byte carries the top four bits and nothing else; the low nibble
     * of that byte is where the twelve bit coordinate starts, which is the
     * layout both other ports of this family read.  Masked rather than shifted
     * so that a controller which sets the spare bits high cannot move the
     * pointer four thousand pixels.
     */
    *out_x = (((int)buf[1] & 0x0f) << 8) | (int)buf[2];
    *out_y = (((int)buf[3] & 0x0f) << 8) | (int)buf[4];
    return (int)(buf[0] & 0x0f);
}

static const char *id_name(uint8_t id)
{
    switch (id) {
    case ID_CST816S:
        return "CST816S";
    case ID_CST816T:
        return "CST816T";
    case ID_CST816D:
        return "CST816D";
    case ID_CST820:
        return "CST820";
    default:
        return "not of this family";
    }
}

/* ---- the frame --------------------------------------------------------- */

/*
 * A raw coordinate to the frame the kernel is told about: quarter turns
 * clockwise, then the mirrors, then a clamp.
 *
 * Clamped rather than dropped, for the reason the resistive driver clamps: a
 * touch in the last millimetre of the glass is still a touch, and a coordinate
 * outside the panel is a controller's rounding rather than a finger.  The
 * mirrored spans are the raw ones swapped, which is what a 90 or 270 degree
 * turn does to a rectangle.
 */
static void to_panel(int rx, int ry, int16_t *out_x, int16_t *out_y)
{
    if (rx < 0) {
        rx = 0;
    } else if (rx > (int)s_dev.raw_w - 1) {
        rx = (int)s_dev.raw_w - 1;
    }
    if (ry < 0) {
        ry = 0;
    } else if (ry > (int)s_dev.raw_h - 1) {
        ry = (int)s_dev.raw_h - 1;
    }

    int x, y;
    switch (s_dev.rot) {
    case 1:
        x = (int)s_dev.raw_h - 1 - ry;
        y = rx;
        break;
    case 2:
        x = (int)s_dev.raw_w - 1 - rx;
        y = (int)s_dev.raw_h - 1 - ry;
        break;
    case 3:
        x = ry;
        y = (int)s_dev.raw_w - 1 - rx;
        break;
    default:
        x = rx;
        y = ry;
        break;
    }

    if (s_dev.flip_x) {
        x = (int)s_dev.span_w - 1 - x;
    }
    if (s_dev.flip_y) {
        y = (int)s_dev.span_h - 1 - y;
    }

    if (x < 0) {
        x = 0;
    } else if (x > (int)s_dev.span_w - 1) {
        x = (int)s_dev.span_w - 1;
    }
    if (y < 0) {
        y = 0;
    } else if (y > (int)s_dev.span_h - 1) {
        y = (int)s_dev.span_h - 1;
    }

    *out_x = (int16_t)x;
    *out_y = (int16_t)y;
}

/* ---- the class vtable -------------------------------------------------- */

static int32_t touch_poll(ag_handle_t h, ag_event_t *out, uint32_t max)
{
    (void)h;
    if (out == NULL || max == 0u) {
        return 0;
    }

    s_tally.polls++;

    /*
     * The interrupt line first, when BOARD.CFG names one, and usually only
     * that: one GPIO read against a five byte transaction on a bus the IMU is
     * also on, ten times a second forever.
     *
     * It is trusted only in the "nothing is happening" direction, and only
     * while nothing is already down - the same narrow trust the CYD's pen line
     * gets, and for a sharper reason.  Some ports of this family pulse INT and
     * some hold it low for the length of a touch; on a pulsing one, skipping a
     * poll is skipping a short tap entirely.  So the default is no pin at all,
     * and this board's INT is not wired to a GPIO: the key exists for the board
     * that has one and knows which kind it is.
     */
    if (s_dev.int_pin >= 0 && !s_dev.down &&
        ag_gpio_read(s_dev.int_pin) != 0) {
        s_tally.idle++;
        tally_tick(false);
        return 0;
    }

    int       rx = 0;
    int       ry = 0;
    const int fingers = read_touch(&rx, &ry);

    if (fingers < 0) {
        /* Not fatal: the bus is shared and the controller is a chip that can be
         * busy, and the next tick is a hundred milliseconds away.  Counted,
         * because an intermittent bus is invisible from above. */
        s_tally.errs++;
        tally_tick(false);
        return 0;
    }

    if (fingers == 0) {
        if (!s_dev.down) {
            s_tally.idle++;
            tally_tick(false);
            return 0;
        }

        /*
         * Released, and believed at once.
         *
         * The resistive driver next door waits sixty milliseconds before it
         * accepts a release, because its pen line reads high for most of a
         * stroke and it has to be argued with.  This one does not: the finger
         * count is a fact the controller established, not a voltage somebody
         * read, and a wait here would be a hundred milliseconds of lag on every
         * tap for a fault that does not exist.
         *
         * The last position is carried out with the release, because a tap is
         * a press and its release at a place, and an application that only
         * learns where at the press has to remember it itself.
         */
        s_dev.down = false;
        out[0].type = AG_EV_POINTER_UP;
        out[0].ptr.x = s_dev.x;
        out[0].ptr.y = s_dev.y;
        out[0].ptr.dx = 0;
        out[0].ptr.dy = 0;
        out[0].ptr.buttons = 0;
        out[0].ptr.slot = 0;
        s_tally.sent++;
        s_tally.releases++;
        tally_tick(true);
        return 1;
    }

    /*
     * One point, and the family reports one: a count above one is a duplicate
     * report of the same finger, not a second one.  The ABI's slot field stays
     * zero, which is what a single pointer means.
     */
    int16_t x = 0, y = 0;
    to_panel(rx, ry, &x, &y);

    const int dx = (int)x - (int)s_dev.x;
    const int dy = (int)y - (int)s_dev.y;
    const bool moved = (dx >= DEADBAND || -dx >= DEADBAND ||
                        dy >= DEADBAND || -dy >= DEADBAND);

    if (!s_dev.down) {
        s_dev.down = true;
        out[0].type = AG_EV_POINTER_DOWN;
    } else if (moved) {
        out[0].type = AG_EV_POINTER_MOVE;
    } else {
        s_tally.still++;
        tally_tick(false);
        return 0; /* still down, still in the same place: nothing happened */
    }

    out[0].ptr.x = x;
    out[0].ptr.y = y;
    /*
     * A move carries how far it moved; a press carries nothing, because the
     * delta from wherever the last touch ended is not a movement this finger
     * made and a consumer that subtracts it would jump the pointer.
     */
    out[0].ptr.dx = (out[0].type == AG_EV_POINTER_MOVE) ? (int16_t)dx : 0;
    out[0].ptr.dy = (out[0].type == AG_EV_POINTER_MOVE) ? (int16_t)dy : 0;
    out[0].ptr.buttons = 1;
    out[0].ptr.slot = 0;

    s_dev.x = x;
    s_dev.y = y;
    s_tally.sent++;
    tally_tick(true);
    return 1;
}

/*
 * Filled at load time, because the spans are BOARD.CFG's.  `units` is not
 * conditional: this driver was written after ABI 0.44 and there is no kernel
 * that can load it which would read these numbers as cells.
 */
static ag_input_ops_t k_input_ops = {
    .size = sizeof(ag_input_ops_t),
    .poll = touch_poll,
    .units = AG_PTR_PIXELS,
    .span_w = DEF_W,
    .span_h = DEF_H,
};

/* A touch device is only ever asked; there is nothing to open or read. */
static const ag_dev_ops_t k_dev_ops = {0};

ag_err_t ag_driver_init(void)
{
    const ag_cfg_api_t *cfg = ag_api()->cfg;
    if (cfg == NULL) {
        return -AG_ENOTSUP;
    }

    /*
     * How many I2C buses exist is the kernel's business and is not in the ABI,
     * so this file does not carry a count of them: this .SYS outlives the
     * kernel it was built beside, and a driver that refused bus 2 on a kernel
     * that has one would be this file guessing.  Only a number that cannot be
     * a bus at all is refused here; the rest is settled by io->i2c_probe()
     * below, where the buses actually are.
     */
    const int32_t bus = cfg->get_int("touch.bus", DEF_BUS);
    if (bus < 0) {
        ag_printf("CST816: touch.bus %d: a bus number is not negative\n",
                  (int)bus);
        return -AG_ERANGE;
    }
    s_dev.bus = (int)bus;

    const int32_t addr = cfg->get_int("touch.addr", DEF_ADDR);
    if (addr < 0x08 || addr > 0x77) {
        ag_printf("CST816: touch.addr 0x%02x is not a 7-bit address\n",
                  (unsigned)addr);
        return -AG_EINVAL;
    }
    s_dev.addr = (uint8_t)addr;

    /*
     * The frame, before any turn.  Checked against something that could be a
     * panel at all: a width of zero would make the span nonsense and a width of
     * eight thousand would put the pointer off the glass on the first touch.
     */
    int32_t w = cfg->get_int("touch.width", DEF_W);
    int32_t h = cfg->get_int("touch.height", DEF_H);
    if (w < 16 || w > 4096 || h < 16 || h > 4096) {
        ag_printf("CST816: touch.width/height %dx%d: not a panel; "
                  "using %dx%d\n", (int)w, (int)h, DEF_W, DEF_H);
        w = DEF_W;
        h = DEF_H;
    }
    s_dev.raw_w = (uint16_t)w;
    s_dev.raw_h = (uint16_t)h;

    const int32_t rot = cfg->get_int("touch.rotation", DEF_ROT);
    if (rot < 0 || rot > 3) {
        ag_printf("CST816: touch.rotation %d: it is 0, 1, 2 or 3 quarter "
                  "turns clockwise; using %d\n", (int)rot, DEF_ROT);
        s_dev.rot = DEF_ROT;
    } else {
        s_dev.rot = (int)rot;
    }
    s_dev.flip_x = cfg->get_bool("touch.flip_x", false);
    s_dev.flip_y = cfg->get_bool("touch.flip_y", false);

    /*
     * A quarter turn swaps which raw axis is the screen's across, so the span
     * reported to the kernel swaps with it.  This is the number the kernel
     * scales to the surface, and it is the one thing about the orientation that
     * cannot be wrong without the pointer being wrong everywhere - so it is
     * derived here rather than written down twice.
     */
    const bool turned = (s_dev.rot == 1 || s_dev.rot == 3);
    s_dev.span_w = turned ? s_dev.raw_h : s_dev.raw_w;
    s_dev.span_h = turned ? s_dev.raw_w : s_dev.raw_h;
    k_input_ops.span_w = s_dev.span_w;
    k_input_ops.span_h = s_dev.span_h;

    s_dev.int_pin = (int)cfg->get_int("touch.int", DEF_INT);
    if (s_dev.int_pin >= 0) {
        /*
         * Failing to take the pin is not failing to load.  The line is a hint
         * that saves ten I2C transactions a second; the touch works without it,
         * and refusing to publish a controller over a pin some other driver got
         * to first would be trading a working screen for a tidier boot log.
         */
        const ag_err_t pin = ag_gpio_config(s_dev.int_pin, AG_GPIO_IN_PULLUP);
        if (pin != AG_OK) {
            ag_printf("CST816: touch.int %d: %s; polling the bus instead\n",
                      s_dev.int_pin, ag_strerror(pin));
            s_dev.int_pin = -1;
        }
    }

    /*
     * Is anything there?  A touch device that answers nothing is a device that
     * will never produce an event, and the person reading the boot log is the
     * one holding the schematic - so this is the one failure worth refusing to
     * load over.  The two answers are different and both are named: -AG_ENODEV
     * is a bus with no pins in BOARD.CFG, -AG_ENOENT is a bus with nothing at
     * this address.
     */
    const ag_err_t probe = ag_i2c_probe(s_dev.bus, s_dev.addr);
    if (probe != AG_OK) {
        ag_printf("CST816: i2c%d 0x%02x: %s\n", s_dev.bus,
                  (unsigned)s_dev.addr, ag_strerror(probe));
        if (probe == -AG_ENODEV) {
            ag_printf("CST816: set i2c%d.sda and i2c%d.scl in BOARD.CFG "
                      "(this board: 48 and 47)\n", s_dev.bus, s_dev.bus);
        } else if (probe == -AG_ENOENT) {
            /*
             * The bus works and nothing is at that address.  On a board whose
             * controller is asleep, unpowered or not fitted, this is the line
             * that says so - and it says it as a refusal, because a touch
             * device that cannot answer is one the system would offer to its
             * neighbours and could not serve.  Deliberately not loosened: a
             * driver that published touch0 anyway would turn "the chip is
             * silent" into "the pointer does nothing", which is the same fault
             * reported one layer further from the cause.
             */
            ag_printf("CST816: nothing answered at 0x%02x - the controller is "
                      "silent (supply, reset, or address)\n",
                      (unsigned)s_dev.addr);
        } else if (probe == -AG_ERANGE) {
            ag_printf("CST816: touch.bus %d: this chip has no such bus\n",
                      s_dev.bus);
        }
        return probe;
    }

    /*
     * The chip id, read and reported but not enforced.  It proves the register
     * interface answers, and it is the first thing anybody wants to see when
     * the pointer does not move - but the family shares one map, something
     * answered at the address, and a driver that refuses to load over an
     * unexpected id byte is a board with no touch at all.  So it is a
     * diagnostic line.
     */
    uint8_t       reg = REG_CHIPID;
    uint8_t       id = 0;
    const ag_err_t idr =
        ag_i2c_wrrd(s_dev.bus, s_dev.addr, &reg, 1, &id, 1, READ_MS);
    if (idr != AG_OK) {
        ag_printf("CST816: chip id unreadable (%s); carrying on\n",
                  ag_strerror(idr));
    }

    /* The name is the same one the resistive driver publishes, on purpose:
     * the two are alternatives for the same glass, the board's A:\DRV holds
     * one of them, and everything above looks for a touch device rather than
     * for a model number. */
    const ag_dev_add_t desc = {
        .name = "touch0",
        .driver = "CST816",
        .cls = AG_DEV_INPUT,
        .ops = &k_dev_ops,
        .class_ops = &k_input_ops,
        .priv = NULL,
    };
    const ag_err_t err = ag_dev_add(&desc);
    if (err != AG_OK) {
        ag_printf("CST816: cannot publish touch0: %s\n", ag_strerror(err));
        return err;
    }

    ag_printf("CST816: touch0 on i2c%d at 0x%02x, %ux%u pixels, "
              "rotation %d%s%s%s\n",
              s_dev.bus, (unsigned)s_dev.addr, (unsigned)s_dev.span_w,
              (unsigned)s_dev.span_h, s_dev.rot,
              s_dev.flip_x ? ", flip x" : "", s_dev.flip_y ? ", flip y" : "",
              s_dev.int_pin >= 0 ? ", int line" : ", polled");
    if (idr == AG_OK) {
        ag_printf("CST816: chip id 0x%02x (%s)\n", (unsigned)id,
                  id_name(id));
    }
    return AG_OK;
}
