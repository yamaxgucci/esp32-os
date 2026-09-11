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
 * The panel this sits on.  Pixels of it, and no grid at all (ABI 0.44).
 *
 * It used to report console cells, because that is what an input driver's
 * contract said, and the kernel turned them back into pixels with the
 * console's grid.  Two faults came of that and only one was arithmetic.  The
 * arithmetic one: this driver divided by a grid of its own - the panel's
 * forty by thirty of 8x8 - while the kernel multiplied by the console's,
 * forty by twenty-five, so each row came back nine pixels instead of eight.
 * The pointer sat below the stylus, further below the lower you touched, and
 * the bottom forty pixels of glass could not be pressed at all.  Both halves
 * were self-consistent, which is why neither one alone explained anything.
 *
 * The other fault is the convention: a stylus on 320x240 can point at a
 * pixel, and rounding it to the nearest cell throws away eight of every nine
 * before the shell ever sees it.  So the driver says AG_PTR_PIXELS and hands
 * over pixels of its own span; the kernel scales that span to the surface and
 * no grid is involved.  On a kernel older than 0.44 that would be read as
 * cells, so the units are chosen at load time from what the kernel says it
 * is, and the cell path is kept for it.
 */
#define PANEL_W  320
#define PANEL_H  240
#define CELLS_MAX 255

/*
 * How far a finger must move to count as having moved, in pixels.
 *
 * A resistive panel wanders by a pixel or two while it is held perfectly
 * still, and reporting cells hid that: a wobble inside one cell was no event
 * at all.  In pixels it is a stream of them, which is a redraw each and a
 * cursor that will not sit still.  Two pixels is below what a hand can aim
 * for and above what the glass invents.
 */
#define DEADBAND 2

/*
 * How long the glass may say "nothing is touching me" before it is believed.
 *
 * The pen line is one wire and it is not a promise.  While a stylus is drawn
 * across this panel it reads high on most polls - measured, seventy per cent
 * of them during a stroke - and every one of those used to end the stroke on
 * the spot: pointer up, and then down again a few polls later somewhere else.
 * From a hand's point of view the pointer stops dead for half a second and
 * then jumps, which is exactly what Maxim reported.
 *
 * So while the pen is down the line only raises the question and the
 * controller answers it: pressure is read, and a release is declared only
 * after the glass has been quiet for this long.  Sixty milliseconds is under
 * what a hand can lift and replace, and over what a stroke does to itself.
 */
#define UP_SETTLE_MS 60u

/*
 * How far away a returning contact is a NEW touch rather than a movement.
 *
 * The settle above bridges a contact that flickers where it is; it must not
 * bridge one that comes back somewhere else.  A double tap does exactly
 * that - the finger leaves and lands a few pixels off, inside the settle -
 * and bridged, the two taps become one press with a jump in the middle,
 * which is a drag.  Maxim found it by double-tapping an icon and watching
 * it move.
 *
 * Twelve pixels: a tap lands within a few of the last one, and a drag that
 * begins with a jump of twelve has already left the thing it grabbed.
 */
#define JUMP_PX 12

/*
 * How long a contact that is MOVING may go quiet before it has ended.
 *
 * A resting stylus keeps a steady pressure; a moving one does not - it
 * rolls onto its edge, the hand unloads it through a change of direction,
 * and the reading dips below any threshold for a few tens of
 * milliseconds.  Sixty of them is not enough: one drag across a list came
 * out of this driver as FOUR separate touches, which the shell above duly
 * read as four separate gestures, each one starting by selecting the row
 * under it and throwing away what the last had marked.
 *
 * A quarter of a second while moving, and the ordinary sixty while not.
 * The distinction matters and it is not cosmetic: the short settle is
 * what keeps a double tap two taps, and a double tap does not move.  A
 * contact that has travelled past the threshold below is a drag, and a
 * drag has no double to protect.
 */
#define UP_SETTLE_DRAG_MS 250u
#define TRAVEL_PX         8

/*
 * Raw readings at the edges of the glass.  A resistive panel is a pair of
 * potentiometers and these are where its ends are; they vary between panels of
 * the same model, so they are a starting point rather than a fact.  Anything
 * outside is clamped rather than dropped: a touch in the last millimetre of
 * the screen is still a touch.
 */
#define RAW_MIN 300
#define RAW_MAX 3800

/*
 * Below this the glass is not being pressed hard enough to trust - and
 * "this" is two numbers, because starting a touch and continuing one are
 * different questions.
 *
 * Starting one has to be sure: a resistive panel reads a little pressure
 * from a knock on the desk, and a stroke that begins where nobody put a
 * finger is worse than one that begins late.
 *
 * Continuing one does not.  The contact is established, the finger is
 * visibly still on the glass, and the pressure a hand applies while MOVING
 * is lower than the pressure it applied to land - the stylus rolls onto its
 * edge, the arm takes some of the weight back.  Holding the landing
 * threshold throughout is what dropped 63 of 300 polls in the middle of a
 * stroke and left the pointer standing still for 80 ms at a time while
 * Maxim was drawing.  Those polls read the glass correctly; they were
 * thrown away by this number alone.
 */
#define Z_MIN      200
#define Z_MIN_HELD 90

/* Start bit, channel, 12-bit differential mode, power down between reads. */
#define CMD_X  0xd0u
#define CMD_Y  0x90u
#define CMD_Z1 0xb0u
#define CMD_Z2 0xc0u

static const ag_io_api_t *io;

/*
 * True when this kernel understands pixels from an input driver (ABI 0.44).
 * On an older one the same numbers would be read as console cells and a tap
 * near the right edge would land four screens away, so the cell path stays.
 */
static bool s_px;

/*
 * Where a poll went, counted, because "it hangs sometimes" cannot be chased
 * from the far end.
 *
 * The shell measures two things and they split the problem in half: how long
 * its slowest repaint took, and how long it waited between pointer events.
 * On the CYD the first is five milliseconds and the second is two hundred to
 * seven hundred - so the shell is not slow, the events are not arriving, and
 * the only place left is here.  A poll that produces nothing does so for one
 * of three reasons and they want different fixes, so each is counted and the
 * tally goes to the kernel's journal - not the console, which is what the
 * screen is showing.
 */
static struct {
    uint32_t polls;    /* touch_poll called                                */
    uint32_t no_pen;   /* the pen line says nothing is down                */
    uint32_t weak;     /* pressed, but under Z_MIN: dropped                */
    uint32_t still;    /* pressed and read, but inside the dead band       */
    uint32_t sent;     /* an event handed to the kernel                    */
    uint32_t flaky;    /* line said up, the glass was still being pressed  */
    /*
     * The longest the pointer stood still WHILE SOMETHING WAS ON THE
     * GLASS, in milliseconds.
     *
     * The "while" is the whole value of the number.  It used to be timed
     * from one delivered event to the next regardless, so a hand lifted
     * for two seconds between strokes was reported as a two-second
     * silence - and a report that says 2630 ms about a person resting
     * their arm cannot also say anything about a pointer that sticks.
     * Timed only inside a contact, it answers the question it is named
     * for.
     */
    uint32_t worst;
    uint32_t quiet_at; /* when the current dry spell started               */
    uint32_t said_at;  /* last time this was written down                  */
    uint32_t asleep;   /* windows in a row where the glass was untouched   */
    uint32_t ended;    /* touches that ended: one per finger, if all is well */
    uint32_t jumped;   /* ...of those, ended because the contact moved away  */
    /*
     * What the glass actually reads while somebody is drawing on it.
     *
     * Both thresholds in this file were picked by eye and then halved
     * when they turned out to be wrong, which is how a drag came out as
     * four touches.  Four counters say where the readings really fall,
     * so the next number can be chosen instead of guessed: nothing,
     * under fifty, under the held threshold, and above it.
     */
    uint32_t z_none;
    uint32_t z_lo;
    uint32_t z_mid;
    uint32_t z_ok;
} s_tally;

static void tally_tick(bool produced, bool touching)
{
    const uint32_t now = ag_millis();

    if (!touching) {
        /* Nothing is on the glass: there is no gap to measure. */
        s_tally.quiet_at = 0u;
        return;
    }

    if (produced) {
        const uint32_t dry = (s_tally.quiet_at != 0u) ? now - s_tally.quiet_at
                                                      : 0u;
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

    /*
     * A window where nobody touched the glass says nothing, and saying it
     * every three seconds is not free: the journal holds a few dozen lines,
     * so a board left alone for two minutes overwrites the only lines worth
     * having.  That is not a hypothetical - it is how the first attempt to
     * measure a stalling pointer came back with forty-four identical lines
     * of `no pen 300, sent 0` and the interesting ones already gone.
     *
     * So an untouched window is counted, not printed, and the count is
     * carried into the next line that has something in it.  Silence stays
     * visible; it just stops shouting.
     */
    if (s_tally.no_pen == s_tally.polls) {
        s_tally.asleep++;
        s_tally.said_at = now;
        s_tally.polls = 0;
        s_tally.no_pen = 0;
        return;
    }

    ag_log(AG_LOG_INFO, "XPT2046",
           "polls %u: no pen %u, flaky %u, weak %u, still %u, sent %u; "
           "%u touch(es) ended, %u of them by a jump; "
           "pressure 0/%u <50/%u <held/%u ok/%u; "
           "longest stuck %u ms; %u quiet window(s) before this",
           (unsigned)s_tally.polls, (unsigned)s_tally.no_pen,
           (unsigned)s_tally.flaky, (unsigned)s_tally.weak,
           (unsigned)s_tally.still, (unsigned)s_tally.sent,
           (unsigned)s_tally.ended, (unsigned)s_tally.jumped,
           (unsigned)s_tally.z_none, (unsigned)s_tally.z_lo,
           (unsigned)s_tally.z_mid, (unsigned)s_tally.z_ok,
           (unsigned)s_tally.worst, (unsigned)s_tally.asleep);
    s_tally.said_at = now;
    s_tally.asleep = 0;
    s_tally.polls = 0;
    s_tally.no_pen = 0;
    s_tally.flaky = 0;
    s_tally.weak = 0;
    s_tally.still = 0;
    s_tally.sent = 0;
    s_tally.ended = 0;
    s_tally.jumped = 0;
    s_tally.z_none = 0;
    s_tally.z_lo = 0;
    s_tally.z_mid = 0;
    s_tally.z_ok = 0;
    s_tally.worst = 0;
}

static struct {
    bool     down;
    int16_t  col, row; /* the last position reported: cells or pixels */
    uint32_t samples;
} s_state;

/* When the glass first went quiet under a pen we still believe is down. */
static uint32_t s_up_since;

/* Where the current contact started, so a drag can be told from a tap. */
static int16_t s_down_x, s_down_y;

static bool travelled(void)
{
    const int dx = (s_state.col > s_down_x) ? s_state.col - s_down_x
                                            : s_down_x - s_state.col;
    const int dy = (s_state.row > s_down_y) ? s_state.row - s_down_y
                                            : s_down_y - s_state.row;
    return dx > TRAVEL_PX || dy > TRAVEL_PX;
}

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
static int16_t to_px(uint16_t raw, int span)
{
    int v = (int)raw;
    if (v < RAW_MIN) {
        v = RAW_MIN;
    }
    if (v > RAW_MAX) {
        v = RAW_MAX;
    }
    int px = ((v - RAW_MIN) * (span - 1)) / (RAW_MAX - RAW_MIN);
    if (px < 0) {
        px = 0;
    }
    if (px > span - 1) {
        px = span - 1;
    }
    return (int16_t)px;
}

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
    s_tally.polls++;

    const bool line = (io->gpio_read(T_IRQ) == 0); /* low: something is on it */

    /*
     * Idle is the cheap case and the line is enough for it: nothing is down,
     * nothing says otherwise, and four conversions a hundred times a second
     * for a screen nobody is touching would be a tax on the whole bus - which
     * the panel is using.
     */
    if (!line && !s_state.down) {
        s_tally.no_pen++;
        tally_tick(false, s_state.down);
        return 0;
    }
    if (!line) {
        s_tally.no_pen++; /* but we do not believe it; see UP_SETTLE_MS */
    }

    const uint16_t z1 = read3(CMD_Z1);
    const uint16_t z2 = read3(CMD_Z2);
    const uint16_t rx = read3(CMD_X);
    const uint16_t ry = read3(CMD_Y);
    s_state.samples++;

    const uint16_t z = pressure(rx, z1, z2);
    const bool     pressed = z >= (s_state.down ? Z_MIN_HELD : Z_MIN);

    if (z == 0u) {
        s_tally.z_none++;
    } else if (z < 50u) {
        s_tally.z_lo++;
    } else if (z < Z_MIN_HELD) {
        s_tally.z_mid++;
    } else {
        s_tally.z_ok++;
    }

    if (!line && pressed) {
        s_tally.flaky++; /* the wire lied and the glass put it right */
    }

    /*
     * Back after a gap, and somewhere else: end the old touch here rather
     * than dragging it across.  The next poll starts a fresh one at the
     * new place.
     */
    if (pressed && s_state.down && s_up_since != 0u) {
        int16_t nx, ny;
        if (s_px) {
            nx = to_px(rx, PANEL_W);
            ny = to_px(ry, PANEL_H);
#if FLIP_X
            nx = (int16_t)(PANEL_W - 1 - nx);
#endif
#if FLIP_Y
            ny = (int16_t)(PANEL_H - 1 - ny);
#endif
            const int dx = (nx > s_state.col) ? nx - s_state.col
                                              : s_state.col - nx;
            const int dy = (ny > s_state.row) ? ny - s_state.row
                                              : s_state.row - ny;
            if (dx > JUMP_PX || dy > JUMP_PX) {
                s_up_since = 0u;
                s_state.down = false;
                s_tally.ended++;
                s_tally.jumped++;
                out[0].type = AG_EV_POINTER_UP;
                out[0].ptr.x = s_state.col;
                out[0].ptr.y = s_state.row;
                out[0].ptr.buttons = 0;
                s_tally.sent++;
                tally_tick(true, s_state.down);
                return 1;
            }
        }
    }

    if (!pressed) {
        if (!s_state.down) {
            s_tally.weak++;
            tally_tick(false, s_state.down);
            return 0;
        }
        /* Down until the glass has been quiet long enough to mean it. */
        const uint32_t now = ag_millis();
        if (s_up_since == 0u) {
            s_up_since = now;
        }
        const uint32_t settle =
            travelled() ? UP_SETTLE_DRAG_MS : UP_SETTLE_MS;
        if (now - s_up_since < settle) {
            s_tally.weak++;
            tally_tick(false, s_state.down);
            return 0;
        }
        s_up_since = 0u;
        s_state.down = false;
        out[0].type = AG_EV_POINTER_UP;
        out[0].ptr.x = s_state.col;
        out[0].ptr.y = s_state.row;
        out[0].ptr.buttons = 0;
        s_tally.sent++;
        s_tally.ended++;
        tally_tick(true, s_state.down);
        return 1;
    }
    s_up_since = 0u;

    /*
     * Which raw axis is the screen's across.
     *
     * The panel is mounted landscape (MADCTL 0x40 in the display driver) and
     * the controller is not, so one of the two is turned relative to the
     * other - but which way is a fact about how the glass was glued on, not
     * something to reason out.  It was reasoned out first, wrongly: a stylus
     * drawn horizontally left a vertical line.
     */
    int16_t col, row;
    int     moved;

    if (s_px) {
        col = to_px(rx, PANEL_W);
        row = to_px(ry, PANEL_H);
#if FLIP_X
        col = (int16_t)(PANEL_W - 1 - col);
#endif
#if FLIP_Y
        row = (int16_t)(PANEL_H - 1 - row);
#endif
        const int dx = (col > s_state.col) ? col - s_state.col
                                           : s_state.col - col;
        const int dy = (row > s_state.row) ? row - s_state.row
                                           : s_state.row - row;
        moved = (dx >= DEADBAND || dy >= DEADBAND);
    } else {
        int cols, rows;
        grid(&cols, &rows);
        col = to_cell(rx, PANEL_W, cols);
        row = to_cell(ry, PANEL_H, rows);
#if FLIP_X
        col = (int16_t)(cols - 1 - col);
#endif
#if FLIP_Y
        row = (int16_t)(rows - 1 - row);
#endif
        moved = (col != s_state.col || row != s_state.row);
    }

    if (!s_state.down) {
        s_state.down = true;
        /* Where this contact began, for the settle above. */
        s_down_x = col;
        s_down_y = row;
        out[0].type = AG_EV_POINTER_DOWN;
    } else if (moved) {
        out[0].type = AG_EV_POINTER_MOVE;
    } else {
        s_tally.still++;
        tally_tick(false, s_state.down);
        return 0; /* still down, still in the same place: nothing happened */
    }

    out[0].ptr.buttons = 1;
    out[0].ptr.x = col;
    out[0].ptr.y = row;
    out[0].ptr.dx = (int16_t)(col - s_state.col);
    out[0].ptr.dy = (int16_t)(row - s_state.row);

    s_state.col = col;
    s_state.row = row;
    s_tally.sent++;
    tally_tick(true, s_state.down);
    return 1;
}

/*
 * Filled at load time rather than written down here, because which units this
 * driver reports depends on whether the kernel it landed on understands the
 * pixel ones (ABI 0.44).
 */
static ag_input_ops_t k_input_ops = {
    .size = sizeof(ag_input_ops_t),
    .poll = touch_poll,
    .units = AG_PTR_CELLS,
    .span_w = PANEL_W,
    .span_h = PANEL_H,
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

    /*
     * Which units, decided here and not at compile time: this .SYS outlives
     * the kernel it was built beside, and the answer is a property of the
     * kernel that loaded it.  Before ag_dev_add, because the table is read
     * from the moment the device exists.
     */
    ag_sysinfo_t si = { 0 };
    ag_api()->sys->info(&si);
    s_px = (si.abi_major > 0u) || (si.abi_minor >= 44u);
    k_input_ops.units = s_px ? AG_PTR_PIXELS : AG_PTR_CELLS;

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

    if (s_px) {
        ag_printf("XPT2046: spi%d cs %d at %d kHz, pen %d, %dx%d pixels\n",
                  T_BUS, T_CS, T_KHZ, T_IRQ, PANEL_W, PANEL_H);
    } else {
        int cols, rows;
        grid(&cols, &rows);
        ag_printf("XPT2046: spi%d cs %d at %d kHz, pen %d, %dx%d cells"
                  " (kernel 0.%u wants cells)\n", T_BUS, T_CS,
                  T_KHZ, T_IRQ, cols, rows, (unsigned)si.abi_minor);
    }
    return AG_OK;
}
