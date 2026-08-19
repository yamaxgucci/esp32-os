/*
 * ArgonOS - ILI9341 panel driver (.SYS).
 *
 *   drv install a:\ili9341.sys
 *   dev                       -> lcd0  display  ILI9341
 *
 * What it is for: the console, on the screen soldered to the board, with no
 * host attached.  It owns the panel from the moment it is loaded until it is
 * unloaded - the pins, the backlight and the initialisation - which is the
 * difference between a driver and the bring-up program that came before it
 * (apps/lcd).  That program drew a perfect frame and then exited, and exiting
 * gave the backlight pin back, and the screen went dark a millisecond later.
 *
 * There is no framebuffer here, and that is the design rather than a
 * shortcut: 320x240 in RGB565 is 150 KB, this chip has no PSRAM, and the
 * largest single block of memory free after boot is about a hundred.  So the
 * kernel sends the console as characters (ag_display_ops_t text_row, ABI 0.27)
 * and this turns one row of them into pixels at a time, in a buffer of 5 KB.
 * A row costs one window command and 320x8 pixels over SPI - about two
 * milliseconds - and only rows that changed are sent at all.
 *
 * Build:
 *   python tools/gen_font8x8.py
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32-elf-gcc \
 *       --include sdk/include --include apps/ili9341 \
 *       -o build/apps/ILI9341.SYS apps/ili9341/ili9341.c
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

#include "font8x8.h"

AG_DRV("ILI9341", "0.4", "argon");

/*
 * The board this was written on: an ESP32-2432S024, whose panel is on SPI2
 * with the pins in BOARD.CFG and whose control lines are these.  They are
 * constants because a driver cannot read BOARD.CFG - api->cfg is NULL - and
 * because a wrong guess here drives a pin that belongs to something else.
 * A board pack is the place this belongs when there is one; see
 * docs/03-board-config.md.
 *
 * Both numbers were found by looking rather than by reading: the 2.8 inch
 * board of the same family has its backlight on 21 and calls itself 240x320.
 */
#define LCD_BUS      2
#define LCD_CS      15
#define LCD_DC       2
#define LCD_BL      27
#define LCD_MADCTL 0x40 /* landscape, and no BGR bit: this panel is RGB */
#define LCD_W      320
#define LCD_H      240

#define CELL_W AG_FONT8X8_W
#define CELL_H AG_FONT8X8_H
#define COLS   (LCD_W / CELL_W) /* 40 */
#define ROWS   (LCD_H / CELL_H) /* 30 */

/* One row of text, as pixels.  The only buffer this driver has. */
static uint16_t s_row[LCD_W * CELL_H];

static const ag_io_api_t *io;
static bool               s_up;

/* ---- the wire ---------------------------------------------------------- */

static void cmd(uint8_t c)
{
    io->gpio_write(LCD_DC, 0);
    (void)io->spi_xfer(LCD_BUS, LCD_CS, &c, NULL, 1);
}

/*
 * How much the port will take in one transfer, found by asking.
 *
 * There is a limit - the SPI layer copies through a bounce buffer of its own -
 * and the ABI does not publish it, so this starts optimistic and halves on the
 * first refusal.  The number matters more than it looks: a transfer costs about
 * seventy microseconds of setup whatever its size, and a frame of 160x144 sent
 * as three hundred and twenty byte rows is two hundred and sixteen of those,
 * which is sixteen milliseconds of a thirty-two millisecond frame spent on
 * overhead rather than on pixels.
 */
static size_t s_chunk = 4096;

static void data(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    io->gpio_write(LCD_DC, 1);
    while (len > 0) {
        const size_t chunk = (len > s_chunk) ? s_chunk : len;
        const ag_err_t err = io->spi_xfer(LCD_BUS, LCD_CS, p, NULL, chunk);
        if (err == -AG_EINVAL && s_chunk > 64u) {
            /* Too large for this port; halve and try the same bytes again. */
            s_chunk /= 2u;
            continue;
        }
        if (err != AG_OK) {
            return;
        }
        p += chunk;
        len -= chunk;
    }
}

static void cmd_data(uint8_t c, const void *buf, size_t len)
{
    cmd(c);
    if (len != 0) {
        data(buf, len);
    }
}

/*
 * The last window set, and where writing has got to inside it.
 *
 * The controller walks its own window and wraps to the next row by itself, so a
 * rectangle that continues exactly where the previous one stopped needs no new
 * window at all.  A frame arriving as eighteen bands then costs one window
 * instead of eighteen, and a window is five transfers - three commands and two
 * pairs of coordinates - which came to nearly seven milliseconds a frame.
 *
 * Anything that breaks the sequence (a different column range, a jump, the
 * console's text path, a wipe) sets a window and the tracking starts again.
 */
static uint16_t s_win_x0, s_win_x1, s_win_y1;
static uint16_t s_win_next; /* the row the controller will write next */
static bool     s_win_live;

static void window_forget(void) { s_win_live = false; }

static void window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    const uint8_t ca[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0,
                           (uint8_t)(x1 >> 8), (uint8_t)x1};
    const uint8_t pa[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0,
                           (uint8_t)(y1 >> 8), (uint8_t)y1};
    cmd_data(0x2a, ca, sizeof(ca));
    cmd_data(0x2b, pa, sizeof(pa));
    cmd(0x2c);

    s_win_x0 = x0;
    s_win_x1 = x1;
    s_win_y1 = y1;
    s_win_next = y0;
    s_win_live = true;
}

/*
 * A window for rows y0..y1 of columns x0..x1, unless the controller is already
 * pointing exactly there.
 */
static void window_for(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    if (s_win_live && x0 == s_win_x0 && x1 == s_win_x1 && y0 == s_win_next &&
        y1 <= s_win_y1) {
        return;
    }
    window(x0, y0, x1, y1);
}

/* Rows just written, so the next call can tell whether it continues them. */
static void window_advance(uint16_t rows)
{
    if (!s_win_live) {
        return;
    }
    s_win_next = (uint16_t)(s_win_next + rows);
    if (s_win_next > s_win_y1) {
        s_win_live = false; /* the window is full; the next write must set one */
    }
}

/* ---- colour ------------------------------------------------------------ */

/*
 * The sixteen CGA colours, RGB565 with the bytes already in the order the
 * panel reads them (high first).  The console's attribute byte is a background
 * nibble and a foreground nibble, and has been since 1981.
 */
static uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static const uint16_t k_cga[16] = {
    0x0000, 0x0015, 0x0540, 0x0555, 0xa800, 0xa815, 0xaaa0, 0xad55,
    0x5295, 0x529f, 0x57ea, 0x57ff, 0xfa95, 0xfa9f, 0xffea, 0xffff,
};

/* ---- glyphs ------------------------------------------------------------ */

static void paint_cell_into_row(uint16_t col, uint8_t ch, uint8_t attr,
                                bool invert)
{
    uint8_t fg = (uint8_t)(attr & 0x0fu);
    uint8_t bg = (uint8_t)((attr >> 4) & 0x0fu);
    if (invert) {
        const uint8_t t = fg;
        fg = bg;
        bg = t;
    }
    const uint16_t fgc = swap16(k_cga[fg]);
    const uint16_t bgc = swap16(k_cga[bg]);
    const uint8_t *glyph = k_font8x8[ch];
    const uint32_t x0 = (uint32_t)col * CELL_W;

    for (uint32_t y = 0; y < CELL_H; y++) {
        const uint8_t bits = glyph[y];
        uint16_t     *out = &s_row[y * LCD_W + x0];
        for (uint32_t x = 0; x < CELL_W; x++) {
            /* Bit 0 is the leftmost pixel, as in the font this came from. */
            out[x] = (bits & (1u << x)) ? fgc : bgc;
        }
    }
}

static void push_row(uint16_t row)
{
    const uint16_t y0 = (uint16_t)(row * CELL_H);
    if (y0 + CELL_H > LCD_H) {
        return;
    }
    window(0, y0, LCD_W - 1, (uint16_t)(y0 + CELL_H - 1));
    data(s_row, sizeof(s_row));
    /* The console has moved the controller; a following blit must set its own
     * window rather than assume it is still where it left off. */
    window_forget();
}

/* ---- the class vtable -------------------------------------------------- */

static ag_err_t lcd_info(ag_handle_t h, ag_gfxinfo_t *out)
{
    (void)h;
    if (out == NULL) {
        return -AG_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->width = LCD_W;
    out->height = LCD_H;
    out->fmt = AG_PIX_RGB565;
    out->stride = LCD_W * 2u;
    out->fb = NULL; /* there is none, and that is the point */
    return AG_OK;
}

static ag_err_t lcd_text_info(ag_handle_t h, uint16_t *cols, uint16_t *rows)
{
    (void)h;
    if (cols != NULL) {
        *cols = COLS;
    }
    if (rows != NULL) {
        *rows = ROWS;
    }
    return AG_OK;
}

static void lcd_text_row(ag_handle_t h, uint16_t row,
                         const ag_textcell_t *cells, uint16_t count)
{
    (void)h;
    if (!s_up || cells == NULL || row >= ROWS) {
        return;
    }
    if (count > COLS) {
        count = COLS;
    }
    for (uint16_t x = 0; x < count; x++) {
        paint_cell_into_row(x, cells[x].ch, cells[x].attr, false);
    }
    /* Past the end of the console's own width: blank, not stale. */
    for (uint16_t x = count; x < COLS; x++) {
        paint_cell_into_row(x, ' ', 0x07u, false);
    }
    push_row(row);
}

/*
 * The caret is drawn as an inverted cell rather than an underline: the cell is
 * already the unit this driver sends, so a block costs one row of pixels and a
 * line under the character would cost the same and be one pixel tall on a
 * screen this size.
 */
static void lcd_text_cursor(ag_handle_t h, uint16_t col, uint16_t row,
                            ag_textcell_t under, bool visible)
{
    (void)h;
    if (!s_up || col >= COLS || row >= ROWS) {
        return;
    }
    /*
     * One cell, into a one-cell window: the row buffer is what this driver
     * sends, but sending a whole row here would blank the rest of the line
     * the caret is standing on.  The character underneath comes with the
     * call, so the caret can invert it instead of covering it.
     */
    paint_cell_into_row(0, under.ch, under.attr, visible);

    const uint16_t x0 = (uint16_t)(col * CELL_W);
    const uint16_t y0 = (uint16_t)(row * CELL_H);
    window(x0, y0, (uint16_t)(x0 + CELL_W - 1), (uint16_t)(y0 + CELL_H - 1));
    for (uint32_t y = 0; y < CELL_H; y++) {
        data(&s_row[y * LCD_W], CELL_W * sizeof(uint16_t));
    }
    window_forget();
}


/* ---- pixels ------------------------------------------------------------ */

/*
 * A surface the kernel owns, put on glass that is bigger than it (ABI 0.30).
 *
 * This board has 320x240 in front of it and about sixty kilobytes it can hand
 * out in one piece; a framebuffer of that size is a hundred and fifty.  So the
 * surface is smaller than the panel and something has to decide what to do
 * with the difference.  Here: scale it by the largest whole number that still
 * fits and centre what is left, which for the 160x120 the system asks for is
 * exactly two and no margin at all.
 *
 * A whole number and not a fraction, because the alternative is a resampler
 * and a line of pixels that changes width as it crosses the screen.  Doubling
 * is a copy.
 */
static uint16_t s_surf_w, s_surf_h;
static uint16_t s_scale, s_off_x, s_off_y;

static void clear_panel(void)
{
    for (size_t i = 0; i < sizeof(s_row) / sizeof(s_row[0]); i++) {
        s_row[i] = 0;
    }
    window(0, 0, LCD_W - 1, LCD_H - 1);
    for (uint16_t band = 0; band < LCD_H / CELL_H; band++) {
        data(s_row, sizeof(s_row));
    }
    window_forget();
}

/* Works out the placement, and wipes the glass when it has changed. */
static void fit_surface(uint16_t w, uint16_t h)
{
    if (w == s_surf_w && h == s_surf_h) {
        return;
    }
    uint16_t sx = (uint16_t)(w ? LCD_W / w : 1);
    uint16_t sy = (uint16_t)(h ? LCD_H / h : 1);
    uint16_t sc = (sx < sy) ? sx : sy;
    if (sc == 0) {
        sc = 1; /* a surface larger than the glass: show its top-left corner */
    }

    s_surf_w = w;
    s_surf_h = h;
    s_scale = sc;
    const uint16_t used_w = (uint16_t)(w * sc);
    const uint16_t used_h = (uint16_t)(h * sc);
    s_off_x = (used_w < LCD_W) ? (uint16_t)((LCD_W - used_w) / 2u) : 0u;
    s_off_y = (used_h < LCD_H) ? (uint16_t)((LCD_H - used_h) / 2u) : 0u;

    /*
     * The margins are ours and they still have the console on them.
     *
     * Nothing is printed here, and that is a rule rather than a preference:
     * see the note on blit_rect in argon/abi.h.  A print from inside this call
     * deadlocks the board, and it took a run that stopped between "text" and
     * "flushed" to find out.
     */
    clear_panel();
}


static void lcd_blit_rect(ag_handle_t h, const ag_blit_t *b)
{
    (void)h;
    if (!s_up || b == NULL || b->px == NULL || b->w == 0 || b->h == 0) {
        return;
    }
    fit_surface(b->surf_w, b->surf_h);

    /* Clip to what the glass can actually show at this scale. */
    uint16_t x = b->x, y = b->y, w = b->w, hgt = b->h;
    const uint16_t max_w = (uint16_t)((LCD_W - s_off_x) / s_scale);
    const uint16_t max_h = (uint16_t)((LCD_H - s_off_y) / s_scale);
    if (x >= max_w || y >= max_h) {
        return;
    }
    if (x + w > max_w) {
        w = (uint16_t)(max_w - x);
    }
    if (y + hgt > max_h) {
        hgt = (uint16_t)(max_h - y);
    }

    const uint16_t out_w = (uint16_t)(w * s_scale);
    const uint16_t x0 = (uint16_t)(s_off_x + x * s_scale);
    const uint16_t y0 = (uint16_t)(s_off_y + y * s_scale);

    /*
     * One window for the whole rectangle: the controller walks it itself, so
     * every row after the first is pixels and nothing else.  Four commands per
     * frame instead of four per line is most of the difference between a
     * picture that moves and one that crawls.
     */
    /*
     * One window for as long as the writes keep following on, and as many rows
     * per transfer as the row buffer holds.
     *
     * Both are about the same thing: a transfer costs about seventy microseconds
     * whatever it carries, and a window costs five transfers.  A 160x144 frame
     * arriving as eighteen bands used to be two hundred and sixteen transfers
     * and eighteen windows - twenty-five milliseconds of a thirty-two
     * millisecond frame, nearly all of it setup.  Sent this way it is a few
     * dozen transfers and one window.
     */
    window_for(x0, y0, (uint16_t)(x0 + out_w - 1),
               (uint16_t)(y0 + hgt * s_scale - 1));

    /* Output rows that fit the row buffer at this width; never zero. */
    uint16_t cap = (uint16_t)((sizeof(s_row) / sizeof(s_row[0])) / out_w);
    if (cap == 0) {
        cap = 1;
    }

    const uint8_t *src = (const uint8_t *)b->px;
    uint16_t       held = 0;

    for (uint16_t row = 0; row < hgt; row++) {
        const uint16_t *in = (const uint16_t *)(const void *)src;

        /*
         * Once per copy of this row, because at a scale above one the same row
         * goes out twice.  Expanded again rather than copied: the copy would
         * have to survive a flush in between, and expanding is the same walk.
         */
        for (uint16_t k = 0; k < s_scale; k++) {
            if (held == cap) {
                data(s_row, (size_t)held * out_w * sizeof(uint16_t));
                window_advance(held);
                held = 0;
            }
            uint16_t *out = &s_row[(size_t)held * out_w];
            uint16_t  o = 0;
            for (uint16_t i = 0; i < w; i++) {
                const uint16_t px = swap16(in[i]);
                for (uint16_t j = 0; j < s_scale; j++) {
                    out[o++] = px;
                }
            }
            held++;
        }
        src += b->stride;
    }

    if (held != 0) {
        data(s_row, (size_t)held * out_w * sizeof(uint16_t));
        window_advance(held);
    }
}


static const ag_display_ops_t k_display_ops = {
    .size = sizeof(ag_display_ops_t),
    .info = lcd_info,
    .acquire = NULL, /* no surface to hand out */
    .release = NULL,
    .flush = NULL,
    .swap = NULL,
    .text_info = lcd_text_info,
    .text_row = lcd_text_row,
    .text_cursor = lcd_text_cursor,
    .blit_rect = lcd_blit_rect,
};

/*
 * Nothing: a panel is not a stream of bytes, and `type d:\lcd0` should say so
 * rather than do something.  The class vtable above is the whole interface.
 */
static const ag_dev_ops_t k_dev_ops = {0};

/* ---- bring-up ---------------------------------------------------------- */

static bool panel_init(void)
{
    static const uint8_t pwctr1[] = {0x23};
    static const uint8_t pwctr2[] = {0x10};
    static const uint8_t vmctr1[] = {0x3e, 0x28};
    static const uint8_t vmctr2[] = {0x86};
    static const uint8_t pixfmt[] = {0x55};
    static const uint8_t frmctr[] = {0x00, 0x18};
    static const uint8_t dfunctr[] = {0x08, 0x82, 0x27};
    static const uint8_t madctl[] = {LCD_MADCTL};
    /* Whole panel scrolls, from row zero: undo what the last firmware set. */
    static const uint8_t vscrdef[] = {0x00, 0x00, 0x01, 0x40, 0x00, 0x00};
    static const uint8_t vscrsadd[] = {0x00, 0x00};

    if (io->gpio_config(LCD_DC, AG_GPIO_OUT) != AG_OK ||
        io->gpio_config(LCD_BL, AG_GPIO_OUT) != AG_OK) {
        return false;
    }

    cmd(0x01);
    ag_api()->time->delay_ms(150);

    cmd_data(0xc0, pwctr1, sizeof(pwctr1));
    cmd_data(0xc1, pwctr2, sizeof(pwctr2));
    cmd_data(0xc5, vmctr1, sizeof(vmctr1));
    cmd_data(0xc7, vmctr2, sizeof(vmctr2));
    cmd_data(0x36, madctl, sizeof(madctl));
    cmd_data(0x3a, pixfmt, sizeof(pixfmt));
    cmd_data(0xb1, frmctr, sizeof(frmctr));
    cmd_data(0xb6, dfunctr, sizeof(dfunctr));
    cmd(0x13);
    cmd(0x20);
    cmd_data(0x33, vscrdef, sizeof(vscrdef));
    cmd_data(0x37, vscrsadd, sizeof(vscrsadd));

    cmd(0x11);
    ag_api()->time->delay_ms(120);
    cmd(0x29);
    ag_api()->time->delay_ms(20);

    /* Black, everywhere, before the backlight comes on: whatever the panel
     * was showing is not ours and must not be handed to the user as if it
     * were. */
    clear_panel();

    io->gpio_write(LCD_BL, 1);
    return true;
}

ag_err_t ag_driver_init(void)
{
    io = ag_api()->io;
    if (io == NULL || !AG_HAS(io, spi_xfer)) {
        return -AG_ENOTSUP;
    }

    if (!panel_init()) {
        /* The usual cause is a pin somebody else holds; say which. */
        return -AG_EBUSY;
    }
    s_up = true;

    const ag_dev_add_t desc = {
        .name = "lcd0",
        .driver = "ILI9341",
        .cls = AG_DEV_DISPLAY,
        .ops = &k_dev_ops,
        .class_ops = &k_display_ops,
        .priv = NULL,
    };
    const ag_err_t err = ag_dev_add(&desc);
    if (err != AG_OK) {
        io->gpio_write(LCD_BL, 0);
        s_up = false;
        return err;
    }

    ag_printf("ILI9341: %dx%d, %dx%d cells, backlight %d\n", LCD_W, LCD_H,
              COLS, ROWS, LCD_BL);
    return AG_OK;
}
