/*
 * ArgonOS - ILI9341 bring-up, from an application.
 *
 *   run a:\lcd.axe                     colour bars and text, 30 s of backlight
 *   run a:\lcd.axe id                  identify only, touch nothing else
 *   run a:\lcd.axe bars 0xe8           the same, rotated (MADCTL)
 *   run a:\lcd.axe bars 0x28 120 21    ... held two minutes, backlight on 21
 *
 * This is deliberately not a driver.  A driver belongs in the system, owns the
 * panel, and has the console behind it; this exists to answer the question
 * that has to be answered first and cannot be answered by reasoning: is the
 * panel the one the schematic says, on the pins the schematic says, and does
 * it light up.  It uses nothing but the ABI - spi_xfer, gpio_config,
 * gpio_write - so a wrong answer here is about the board and not about the
 * kernel.
 *
 * The pins come from BOARD.CFG ([spi2] for the bus) and from the command line
 * (the panel's own control pins), because they are the part that differs
 * between boards that all call themselves the same thing.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/libc.h>

AG_APP("LCD", "0.1", "argon", 0);

/*
 * The yellow display boards, as sold.  Overridable, because it is not a law -
 * and on the first board this was run on it was wrong: the backlight of an
 * ESP32-2432S024 is GPIO 27, where its 2.8 inch cousin has 21, and everything
 * else about the two is the same.  The panel was drawn perfectly and nobody
 * could see it.  Hence the argument.
 */
#define LCD_BUS     2  /* SPI2 = HSPI, pins in BOARD.CFG                     */
#define LCD_CS     15
#define LCD_DC      2
#define LCD_BL     27 /* 21 on the 2.8" (ESP32-2432S028R)                    */

/*
 * 320 across, 240 down, and both of those were found by looking rather than by
 * reading: the glass is 2.4 inches and every description of it says 240x320,
 * which is the controller's memory and not the way this board mounts it.  The
 * evidence was a fill that left the right 30% of the screen showing the
 * firmware it was sold with - the width was short by 80 columns, and nothing
 * about the picture that *was* drawn looked wrong.
 */
#define LCD_W_MAX 320
#define LCD_W     320
#define LCD_H     240

/*
 * Memory access control: which corner is the origin, which way the rows run,
 * and - the bit that is not about geometry at all - whether the three colour
 * fields arrive red first or blue first.  0x40 is this board: mirrored in x,
 * and *no* BGR bit.  With the bit set, a fill sent as full red came out full
 * blue, which is the cheapest possible way to find out what a panel thinks
 * the pixel format is.
 */
#define LCD_MADCTL 0x40

static int s_bus = LCD_BUS, s_cs = LCD_CS, s_dc = LCD_DC, s_bl = LCD_BL;
static int s_w = LCD_W, s_h = LCD_H;

static const ag_io_api_t *io;

/* ---- the wire ---------------------------------------------------------- */

static ag_err_t cmd(uint8_t c)
{
    io->gpio_write(s_dc, 0);
    return io->spi_xfer(s_bus, s_cs, &c, NULL, 1);
}

static ag_err_t data(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    io->gpio_write(s_dc, 1);
    while (len > 0) {
        /* One transfer is bounded by the port's bounce buffer, and a row of
         * this panel is 640 bytes, so a row is one transfer and no row is
         * ever split. */
        const size_t chunk = (len > 512) ? 512 : len;
        const ag_err_t err = io->spi_xfer(s_bus, s_cs, p, NULL, chunk);
        if (err != AG_OK) {
            return err;
        }
        p += chunk;
        len -= chunk;
    }
    return AG_OK;
}

static ag_err_t cmd_data(uint8_t c, const void *buf, size_t len)
{
    const ag_err_t err = cmd(c);
    if (err != AG_OK || len == 0) {
        return err;
    }
    return data(buf, len);
}

/* ---- who is out there -------------------------------------------------- */

/*
 * RDDID (0xd3) answers with a dummy byte and then 00 93 41 on an ILI9341.
 * Worth asking before anything else: half the boards sold as this one have an
 * ST7789 behind the same connector, and the init sequence below would put that
 * panel into a state that looks exactly like a dead panel.
 */
static void identify(void)
{
    uint8_t id[4] = {0, 0, 0, 0};

    io->gpio_write(s_dc, 0);
    if (io->spi_xfer(s_bus, s_cs, (const uint8_t[]){0xd3}, NULL, 1) != AG_OK) {
        ag_print("spi_xfer failed: is [spi2] in BOARD.CFG?\n");
        return;
    }
    io->gpio_write(s_dc, 1);
    if (io->spi_xfer(s_bus, s_cs, NULL, id, sizeof(id)) != AG_OK) {
        ag_print("panel does not answer reads (MISO not wired?)\n");
        return;
    }

    ag_printf("RDDID: %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);
    if (id[1] == 0x00 && id[2] == 0x93 && id[3] == 0x41) {
        ag_print("  -> ILI9341\n");
    } else if (id[2] == 0x93 && id[3] == 0x41) {
        ag_print("  -> ILI9341 (no dummy byte)\n");
    } else {
        ag_print("  -> not an ILI9341, or reads do not work on this wiring\n");
    }
}

/* ---- bring it up ------------------------------------------------------- */

static void init_panel(uint8_t madctl)
{
    static const uint8_t pwctr1[] = {0x23};
    static const uint8_t pwctr2[] = {0x10};
    static const uint8_t vmctr1[] = {0x3e, 0x28};
    static const uint8_t vmctr2[] = {0x86};
    static const uint8_t pixfmt[] = {0x55}; /* 16 bits per pixel */
    static const uint8_t frmctr[] = {0x00, 0x18};
    static const uint8_t dfunctr[] = {0x08, 0x82, 0x27};

    (void)cmd(0x01); /* software reset - the panel may be mid-anything */
    ag_api()->time->delay_ms(150);

    (void)cmd_data(0xc0, pwctr1, sizeof(pwctr1));
    (void)cmd_data(0xc1, pwctr2, sizeof(pwctr2));
    (void)cmd_data(0xc5, vmctr1, sizeof(vmctr1));
    (void)cmd_data(0xc7, vmctr2, sizeof(vmctr2));
    (void)cmd_data(0x36, &madctl, 1);
    (void)cmd_data(0x3a, pixfmt, sizeof(pixfmt));
    (void)cmd_data(0xb1, frmctr, sizeof(frmctr));
    (void)cmd_data(0xb6, dfunctr, sizeof(dfunctr));

    /*
     * Undo whatever the last firmware left set.  A software reset is supposed
     * to cover this, and mostly does, but the three that survive it on some
     * parts are exactly the three that make a freshly written screen still
     * show the old one: partial mode (only a strip of memory is displayed),
     * inversion, and a vertical scroll offset (memory is displayed from a
     * different starting row, so a full write appears shifted with a band of
     * the old picture left over).
     */
    static const uint8_t vscrdef[6] = {0x00, 0x00, 0x01, 0x40, 0x00, 0x00};
    static const uint8_t vscrsadd[2] = {0x00, 0x00};

    (void)cmd(0x13); /* normal display: not partial, not idle */
    (void)cmd(0x20); /* inversion off */
    (void)cmd_data(0x33, vscrdef, sizeof(vscrdef));  /* whole panel scrolls  */
    (void)cmd_data(0x37, vscrsadd, sizeof(vscrsadd)); /* from row zero        */

    (void)cmd(0x11); /* out of sleep */
    ag_api()->time->delay_ms(120);
    (void)cmd(0x29); /* display on */
    ag_api()->time->delay_ms(20);
}

static void window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    const uint8_t ca[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0,
                           (uint8_t)(x1 >> 8), (uint8_t)x1};
    const uint8_t pa[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0,
                           (uint8_t)(y1 >> 8), (uint8_t)y1};
    (void)cmd_data(0x2a, ca, sizeof(ca));
    (void)cmd_data(0x2b, pa, sizeof(pa));
    (void)cmd(0x2c);
}

/* ---- something to look at ---------------------------------------------- */

/*
 * A row at a time.  This board has 232 KB of free memory and a screen of this
 * size in RGB565 is 150 KB of it in one piece, which the heap does not have -
 * so a full framebuffer is not the way a panel gets driven here, and a row
 * buffer of 640 bytes is.
 */
static uint16_t s_row[LCD_W_MAX];

/* Five by seven, uppercase and digits: enough to say what this is. */
#define GLYPH_FIRST ' '
#define GLYPH_LAST  '_'
static const uint8_t k_font[GLYPH_LAST - GLYPH_FIRST + 1][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /*   */ {0x00,0x00,0x5f,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */ {0x14,0x7f,0x14,0x7f,0x14}, /* # */
    {0x24,0x2a,0x7f,0x2a,0x12}, /* $ */ {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */ {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1c,0x22,0x41,0x00}, /* ( */ {0x00,0x41,0x22,0x1c,0x00}, /* ) */
    {0x14,0x08,0x3e,0x08,0x14}, /* * */ {0x08,0x08,0x3e,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */ {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */ {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3e,0x51,0x49,0x45,0x3e}, /* 0 */ {0x00,0x42,0x7f,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */ {0x21,0x41,0x45,0x4b,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7f,0x10}, /* 4 */ {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3c,0x4a,0x49,0x49,0x30}, /* 6 */ {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */ {0x06,0x49,0x49,0x29,0x1e}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */ {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */ {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */ {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3e}, /* @ */ {0x7e,0x11,0x11,0x11,0x7e}, /* A */
    {0x7f,0x49,0x49,0x49,0x36}, /* B */ {0x3e,0x41,0x41,0x41,0x22}, /* C */
    {0x7f,0x41,0x41,0x22,0x1c}, /* D */ {0x7f,0x49,0x49,0x49,0x41}, /* E */
    {0x7f,0x09,0x09,0x09,0x01}, /* F */ {0x3e,0x41,0x49,0x49,0x7a}, /* G */
    {0x7f,0x08,0x08,0x08,0x7f}, /* H */ {0x00,0x41,0x7f,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3f,0x01}, /* J */ {0x7f,0x08,0x14,0x22,0x41}, /* K */
    {0x7f,0x40,0x40,0x40,0x40}, /* L */ {0x7f,0x02,0x0c,0x02,0x7f}, /* M */
    {0x7f,0x04,0x08,0x10,0x7f}, /* N */ {0x3e,0x41,0x41,0x41,0x3e}, /* O */
    {0x7f,0x09,0x09,0x09,0x06}, /* P */ {0x3e,0x41,0x51,0x21,0x5e}, /* Q */
    {0x7f,0x09,0x19,0x29,0x46}, /* R */ {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7f,0x01,0x01}, /* T */ {0x3f,0x40,0x40,0x40,0x3f}, /* U */
    {0x1f,0x20,0x40,0x20,0x1f}, /* V */ {0x7f,0x20,0x18,0x20,0x7f}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */ {0x03,0x04,0x78,0x04,0x03}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */ {0x00,0x7f,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */ {0x00,0x41,0x41,0x7f,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */ {0x40,0x40,0x40,0x40,0x40}, /* _ */
};

static uint8_t glyph_col(char c, int col)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    if (c < GLYPH_FIRST || c > GLYPH_LAST) {
        c = '?';
    }
    return k_font[c - GLYPH_FIRST][col];
}

/*
 * The text is drawn into the row buffer as the rows go past, rather than into
 * a picture that is then sent: with no room for a framebuffer, the only place
 * a pixel exists is on its way out.
 */
static void draw_text_into_row(const char *text, int x, int y, int row,
                               int scale, uint16_t colour)
{
    const int h = 7 * scale;
    if (text == NULL || row < y || row >= y + h) {
        return;
    }
    const int gy = (row - y) / scale;

    for (int i = 0; text[i] != '\0'; i++) {
        const int gx0 = x + i * 6 * scale;
        for (int col = 0; col < 5; col++) {
            if ((glyph_col(text[i], col) & (1u << gy)) == 0) {
                continue;
            }
            for (int s = 0; s < scale; s++) {
                const int px = gx0 + col * scale + s;
                if (px >= 0 && px < s_w) {
                    s_row[px] = colour;
                }
            }
        }
    }
}

static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    const uint16_t v = (uint16_t)(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) |
                                  (b >> 3));
    /* The panel takes the high byte first. */
    return (uint16_t)((v >> 8) | (v << 8));
}

static void bars(void)
{
    const uint16_t k_bar[8] = {
        rgb(255, 255, 255), rgb(255, 255, 0), rgb(0, 255, 255),
        rgb(0, 255, 0),     rgb(255, 0, 255), rgb(255, 0, 0),
        rgb(0, 0, 255),     rgb(0, 0, 0),
    };
    const uint16_t black = rgb(0, 0, 0);
    const uint16_t white = rgb(255, 255, 255);
    const uint16_t amber = rgb(255, 176, 0);

    const int bars_h = s_h / 2;

    /* The whole panel, every row: whatever was in its memory before - the
     * firmware it was sold with, in the orientation that firmware chose -
     * is still being displayed everywhere this does not write. */
    window(0, 0, (uint16_t)(s_w - 1), (uint16_t)(s_h - 1));

    for (int y = 0; y < s_h; y++) {
        if (y < bars_h) {
            for (int x = 0; x < s_w; x++) {
                s_row[x] = k_bar[(x * 8) / s_w];
            }
        } else {
            for (int x = 0; x < s_w; x++) {
                s_row[x] = black;
            }
        }
        draw_text_into_row("ARGONOS", 10, bars_h + 20, y, 4, amber);
        draw_text_into_row("0.1.0 ON REAL SILICON", 10, bars_h + 60, y, 2, white);
        draw_text_into_row("ILI9341 SPI2 40 MHZ", 10, bars_h + 80, y, 2, white);
        draw_text_into_row("ESP32-2432S024", 10, bars_h + 100, y, 2, amber);
        (void)data(s_row, (size_t)s_w * sizeof(s_row[0]));
    }
}

/*
 * One colour, every pixel the window covers, and a count of what went out.
 *
 * The question this answers is not "does it draw" but "where do the pixels
 * actually land": whatever part of the glass is *not* this colour afterwards
 * is a part the writes are not reaching, and its shape says why - a band on
 * one edge is a size that is wrong, a scattered remainder is a stream that is
 * being dropped.
 */
static void fill(uint16_t colour)
{
    for (int x = 0; x < s_w; x++) {
        s_row[x] = colour;
    }
    window(0, 0, (uint16_t)(s_w - 1), (uint16_t)(s_h - 1));
    for (int y = 0; y < s_h; y++) {
        (void)data(s_row, (size_t)s_w * sizeof(s_row[0]));
    }
    ag_printf("filled %dx%d = %u pixels\n", s_w, s_h, (unsigned)(s_w * s_h));
}

/* ---- ---------------------------------------------------------------- */

static int hex_or_dec(const char *s, int fallback)
{
    if (s == NULL || *s == '\0') {
        return fallback;
    }
    int base = 10, v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    for (; *s != '\0'; s++) {
        int d;
        if (*s >= '0' && *s <= '9') {
            d = *s - '0';
        } else if (base == 16 && *s >= 'a' && *s <= 'f') {
            d = *s - 'a' + 10;
        } else if (base == 16 && *s >= 'A' && *s <= 'F') {
            d = *s - 'A' + 10;
        } else {
            return fallback;
        }
        v = v * base + d;
    }
    return v;
}

int ag_main(int argc, char **argv)
{
    io = ag_api()->io;
    if (io == NULL || !AG_HAS(io, spi_xfer)) {
        ag_print("this build has no direct hardware access\n");
        return 1;
    }

    const char *mode = (argc > 1) ? argv[1] : "bars";
    const uint8_t madctl =
        (uint8_t)((argc > 2) ? hex_or_dec(argv[2], LCD_MADCTL) : LCD_MADCTL);
    if (argc > 4) {
        s_bl = hex_or_dec(argv[4], LCD_BL);
    }
    if (argc > 6) {
        s_w = hex_or_dec(argv[5], LCD_W);
        s_h = hex_or_dec(argv[6], LCD_H);
        if (s_w < 16 || s_w > LCD_W_MAX || s_h < 16) {
            ag_print("width 16..320, height at least 16\n");
            return 1;
        }
    }

    /*
     * Data/command and the backlight are ours for as long as this runs; the
     * chip select belongs to the SPI layer, which takes it when a transfer
     * names it.  All three come back when this process ends, which is why an
     * experiment like this one is safe to repeat.
     */
    ag_err_t err = io->gpio_config(s_dc, AG_GPIO_OUT);
    if (err != AG_OK) {
        ag_printf("pin %d (dc): %s\n", s_dc, ag_strerror(err));
        return 1;
    }
    err = io->gpio_config(s_bl, AG_GPIO_OUT);
    if (err != AG_OK) {
        ag_printf("pin %d (backlight): %s\n", s_bl, ag_strerror(err));
        return 1;
    }
    io->gpio_write(s_bl, 1);

    identify();
    if (ag_strcmp(mode, "id") == 0) {
        return 0;
    }

    ag_printf("init: madctl 0x%02x, %dx%d, cs %d, dc %d, backlight %d\n",
              madctl, s_w, s_h, s_cs, s_dc, s_bl);
    init_panel(madctl);

    const uint32_t t0 = ag_api()->time->ms();
    if (ag_strcmp(mode, "fill") == 0) {
        fill(rgb(255, 0, 0));
    } else {
        bars();
    }
    const uint32_t took = ag_api()->time->ms() - t0;

    ag_printf("one full frame: %u ms (%u KB over SPI)\n", (unsigned)took,
              (unsigned)((s_w * s_h * 2) / 1024));

    /*
     * And now stay alive, holding the backlight.
     *
     * The panel keeps the picture in its own memory whatever this program
     * does, but the backlight is a pin, and a pin belongs to the process that
     * claimed it: ending gives it back, the driver stops driving it, and the
     * screen goes dark within a millisecond of the picture being finished.
     * The first run of this program drew a perfect frame that nobody could
     * see, and the symptom - a dark screen - is the same one a dead panel
     * gives.  That is the whole reason a display belongs to the system rather
     * than to an application.
     */
    const int hold = (argc > 3) ? hex_or_dec(argv[3], 30) : 30;
    ag_printf("holding the backlight for %d s (Ctrl+\\ to stop sooner)\n", hold);
    for (int i = 0; i < hold * 10; i++) {
        ag_api()->time->delay_ms(100);
    }
    ag_print("backlight released; the picture stays in the panel's memory\n");
    return 0;
}
