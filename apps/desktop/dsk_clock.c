#include "dsk_clock.h"

#include "dsk.h"
#include "dsk_paint.h"

/*
 * Which of the seven segments each digit lights, one bit per segment:
 *
 *      a          bit 0  a  top
 *    f   b        bit 1  b  upper right
 *      g          bit 2  c  lower right
 *    e   c        bit 3  d  bottom
 *      d          bit 4  e  lower left
 *                 bit 5  f  upper left
 *                 bit 6  g  middle
 */
#define SEG_A 0x01u
#define SEG_B 0x02u
#define SEG_C 0x04u
#define SEG_D 0x08u
#define SEG_E 0x10u
#define SEG_F 0x20u
#define SEG_G 0x40u

static const uint8_t k_digits[10] = {
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,         /* 0 */
    SEG_B | SEG_C,                                         /* 1 */
    SEG_A | SEG_B | SEG_G | SEG_E | SEG_D,                 /* 2 */
    SEG_A | SEG_B | SEG_G | SEG_C | SEG_D,                 /* 3 */
    SEG_F | SEG_G | SEG_B | SEG_C,                         /* 4 */
    SEG_A | SEG_F | SEG_G | SEG_C | SEG_D,                 /* 5 */
    SEG_A | SEG_F | SEG_G | SEG_E | SEG_C | SEG_D,         /* 6 */
    SEG_A | SEG_B | SEG_C,                                 /* 7 */
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G, /* 8 */
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,         /* 9 */
};

/*
 * Grey, not white.  This is meant to be looked at in a dark room by someone
 * who did not ask to be looked back at, and on a panel whose light is a
 * plain transistor the only brightness the shell controls is this one.
 */
#define CLOCK_INK DSK_LGRAY

static void bar(int x, int y, int w, int h)
{
    dsk_rect_t r;
    r.x = (int16_t)x;
    r.y = (int16_t)y;
    r.w = (int16_t)w;
    r.h = (int16_t)h;
    dsk_fill(r, CLOCK_INK);
}

/*
 * One digit in a w x h box at (x, y), strokes `t` thick.
 *
 * The horizontals are inset by `t` at each end and the verticals by `t` at
 * top and bottom, so the corners are mitred by absence: no segment paints a
 * pixel another one has, which matters on a band renderer where every
 * rectangle is its own strip of work.
 */
static void digit(int x, int y, int w, int h, int t, uint8_t segs)
{
    const int mid = y + (h - t) / 2;
    const int vh = mid - y - t; /* height of one vertical run */

    if (segs & SEG_A) {
        bar(x + t, y, w - 2 * t, t);
    }
    if (segs & SEG_D) {
        bar(x + t, y + h - t, w - 2 * t, t);
    }
    if (segs & SEG_G) {
        bar(x + t, mid, w - 2 * t, t);
    }
    if (segs & SEG_F) {
        bar(x, y + t, t, vh);
    }
    if (segs & SEG_B) {
        bar(x + w - t, y + t, t, vh);
    }
    if (segs & SEG_E) {
        bar(x, mid + t, t, vh);
    }
    if (segs & SEG_C) {
        bar(x + w - t, mid + t, t, vh);
    }
}

void dsk_clock_draw(dsk_rect_t screen, int hour, int minute, bool valid,
                    unsigned nudge)
{
    dsk_fill(screen, DSK_BLACK);

    /*
     * Four digits, a colon between them, and a margin: nine digit-widths
     * across, which puts a 320-pixel screen at 35 and leaves the strokes
     * thick enough to read from the other side of a desk.
     */
    int w = screen.w / 9;
    if (w < 6) {
        w = 6;
    }
    int h = w * 2;
    if (h > screen.h / 2) {
        h = screen.h / 2;
    }
    int t = w / 5;
    if (t < 2) {
        t = 2;
    }

    const int gap = w / 3;
    const int colon_w = w / 2;
    const int total = 4 * w + 3 * gap + colon_w;

    /*
     * The nudge walks the block around a small box rather than along a line:
     * a clock that drifts to one edge and stays there has only moved once.
     */
    const int slack_x = (screen.w - total) / 2;
    const int slack_y = (screen.h - h) / 2;
    const int dx = (int)(nudge % 5u) - 2;
    const int dy = (int)((nudge / 5u) % 5u) - 2;
    const int step_x = (slack_x > 8) ? 4 : 1;
    const int step_y = (slack_y > 8) ? 4 : 1;

    int x = screen.x + slack_x + dx * step_x;
    const int y = screen.y + slack_y + dy * step_y;

    const int d[4] = { hour / 10, hour % 10, minute / 10, minute % 10 };

    for (int i = 0; i < 4; i++) {
        if (!valid) {
            digit(x, y, w, h, t, SEG_G); /* --:-- */
        } else if (i == 0 && d[0] == 0) {
            /* No leading zero: 07:05 reads as a date, 7:05 as a time. */
        } else {
            digit(x, y, w, h, t, k_digits[d[i]]);
        }
        x += w + gap;
        if (i == 1) {
            /*
             * The colon, and it does not blink.  A blinking colon is a
             * repaint every second for the life of the saver, which is the
             * one thing this screen exists not to do.
             */
            const int cy = y + h / 3;
            bar(x - gap / 2 - t / 2, cy, t, t);
            bar(x - gap / 2 - t / 2, y + h - h / 3 - t, t, t);
            x += colon_w - gap;
        }
    }

}
