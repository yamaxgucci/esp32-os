/*
 * MIDIPAD - a six-button MIDI pad you play by touch.
 *
 * Two rows of three buttons fill the screen.  Touch one and the board sends a
 * MIDI note-on over BLE-MIDI to whatever connected to it - a phone running a
 * synth plays the note.  It is the smallest thing that ties three of ArgonOS's
 * pieces together: the panel, the touch screen, and the BLE peripheral.
 *
 * Two facts about this board shape the whole file:
 *
 * 1. There is no system framebuffer on it ("display: no system surface;
 *    applications bring their own").  In that state gfx_acquire hands back
 *    fb = NULL and every soft-draw call is a no-op - so fill_rect and text
 *    would draw precisely nothing.  The way to put pixels on the glass is
 *    gfx->present (ABI 0.31): the application owns the pixels and hands them
 *    over a band at a time, which is what apps/gfxpix does on this same board.
 *    A band of 160x20 is 6 KB; the system surface would have been 37 KB, and
 *    those kilobytes are the difference between this app running and not -
 *    with both radios linked and BLE up there are barely fifty to spend.
 *
 * 2. Touch arrives in console cells (40x30 here), not pixels, because that is
 *    what the XPT2046 driver reports.  So the hit test works in proportions -
 *    which column of three, which row of two - and is right whatever either
 *    coordinate space happens to be.
 *
 * The surface is 160x120 and the panel is 320x240, so the driver puts it up at
 * exactly two-to-one: no resampling, no letterbox.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/argon.h>

/*
 * Six kilobytes of stack rather than the default sixteen.  This is not tidiness:
 * the stack comes out of internal SRAM for the life of the process, BLE wants
 * about forty-seven kilobytes to come up, and on this board there are about
 * seventy to go round.  The app draws from a static band and recurses nowhere.
 */
AG_APP_SIZED("MIDIPAD", "1.0", "ArgonOS", AG_AXE_NEEDS_GFX, 6144, 0);

#define SURF_W 160
#define SURF_H 120
#define BAND   20 /* rows per present: one window command, one transfer */

#define COLS 3
#define ROWS 2
#define PADS (COLS * ROWS)

static uint16_t s_band[SURF_W * BAND];

/* C major, one octave from middle C: top row C D E, bottom row F G A. */
static const uint8_t k_note[PADS] = {60, 62, 64, 65, 67, 69};

/* 8x8 glyphs for the six labels, drawn three times up (24x24 on the surface). */
static const uint8_t k_glyph[PADS][8] = {
    {0x3c, 0x66, 0xc0, 0xc0, 0xc0, 0x66, 0x3c, 0x00}, /* C */
    {0xf8, 0xcc, 0xc6, 0xc6, 0xc6, 0xcc, 0xf8, 0x00}, /* D */
    {0xfe, 0xc0, 0xc0, 0xfc, 0xc0, 0xc0, 0xfe, 0x00}, /* E */
    {0xfe, 0xc0, 0xc0, 0xfc, 0xc0, 0xc0, 0xc0, 0x00}, /* F */
    {0x3c, 0x66, 0xc0, 0xce, 0xc6, 0x66, 0x3e, 0x00}, /* G */
    {0x38, 0x6c, 0xc6, 0xfe, 0xc6, 0xc6, 0xc6, 0x00}, /* A */
};

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | (b >> 3));
}

/* One colour per pad, and a brighter version of it while it is held. */
static uint16_t pad_colour(int i, bool pressed, bool live)
{
    static const uint8_t rgb[PADS][3] = {
        {176, 48, 48},  {176, 96, 32}, {160, 160, 32},
        {48, 160, 64},  {32, 128, 176}, {64, 80, 192},
    };
    uint8_t r = rgb[i][0], g = rgb[i][1], b = rgb[i][2];
    if (pressed) {
        r = (uint8_t)(r + (255 - r) / 2);
        g = (uint8_t)(g + (255 - g) / 2);
        b = (uint8_t)(b + (255 - b) / 2);
    } else if (!live) {
        /* Nobody is listening yet: the keys are dimmed, which is the whole
         * status display this app needs - no text, no room wasted. */
        r /= 3;
        g /= 3;
        b /= 3;
    }
    return rgb565(r, g, b);
}

static void pad_bounds(int i, int *x0, int *y0, int *w, int *h)
{
    const int col = i % COLS;
    const int row = i / COLS;
    const int xa = col * SURF_W / COLS, xb = (col + 1) * SURF_W / COLS;
    const int ya = row * SURF_H / ROWS, yb = (row + 1) * SURF_H / ROWS;
    *x0 = xa;
    *y0 = ya;
    *w = xb - xa;
    *h = yb - ya;
}

/*
 * The whole picture, band by band.  Redrawing everything on a press costs one
 * surface's worth of SPI (37 KB, a handful of milliseconds) and keeps the code
 * to one path; a pad is not a game loop.
 */
static void draw(int held, bool live)
{
    const uint16_t gap = rgb565(16, 16, 20);

    for (int y0 = 0; y0 < SURF_H; y0 += BAND) {
        const int rows = (SURF_H - y0 < BAND) ? (SURF_H - y0) : BAND;

        for (int row = 0; row < rows; row++) {
            const int y = y0 + row;
            for (int x = 0; x < SURF_W; x++) {
                const int col = x * COLS / SURF_W;
                const int prow = y * ROWS / SURF_H;
                const int idx = prow * COLS + col;

                int px, py, pw, ph;
                pad_bounds(idx, &px, &py, &pw, &ph);

                uint16_t c;
                /* A two pixel margin: separate keys, not one striped block. */
                if (x - px < 2 || py + ph - y <= 2 || px + pw - x <= 2 ||
                    y - py < 2) {
                    c = gap;
                } else {
                    c = pad_colour(idx, idx == held, live);

                    /* The label, three times up and centred. */
                    const int gx = px + (pw - 24) / 2;
                    const int gy = py + (ph - 24) / 2;
                    if (x >= gx && x < gx + 24 && y >= gy && y < gy + 24) {
                        const int bit = (x - gx) / 3;
                        const int lin = (y - gy) / 3;
                        if ((k_glyph[idx][lin] >> (7 - bit)) & 1u) {
                            c = rgb565(255, 255, 255);
                        }
                    }
                }
                s_band[row * SURF_W + x] = c;
            }
        }

        const ag_blit_t b = {
            .px = s_band,
            .stride = SURF_W * sizeof(uint16_t),
            .surf_w = SURF_W,
            .surf_h = SURF_H,
            .x = 0,
            .y = (uint16_t)y0,
            .w = SURF_W,
            .h = (uint16_t)rows,
        };
        (void)ag_gfx_present(&b);
    }
}

/* Touch cell (console grid) -> which pad, or -1.  Proportional on purpose. */
static int hit_pad(int16_t px, int16_t py, uint16_t cols, uint16_t rows)
{
    if (cols == 0 || rows == 0 || px < 0 || py < 0) {
        return -1;
    }
    int col = (int)px * COLS / (int)cols;
    int row = (int)py * ROWS / (int)rows;
    if (col >= COLS) col = COLS - 1;
    if (row >= ROWS) row = ROWS - 1;
    return row * COLS + col;
}

int ag_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (ag_api()->gfx == NULL || !AG_HAS(ag_api()->gfx, present)) {
        ag_printf("midipad: this kernel cannot show application pixels\n");
        return 1;
    }
    if (!ag_ble_available()) {
        ag_printf("midipad: this build has no BLE peripheral\n");
        return 1;
    }

    ag_gfxinfo_t info;
    if (ag_gfx_acquire(&info) != AG_OK) {
        ag_printf("midipad: the display belongs to somebody else\n");
        return 1;
    }

    const ag_err_t aerr = ag_ble_midi_advertise("ArgMIDI");
    if (aerr != AG_OK) {
        ag_gfx_release();
        ag_printf("midipad: BLE would not start (%d)\n", (int)aerr);
        return 1;
    }
    ag_printf("midipad: advertising as ArgMIDI - connect a MIDI app\n");

    ag_coninfo_t ci;
    ci.cols = 40;
    ci.rows = 30;
    if (ag_api()->con != NULL && ag_api()->con->info != NULL) {
        ag_api()->con->info(&ci);
    }

    bool live = ag_ble_midi_ready();
    int  held = -1;
    int  hold = 0;
    draw(held, live);

    for (;;) {
        ag_event_t ev;
        const bool got = ag_poll_event(&ev, 100);

        /* A phone connecting or leaving changes how the keys look. */
        const bool now = ag_ble_midi_ready();
        if (now != live) {
            live = now;
            draw(held, live);
        }

        if (!got) {
            /* Let a held note expire, so a release that never arrived does
             * not leave a note sounding for ever. */
            if (held >= 0 && --hold <= 0) {
                (void)ag_ble_midi_send(0x80, k_note[held], 0);
                held = -1;
                draw(held, live);
            }
            continue;
        }

        if (ev.type == AG_EV_QUIT) {
            break;
        }
        if (ev.type == AG_EV_FOCUS_GAINED) {
            draw(held, live);
            continue;
        }

        if (ev.type == AG_EV_POINTER_DOWN) {
            const int pad = hit_pad(ev.ptr.x, ev.ptr.y, ci.cols, ci.rows);
            if (pad < 0 || pad == held) {
                continue;
            }
            if (held >= 0) {
                (void)ag_ble_midi_send(0x80, k_note[held], 0);
            }
            (void)ag_ble_midi_send(0x90, k_note[pad], 100);
            held = pad;
            hold = 5; /* ~500 ms if no release ever arrives */
            draw(held, live);
        } else if (ev.type == AG_EV_POINTER_UP && held >= 0) {
            (void)ag_ble_midi_send(0x80, k_note[held], 0);
            held = -1;
            draw(held, live);
        }
    }

    if (held >= 0) {
        (void)ag_ble_midi_send(0x80, k_note[held], 0);
    }
    (void)ag_ble_adv_stop();
    ag_gfx_release();
    return 0;
}
