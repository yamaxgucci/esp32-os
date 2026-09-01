/*
 * ArgonOS - the `rmt` command: light a WS2812 / NeoPixel LED chain.
 *
 *   rmt ws2812 <gpio> <RRGGBB> [RRGGBB ...]
 *
 * One RRGGBB hex triplet per LED, ordinary red-green-blue as written; the wire
 * order (green-red-blue) and the pulse timing are the port's business.  A board
 * with an on-board addressable LED (the S3-Zero's is on GPIO 21) needs no extra
 * parts:  rmt ws2812 21 200000  is a dim green.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/config.h>

#if defined(CONFIG_ARGON_RMT) && CONFIG_ARGON_RMT

#include <stdlib.h>

#include <argon/console.h>
#include <argon/path.h>

#include <argon/port/rmt.h>

#define RMT_MAX_LEDS 64

/* Parse exactly six hex digits (optional 0x) into r,g,b; -1 on a bad triplet. */
static int parse_rgb(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 16);
    if (end == s || *end != '\0' || v > 0xFFFFFFul) {
        return -1;
    }
    *r = (uint8_t)((v >> 16) & 0xFF);
    *g = (uint8_t)((v >> 8) & 0xFF);
    *b = (uint8_t)(v & 0xFF);
    return 0;
}

int ag_cmd_rmt(int argc, char **argv)
{
    if (argc < 4 || ag_path_icmp(argv[1], "ws2812") != 0) {
        ag_console_puts("usage: rmt ws2812 <gpio> <RRGGBB> [RRGGBB ...]\n");
        return 1;
    }
    const int pin = atoi(argv[2]);
    const int leds = argc - 3;
    if (leds > RMT_MAX_LEDS) {
        ag_console_printf("at most %d LEDs at once\n", RMT_MAX_LEDS);
        return 1;
    }

    uint8_t grb[RMT_MAX_LEDS * 3];
    for (int i = 0; i < leds; i++) {
        uint8_t r, g, b;
        if (parse_rgb(argv[3 + i], &r, &g, &b) != 0) {
            ag_console_printf("bad colour '%s' (want six hex digits)\n",
                              argv[3 + i]);
            return 1;
        }
        grb[i * 3 + 0] = g; /* WS2812 wants green, then red, then blue */
        grb[i * 3 + 1] = r;
        grb[i * 3 + 2] = b;
    }

    if (ag_port_rmt_ws2812(pin, grb, leds * 3) != 0) {
        ag_console_puts("RMT transmit failed (pin in use, or no channel?)\n");
        return 1;
    }
    ag_console_printf("lit %d LED%s on gpio %d\n", leds, leds == 1 ? "" : "s",
                      pin);
    return 0;
}

#endif /* CONFIG_ARGON_RMT */
