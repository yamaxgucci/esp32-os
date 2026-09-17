/*
 * ArgonOS - ST7789 panel driver (.SYS).
 *
 *   drv install c:\st7789.sys
 *   dev                       -> lcd0  display  ST7789
 *
 * Two panels, both ST7789, and one file: the Waveshare ESP32-C6-LCD-1.47's
 * 1.47 inch 172x320, driven landscape as 320x172, and a 2.0 inch 240x320
 * module soldered to the ESP32-S3-WROOM CAM board's header, driven landscape
 * as 320x240.  The difference between them is the block of constants below
 * and nothing else; -DST7789_BOARD_S3CAM picks the second, and
 * tools/apps.json builds both, as ST7789.SYS (RISC-V) and ST7789S3.SYS
 * (xtensa).
 *
 * Written from apps/ili9341/ili9341.c, which it
 * follows closely on purpose - the console path, the window tracking, the
 * scaler and the rules about what a driver may not do are that file's, and
 * copying them wrongly here would be a second set of bugs to find.  What is
 * genuinely different is written down where it happens: the offset, the
 * inversion, the reset pin, and the bus this panel has to share.
 *
 * The first image in this tree built for RISC-V.  A .SYS carries its
 * instruction set in its header, so this one is not interchangeable with
 * ILI9341.SYS even though most of it is the same source.
 *
 * Build:
 *   python tools/gen_font8x8.py
 *   python tools/mkaxe.py --arch riscv32 --gcc riscv32-esp-elf-gcc \
 *       --include sdk/include --include apps/common \
 *       -o build/apps/ST7789.SYS apps/st7789/st7789.c
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --cflags "-Os -ffunction-sections -fdata-sections -DST7789_BOARD_S3CAM" \
 *       --include sdk/include --include apps/common \
 *       -o build/apps/ST7789S3.SYS apps/st7789/st7789.c
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

#include "font8x8.h"

AG_DRV("ST7789", "0.2", "argon");

/*
 * Two boards, one panel controller, and the constants are the whole of the
 * difference.  They are constants rather than settings for the same reason
 * they are in the ILI9341 driver: a driver cannot read BOARD.CFG - api->cfg is
 * NULL - and a wrong guess here drives a pin belonging to something else.
 * Both sets are written down in the board's own BOARD.CFG as well, where a
 * person looking for them will look first.
 *
 * Which set is compiled in is -DST7789_BOARD_S3CAM, from tools/apps.json.  The
 * two images are not interchangeable for a second reason anyway: a .SYS
 * carries its instruction set in its header, and one of these is RISC-V.
 */
#if defined(ST7789_BOARD_WSLCD2)

/*
 * Waveshare ESP32-S3-Touch-LCD-2: a 2.0 inch 240x320 IPS panel, driven
 * landscape as 320x240, with a CST816D touch controller on the same glass.
 *
 * The bus is shared with the microSD slot - clock 39, data 38, and a MISO on
 * 40 that only the card uses - so the chip select is the whole of what keeps
 * them apart, and this driver must never drive the bus pins itself.
 *
 * LCD_RST is -1, and that is deliberate rather than unknown-and-ignored.
 * CircuitPython's board file for this board calls GPIO0 "LCD_RESET"; GPIO0 is
 * also the BOOT button; and Waveshare's schematic shows LCD_RST reaching the
 * panel through a pair of fitting options (R16 NC/0R, R17 NC/10K) rather than
 * from a named GPIO.  Two sources, two answers, and driving the wrong pin is
 * worse than driving none - so the panel is brought up with SWRESET, which the
 * controller has for exactly this case.  If it ever comes up dark or
 * scrambled, GPIO0 here is the first thing to try.
 */
#define LCD_BUS      2
#define LCD_CS      45
#define LCD_DC      42
#define LCD_RST     -1
#define LCD_BL       1

/* Landscape.  Which of the two landscapes is right depends on which end of
 * the flex the glass was attached by, and no document says; 0x60 is the first
 * guess and the screen settles it, as it did on the other two boards. */
#define LCD_MADCTL 0x60

#define LCD_W      320
#define LCD_H      240

/* The controller addresses 240x320 and this glass is all of it. */
#define LCD_X_OFF 0
#define LCD_Y_OFF 0

#elif defined(ST7789_BOARD_S3CAM)

/*
 * A 2.0 inch 240x320 module (020-06PS V2.2, GM1020-06 flex) soldered to the
 * header of the ESP32-S3-WROOM CAM board, driven landscape as 320x240.
 *
 * The bus pins - clock 12, data 11 - are SPI2's IOMUX pins on this part, which
 * is why the panel is not capped at 40 MHz the way the C6's is.  The chip
 * select is FSPICS0, and DC sits on 13, which would have been MISO: this panel
 * has no way to answer, so nothing is lost by spending it.
 *
 * The backlight is on 46 and the chip select is not, deliberately.  46 is a
 * strapping pin that must be low through reset and has an internal pull-down;
 * BL on the module is a transistor base, which pulls the same way, whereas its
 * CS may well have a pull-up to VDD - and a pull-up on 46 is a board that
 * boots into download mode.
 *
 * Everything here except 3 and 46 used to be the camera.  See BOARD.CFG.
 */
#define LCD_BUS      2
#define LCD_CS      10
#define LCD_DC      13
#define LCD_RST      3
#define LCD_BL      46

/*
 * Landscape.  MV turns the addressing on its side; MX or MY then says which
 * corner is the origin, so 0x60 (MV|MX) and 0xA0 (MV|MY) are the two
 * landscapes, one the other's 180-degree rotation.  Bit 3 clear is RGB: a
 * picture with the reds and blues exchanged is that bit and nothing else.
 *
 * Which of the two is right is not derivable - it is which end of the flex the
 * module was soldered by - so this is the first guess and the screen settles
 * it.
 */
#define LCD_MADCTL 0x60

#define LCD_W      320
#define LCD_H      240

/*
 * No offset, and that is the easy case: the controller addresses 240x320 and
 * this glass is all of it.  The C6's panel is 172 rows inside those 240 and
 * has to be told where they start.
 */
#define LCD_X_OFF 0
#define LCD_Y_OFF 0

#else /* the Waveshare ESP32-C6-LCD-1.47 */

/*
 * The board this was written on.
 *
 * The chip select is the whole of what separates this panel from the SD card:
 * both are on SPI2, both share clock and data, and only 14 and 4 tell them
 * apart.  The port keeps one SPI device per chip select with a clock of its
 * own, so nothing here has to know the card exists - but it is why this driver
 * must never drive the bus pins itself.
 */
#define LCD_BUS      2
#define LCD_CS      14
#define LCD_DC      15
#define LCD_RST     21
#define LCD_BL      22

/*
 * Landscape, and which way up.
 *
 * MV turns the addressing on its side; MX or MY then decides which corner is
 * the origin, which is to say which end of the board the first row is at.
 * 0x60 is MV|MX and 0xA0 is MV|MY - the two landscapes, one the other's
 * 180-degree rotation.
 *
 * 0x60 reads the right way up with the USB socket on the right, which is the
 * board held as a phone is held in landscape and is what this was checked on.
 * 0xA0 is the same screen for somebody who puts the cable out the other side;
 * it is the only line to change, because the offset below is the same either
 * way - the 172 rows sit centred in the controller's 240 (34 above, 34 below),
 * so mirroring the axis does not move them.
 *
 * Bit 3 is left clear: this panel is RGB, not BGR.  A picture with the reds
 * and blues exchanged is that bit and nothing else.
 */
#define LCD_MADCTL 0x60

#define LCD_W      320
#define LCD_H      172

/*
 * Where the glass is inside the controller.
 *
 * The ST7789 addresses 240x320 whatever is attached to it; this panel is 172
 * wide, centred, so the short axis starts 34 in.  Landscape puts the short
 * axis on the rows, which is why the offset is on Y here and would be on X in
 * portrait.  Getting this wrong does not look like an offset - it looks like a
 * screen with a band of noise down one side and everything shifted, because
 * the rows past the end still exist in memory and are simply not lit.
 */
#define LCD_X_OFF 0
#define LCD_Y_OFF 34

#endif

#define CELL_W AG_FONT8X8_W
#define CELL_H AG_FONT8X8_H
#define COLS   (LCD_W / CELL_W) /* 40 */
#define ROWS   (LCD_H / CELL_H) /* 30 on the S3 panel; 21 on the C6, four spare */

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
 * How much the port will take in one transfer, found by asking.  See the note
 * in ili9341.c: the limit is not published, so this starts optimistic and
 * halves on the first refusal, and the number matters because a transfer costs
 * about seventy microseconds of setup whatever its size.
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
 * The last window set, and where writing has got to inside it.  Same idea as
 * the ILI9341 driver: the controller wraps to the next row by itself, so a
 * rectangle continuing exactly where the last one stopped needs no new window,
 * and a frame arriving as bands then costs one window instead of eighteen.
 */
static uint16_t s_win_x0, s_win_x1, s_win_y1;
static uint16_t s_win_next; /* the row the controller will write next */
static bool     s_win_live;

static void window_forget(void) { s_win_live = false; }

static void window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    const uint16_t cx0 = (uint16_t)(x0 + LCD_X_OFF);
    const uint16_t cx1 = (uint16_t)(x1 + LCD_X_OFF);
    const uint16_t cy0 = (uint16_t)(y0 + LCD_Y_OFF);
    const uint16_t cy1 = (uint16_t)(y1 + LCD_Y_OFF);
    const uint8_t  ca[4] = {(uint8_t)(cx0 >> 8), (uint8_t)cx0,
                            (uint8_t)(cx1 >> 8), (uint8_t)cx1};
    const uint8_t  pa[4] = {(uint8_t)(cy0 >> 8), (uint8_t)cy0,
                            (uint8_t)(cy1 >> 8), (uint8_t)cy1};
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
     * sends, but sending a whole row here would blank the rest of the line the
     * caret is standing on.  The character underneath comes with the call, so
     * the caret can invert it instead of covering it.
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
 * A surface the kernel owns, put on glass that is a different size (ABI 0.30).
 *
 * The default here is 160x86 - half of 320x172 - because 110 KB of framebuffer
 * out of this chip's 240 is more than the radio can spare.  At that size the
 * scale is exactly two and the picture covers the glass edge to edge with no
 * margin at all.  The arithmetic below is the ILI9341 driver's and handles any
 * other size the board is configured for, at the largest exact ratio that fits.
 */
static uint16_t s_surf_w, s_surf_h;

/*
 * The picture's size on the glass, as a ratio rather than a whole number: at a
 * whole number a 160x144 source on a 320x172 panel would only fit at 1:1, a
 * small picture in the middle of a screen it nearly fills.  The ratio that fits
 * is chosen per axis by cross-multiplying, and the same one is used for both so
 * a circle stays a circle.  Nearest neighbour at an exact ratio: no pixel is
 * averaged with its neighbours, so nothing is blurred.
 */
static uint32_t s_num = 1, s_den = 1; /* the scale, num/den */
static uint16_t s_out_w, s_out_h;     /* the picture on the glass */
static uint16_t s_off_x, s_off_y;     /* where it starts */

/*
 * Which source column each output column comes from.  Worked out once per
 * surface rather than per pixel: a divide per entry against a divide per pixel.
 */
static uint16_t s_colmap[LCD_W];

static void clear_panel(void)
{
    for (size_t i = 0; i < sizeof(s_row) / sizeof(s_row[0]); i++) {
        s_row[i] = 0;
    }
    window(0, 0, LCD_W - 1, LCD_H - 1);
    /*
     * The buffer is eight rows tall and the panel is 172, which is not a
     * multiple of eight: the last band would run four rows past the bottom of
     * the window and the controller would wrap them onto the top.  So the
     * whole bands go first and the remainder goes as its own short transfer.
     */
    for (uint16_t band = 0; band < LCD_H / CELL_H; band++) {
        data(s_row, sizeof(s_row));
    }
    if ((LCD_H % CELL_H) != 0) {
        data(s_row, (size_t)(LCD_H % CELL_H) * LCD_W * sizeof(uint16_t));
    }
    window_forget();
}

/* Works out the placement, and wipes the glass when it has changed. */
static void fit_surface(uint16_t w, uint16_t h)
{
    if (w == s_surf_w && h == s_surf_h) {
        return;
    }
    if (w == 0 || h == 0) {
        return;
    }

    s_surf_w = w;
    s_surf_h = h;

    /*
     * The larger scale that still fits, as an exact ratio: whichever of
     * LCD_W/w and LCD_H/h is the smaller.  Compared by cross-multiplying so
     * there is no division and no rounding in the comparison itself.
     */
    if ((uint32_t)LCD_W * h <= (uint32_t)LCD_H * w) {
        s_num = LCD_W;
        s_den = w;
    } else {
        s_num = LCD_H;
        s_den = h;
    }

    s_out_w = (uint16_t)(((uint32_t)w * s_num) / s_den);
    s_out_h = (uint16_t)(((uint32_t)h * s_num) / s_den);
    if (s_out_w > LCD_W) {
        s_out_w = LCD_W;
    }
    if (s_out_h > LCD_H) {
        s_out_h = LCD_H;
    }
    s_off_x = (uint16_t)((LCD_W - s_out_w) / 2u);
    s_off_y = (uint16_t)((LCD_H - s_out_h) / 2u);

    for (uint16_t i = 0; i < s_out_w; i++) {
        uint32_t c = ((uint32_t)i * s_den) / s_num;
        if (c >= w) {
            c = w - 1u;
        }
        s_colmap[i] = (uint16_t)c;
    }

    /*
     * The margins are ours and they still have the console on them.
     *
     * Nothing is printed here, and that is a rule rather than a preference:
     * see the note on blit_rect in argon/abi.h.  A print from inside this call
     * deadlocks the board.
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
    if (s_out_w == 0 || s_out_h == 0) {
        return;
    }

    const uint16_t x = b->x, y = b->y, w = b->w, hgt = b->h;
    if (x >= s_surf_w || y >= s_surf_h) {
        return;
    }

    /*
     * The rectangle in output rows and columns.
     *
     * Floor division at both ends, which is what makes consecutive rectangles
     * meet exactly: the end of one is the start of the next, so a frame
     * arriving as a run of bands covers every row once and none twice.
     */
    const uint16_t ox0 = (uint16_t)(((uint32_t)x * s_num) / s_den);
    const uint16_t oy0 = (uint16_t)(((uint32_t)y * s_num) / s_den);
    uint16_t       ox1 = (uint16_t)((((uint32_t)x + w) * s_num) / s_den);
    uint16_t       oy1 = (uint16_t)((((uint32_t)y + hgt) * s_num) / s_den);
    if (ox1 > s_out_w) {
        ox1 = s_out_w;
    }
    if (oy1 > s_out_h) {
        oy1 = s_out_h;
    }
    if (ox1 <= ox0 || oy1 <= oy0) {
        return;
    }
    const uint16_t ow = (uint16_t)(ox1 - ox0);

    window_for((uint16_t)(s_off_x + ox0), (uint16_t)(s_off_y + oy0),
               (uint16_t)(s_off_x + ox1 - 1), (uint16_t)(s_off_y + oy1 - 1));

    /* Output rows that fit the row buffer at this width; never zero. */
    uint16_t cap = (uint16_t)((sizeof(s_row) / sizeof(s_row[0])) / ow);
    if (cap == 0) {
        cap = 1;
    }

    const uint8_t *const src = (const uint8_t *)b->px;
    uint16_t             held = 0;

    for (uint16_t j = oy0; j < oy1; j++) {
        /* Which source row this output row draws from, and where it is in the
         * rectangle we were handed. */
        uint32_t srow = ((uint32_t)j * s_den) / s_num;
        if (srow < y) {
            srow = y;
        }
        if (srow >= (uint32_t)y + hgt) {
            srow = (uint32_t)y + hgt - 1u;
        }
        const uint16_t *in = (const uint16_t *)(const void *)
            (src + (size_t)(srow - y) * b->stride);

        if (held == cap) {
            data(s_row, (size_t)held * ow * sizeof(uint16_t));
            window_advance(held);
            held = 0;
        }
        uint16_t *out = &s_row[(size_t)held * ow];
        for (uint16_t i = 0; i < ow; i++) {
            out[i] = swap16(in[s_colmap[ox0 + i] - x]);
        }
        held++;
    }

    if (held != 0) {
        data(s_row, (size_t)held * ow * sizeof(uint16_t));
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
 * The panel is not a stream of bytes, so read and write are absent: `type
 * d:\lcd0` says so rather than doing something.  The one ioctl is the light.
 *
 * The backlight here is a transistor on a plain output, not a PWM channel, so
 * any percentage above zero is "on".  The controller is put to sleep as well as
 * darkened: that is the larger half of the saving, because a panel left
 * scanning keeps its charge pumps and its oscillator running for a picture
 * nobody can see.  Waking it costs the datasheet's 120 ms and a cleared screen.
 */
static ag_err_t lcd_ioctl(ag_device_t *dev, uint32_t code, void *arg,
                          size_t arglen)
{
    (void)dev;

    if (code != AG_IOC_DISPLAY_BACKLIGHT) {
        return -AG_ENOTSUP;
    }
    if (arg == NULL || arglen < sizeof(uint8_t)) {
        return -AG_EINVAL;
    }
    if (!s_up) {
        return -AG_ENODEV;
    }

    const uint8_t percent = *(const uint8_t *)arg;
    if (percent == 0u) {
        io->gpio_write(LCD_BL, 0);
        cmd(0x28); /* display off */
        cmd(0x10); /* sleep in    */
        return AG_OK;
    }

    cmd(0x11); /* sleep out */
    ag_api()->time->delay_ms(120);
    clear_panel();
    cmd(0x29); /* display on */
    io->gpio_write(LCD_BL, 1);
    return AG_OK;
}

static const ag_dev_ops_t k_dev_ops = {
    .ioctl = lcd_ioctl,
};

/* ---- bring-up ---------------------------------------------------------- */

static bool panel_init(void)
{
    /*
     * The ST7789's own defaults are usable and most of this is the datasheet's
     * recommended set rather than anything discovered here.  The three lines
     * that are not optional are marked.
     */
    static const uint8_t porctrl[] = {0x0c, 0x0c, 0x00, 0x33, 0x33};
    static const uint8_t gctrl[]   = {0x35};
    static const uint8_t vcoms[]   = {0x19};
    static const uint8_t lcmctrl[] = {0x2c};
    static const uint8_t vdvvrhen[] = {0x01};
    static const uint8_t vrhs[]    = {0x12};
    static const uint8_t vdvs[]    = {0x20};
    static const uint8_t frctrl2[] = {0x0f};
    static const uint8_t pwctrl1[] = {0xa4, 0xa1};
    static const uint8_t pvgamctrl[] = {0xd0, 0x04, 0x0d, 0x11, 0x13, 0x2b,
                                        0x3f, 0x54, 0x4c, 0x18, 0x0d, 0x0b,
                                        0x1f, 0x23};
    static const uint8_t nvgamctrl[] = {0xd0, 0x04, 0x0c, 0x11, 0x13, 0x2c,
                                        0x3f, 0x44, 0x51, 0x2f, 0x1f, 0x1f,
                                        0x20, 0x23};
    static const uint8_t madctl[]  = {LCD_MADCTL};
    static const uint8_t pixfmt[]  = {0x55}; /* 16 bits, and this one matters */

    if (io->gpio_config(LCD_DC, AG_GPIO_OUT) != AG_OK ||
        io->gpio_config(LCD_BL, AG_GPIO_OUT) != AG_OK) {
        return false;
    }
    /* A board may have no reset line of its own; see LCD_RST above. */
    if (LCD_RST >= 0 && io->gpio_config(LCD_RST, AG_GPIO_OUT) != AG_OK) {
        return false;
    }

    /* Dark until there is something of ours to show. */
    io->gpio_write(LCD_BL, 0);

    /*
     * A real reset line, which the ILI9341 board did not have.  Worth using:
     * the panel comes out of it in a known state whatever the last firmware
     * left in its registers, and this board ships with a factory demo that
     * leaves plenty.
     */
    if (LCD_RST >= 0) {
        io->gpio_write(LCD_RST, 1);
        ag_api()->time->delay_ms(10);
        io->gpio_write(LCD_RST, 0);
        ag_api()->time->delay_ms(10);
        io->gpio_write(LCD_RST, 1);
        ag_api()->time->delay_ms(120);
    }

    cmd(0x01); /* software reset as well, for the registers RST does not clear */
    ag_api()->time->delay_ms(150);

    cmd(0x11); /* sleep out - nothing below this takes effect without it */
    ag_api()->time->delay_ms(120);

    cmd_data(0x36, madctl, sizeof(madctl));
    cmd_data(0x3a, pixfmt, sizeof(pixfmt));

    cmd_data(0xb2, porctrl, sizeof(porctrl));
    cmd_data(0xb7, gctrl, sizeof(gctrl));
    cmd_data(0xbb, vcoms, sizeof(vcoms));
    cmd_data(0xc0, lcmctrl, sizeof(lcmctrl));
    cmd_data(0xc2, vdvvrhen, sizeof(vdvvrhen));
    cmd_data(0xc3, vrhs, sizeof(vrhs));
    cmd_data(0xc4, vdvs, sizeof(vdvs));
    cmd_data(0xc6, frctrl2, sizeof(frctrl2));
    cmd_data(0xd0, pwctrl1, sizeof(pwctrl1));
    cmd_data(0xe0, pvgamctrl, sizeof(pvgamctrl));
    cmd_data(0xe1, nvgamctrl, sizeof(nvgamctrl));

    /*
     * Inversion on, and it is not a preference.  This is a normally-black IPS
     * panel wired the way every 172x320 module of this size is: without INVON
     * every colour comes out as its complement - white text on a white screen,
     * which reads as a backlight that came on over nothing.
     */
    cmd(0x21);
    cmd(0x13); /* normal display mode (no partial, no idle) */

    cmd(0x29); /* display on */
    ag_api()->time->delay_ms(20);

    /*
     * Black, everywhere, before the backlight comes on: whatever the panel was
     * showing is not ours and must not be handed to the user as if it were.
     * On this board that is exactly the case - what is in the controller's
     * memory at this moment is the factory demo, or the noise it powered up
     * with.
     */
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
        .driver = "ST7789",
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

    ag_printf("ST7789: %dx%d, %dx%d cells, backlight %d\n", LCD_W, LCD_H, COLS,
              ROWS, LCD_BL);
    return AG_OK;
}
