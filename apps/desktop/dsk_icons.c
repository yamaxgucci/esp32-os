/*
 * ArgonOS DESKTOP - the icon pictures.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dsk_icons.h"

/*
 * Bit 0 is the leftmost pixel, as in the fonts, and the tile is anchored to
 * the screen rather than to whatever piece is being repainted - see
 * dsk_pattern() in dsk_paint.c for why that matters.
 */
static const uint8_t k_patterns[DSK_PATTERNS][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0},                               /* plain      */
    {0x55, 0x00, 0xAA, 0x00, 0x55, 0x00, 0xAA, 0x00},       /* fine dots  */
    {0x88, 0x00, 0x22, 0x00, 0x88, 0x00, 0x22, 0x00},       /* wide dots  */
    {0xFF, 0x88, 0x88, 0x88, 0xFF, 0x88, 0x88, 0x88},       /* bricks     */
    {0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44, 0x88},       /* weave      */
};

static const char *const k_pattern_names[DSK_PATTERNS] = {
    "plain", "fine dots", "wide dots", "bricks", "weave",
};

const uint8_t *dsk_pattern_rows(int which)
{
    if (which <= 0 || which >= DSK_PATTERNS) {
        return NULL; /* plain: the caller fills and draws no tile */
    }
    return k_patterns[which];
}

const char *dsk_pattern_name(int which)
{
    if (which < 0 || which >= DSK_PATTERNS) {
        return k_pattern_names[0];
    }
    return k_pattern_names[which];
}

#include "dsk_paint.h"

/* The sixteen VGA colours, in their own order: 0 black .. F white. */
static const uint32_t k_pal[16] = {
    DSK_BLACK, DSK_NAVY,  DSK_GREEN,   DSK_TEAL,  DSK_MAROON, DSK_PURPLE,
    DSK_OLIVE, DSK_LGRAY, DSK_DGRAY,   DSK_BLUE,  DSK_LIME,   DSK_CYAN,
    DSK_RED,   DSK_MAGENTA, DSK_YELLOW, DSK_WHITE,
};

uint32_t dsk_icon_colour(int index)
{
    return k_pal[(index < 0 ? 0 : index) & 15];
}

/* '.' is nothing; 0..F index the palette above. */
static const char *const k_art[DSK_ICON_COUNT][DSK_ICON_H] = {
    /* DRIVE - a stack with a light on the front */
    {"................", "................", "..000000000000..",
     "..0FFFFFFFFFF0..", "..0F77777777F0..", "..0F77777777F0..",
     "..0FFFFFFFFFF0..", "..000000000000..", "..0FFFFFFFFFF0..",
     "..0F777777C7F0..", "..0F77777777F0..", "..0FFFFFFFFFF0..",
     "..000000000000..", "................", "................",
     "................"},
    /* FLOPPY - the shutter at the top, the label at the bottom */
    {"................", ".00000000000000.", ".07777777777770.",
     ".07000000000070.", ".07077777777070.", ".07077777777070.",
     ".07000000000070.", ".07777777777770.", ".077FFFFFFFF770.",
     ".077F888888F770.", ".077F888888F770.", ".077FFFFFFFF770.",
     ".07777777777770.", ".00000000000000.", "................",
     "................"},
    /* HOST - a folder with a plug on it: the development machine's share */
    {"................", "................", "..6666..........",
     "..6EE666666666..", "..6EEEEEEEEEE6..", "..6EEEEEEEEEE6..",
     "..6EE1111111E6..", "..6EE1999991E6..", "..6EE1111111E6..",
     "..6EEEE111EEE6..", "..6EEE11111EE6..", "..666666666666..",
     "................", "................", "................",
     "................"},
    /* FOLDER */
    {"................", "................", "..6666..........",
     "..6EE666666666..", "..6EEEEEEEEEE6..", "..6EEEEEEEEEE6..",
     "..6EEEEEEEEEE6..", "..6EEEEEEEEEE6..", "..6EEEEEEEEEE6..",
     "..6EEEEEEEEEE6..", "..6EEEEEEEEEE6..", "..666666666666..",
     "................", "................", "................",
     "................"},
    /* UP - the folder with an arrow out of it */
    {"................", "................", "..6666..........",
     "..6EE666666666..", "..6EEEE00EEEE6..", "..6EEE0000EEE6..",
     "..6EE000000EE6..", "..6EEEE00EEEE6..", "..6EEEE00EEEE6..",
     "..6EEEE00EEEE6..", "..6EEEEEEEEEE6..", "..666666666666..",
     "................", "................", "................",
     "................"},
    /* PROGRAM - a little window, which is what a program looks like here */
    {"................", "..8888888888888.", "..8FFFFFFFFFFF8.",
     "..811111111FF8..", "..811111111FF8..", "..8FFFFFFFFFFF8.",
     "..8F777777777F8.", "..8F7FFFFFFF7F8.", "..8F7FFFFFFF7F8.",
     "..8F7FFFFFFF7F8.", "..8F777777777F8.", "..8FFFFFFFFFFF8.",
     "..8888888888888.", "................", "................",
     "................"},
    /* DRIVER - a chip, because a .SYS is hardware's half of the system */
    {"................", "....8.8.8.8.....", "..000000000000..",
     "..0AAAAAAAAAA0..", "8.0A00000000A0.8", "..0A0AAAAAAA0A0.",
     "8.0A0A0000A0A0.8", "..0A0A0AA0A0A0..", "8.0A0A0AA0A0A0.8",
     "..0A0A0000A0A0..", "8.0A0AAAAAAA0.8.", "..0AAAAAAAAAA0..",
     "..000000000000..", "....8.8.8.8.....", "................",
     "................"},
    /* TEXT - a page with lines on it */
    {"................", "..000000000000..", "..0FFFFFFFFFF0..",
     "..0F88888888F0..", "..0FFFFFFFFFF0..", "..0F88888888F0..",
     "..0FFFFFFFFFF0..", "..0F88888888F0..", "..0FFFFFFFFFF0..",
     "..0F88888888F0..", "..0FFFFFFFFFF0..", "..0F8888FFFFF0..",
     "..0FFFFFFFFFF0..", "..000000000000..", "................",
     "................"},
    /* FILE - the same page, with a question on it */
    {"................", "..000000000000..", "..0FFFFFFFFFF0..",
     "..0FFF8888FFF0..", "..0FF88FF88FF0..", "..0FFFFFFF88F0..",
     "..0FFFFFF88FF0..", "..0FFFFF88FFF0..", "..0FFFF88FFFF0..",
     "..0FFFF88FFFF0..", "..0FFFFFFFFFF0..", "..0FFFF88FFFF0..",
     "..0FFFFFFFFFF0..", "..000000000000..", "................",
     "................"},
};

static int nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1; /* '.' and anything else: not there */
}

static uint16_t to565(uint32_t rgb)
{
    const uint32_t r = (rgb >> 16) & 0xFFu;
    const uint32_t g = (rgb >> 8) & 0xFFu;
    const uint32_t b = rgb & 0xFFu;
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/* Doubled is the largest anybody asks for: 32x32. */
static uint16_t s_scratch[32 * 32];

void dsk_icon_draw(dsk_icon_t id, int16_t x, int16_t y, int scale, uint32_t bg)
{
    if (id >= DSK_ICON_COUNT) {
        id = DSK_ICON_FILE;
    }
    if (scale < 1) {
        scale = 1;
    }
    if (scale > 2) {
        scale = 2;
    }
    const int        w = DSK_ICON_W * scale;
    const int        h = DSK_ICON_H * scale;
    const uint16_t   back = to565(bg);
    const bool       keep = (bg == DSK_TRANSPARENT);
    const dsk_rect_t box = dsk_rect(x, y, (int16_t)w, (int16_t)h);

    if (keep) {
        /*
         * What is behind the icon, so that its transparent pixels stay
         * behind it.  Every icon here is a shape on a background it does
         * not own - the desk's pattern, or the white of a list - and
         * composing against a flat colour was a rectangle of that colour
         * around every icon.
         */
        dsk_paint()->read(box, s_scratch);
    }

    for (int row = 0; row < DSK_ICON_H; row++) {
        const char *art = k_art[id][row];
        for (int col = 0; col < DSK_ICON_W; col++) {
            const int n = nibble(art[col]);
            if (n < 0 && keep) {
                continue; /* leave what was read there */
            }
            const uint16_t px = (n < 0) ? back : to565(k_pal[n]);
            for (int sy = 0; sy < scale; sy++) {
                uint16_t *out = &s_scratch[(row * scale + sy) * w + col * scale];
                for (int sx = 0; sx < scale; sx++) {
                    out[sx] = px;
                }
            }
        }
    }
    dsk_paint()->blit(box, s_scratch, (uint32_t)w);
}

/* ---- which icon a name deserves ---------------------------------------- */

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = 0, m = 0;
    while (s[n] != '\0') {
        n++;
    }
    while (suffix[m] != '\0') {
        m++;
    }
    if (m > n) {
        return false;
    }
    for (size_t i = 0; i < m; i++) {
        char a = s[n - m + i];
        char b = suffix[i];
        if (a >= 'a' && a <= 'z') {
            a = (char)(a - 32);
        }
        if (b >= 'a' && b <= 'z') {
            b = (char)(b - 32);
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

dsk_icon_t dsk_icon_for(const char *name)
{
    if (name == NULL) {
        return DSK_ICON_FILE;
    }
    if (ends_with(name, ".AXE")) {
        return DSK_ICON_PROGRAM;
    }
    if (ends_with(name, ".SYS")) {
        return DSK_ICON_DRIVER;
    }
    if (ends_with(name, ".TXT") || ends_with(name, ".CFG") ||
        ends_with(name, ".MD") || ends_with(name, ".C") ||
        ends_with(name, ".H") || ends_with(name, ".BAT") ||
        ends_with(name, ".CMD") || ends_with(name, ".LOG") ||
        ends_with(name, ".INI")) {
        return DSK_ICON_TEXT;
    }
    return DSK_ICON_FILE;
}
