/*
 * ArgonOS - the smallest thing that puts pixels on a panel.
 *
 * GFXDEMO exercises everything the soft renderer has, at coordinates chosen
 * for a 640x400 surface.  That makes it a poor first test on a board whose
 * surface is 160x120: when it stops, the question "did the pixels reach the
 * glass" is tangled up with a dozen clipping paths it also just walked.
 *
 * This draws four rectangles and some text with a print between every step, so
 * a run that stops says where.  Nothing here needs a particular size.
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32-elf-gcc \
 *       --include sdk/include -o build/apps/GFXPIX.AXE apps/gfxpix/gfxpix.c
 *   run a:\gfxpix.axe            (any key leaves; `hold N` stays N seconds)
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <argon/keys.h>

AG_APP("GFXPIX", "1.0", "argon", AG_AXE_NEEDS_GFX);

int ag_main(int argc, char **argv)
{
    ag_gfxinfo_t info;
    uint32_t     hold_s = 0;

    /* Digits, by hand: the SDK has no atoi and one argument is not a reason
     * to want one. */
    if (argc >= 2 && argv[1] != NULL) {
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) {
            hold_s = hold_s * 10u + (uint32_t)(*p - '0');
        }
    }

    if (ag_api()->gfx == NULL) {
        ag_printf("no gfx in this build\n");
        return 1;
    }

    const ag_err_t err = ag_gfx_acquire(&info);
    if (err != AG_OK) {
        ag_printf("acquire: %s\n", ag_strerror(err));
        return 1;
    }
    ag_printf("1 acquired %ux%u stride=%u direct=%d\n", (unsigned)info.width,
              (unsigned)info.height, (unsigned)info.stride,
              info.direct ? 1 : 0);

    const uint16_t w = info.width;
    const uint16_t h = info.height;

    ag_gfx_clear(0x00000080u); /* navy, so an untouched panel is obvious */
    ag_printf("2 cleared\n");

    /* One corner each, so a mirrored or rotated surface shows up as such. */
    ag_gfx_fill_rect(0, 0, (uint16_t)(w / 4), (uint16_t)(h / 4), 0x00FF0000u);
    ag_gfx_fill_rect((int16_t)(w - w / 4), 0, (uint16_t)(w / 4),
                     (uint16_t)(h / 4), 0x0000FF00u);
    ag_gfx_fill_rect(0, (int16_t)(h - h / 4), (uint16_t)(w / 4),
                     (uint16_t)(h / 4), 0x00FFFF00u);
    ag_gfx_fill_rect((int16_t)(w - w / 4), (int16_t)(h - h / 4),
                     (uint16_t)(w / 4), (uint16_t)(h / 4), 0x00FFFFFFu);
    ag_printf("3 corners\n");

    (void)ag_gfx_text(4, (int16_t)(h / 2 - 8), "GFXPIX", 0x00FFFFFFu,
                      0x00000080u);
    ag_printf("4 text\n");

    ag_gfx_flush(0, 0, w, h);
    ag_printf("5 flushed\n");

    if (hold_s != 0) {
        ag_delay(hold_s * 1000u);
        ag_printf("6 held %u s\n", (unsigned)hold_s);
        ag_gfx_release();
        return 0;
    }

    for (;;) {
        ag_event_t ev;
        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_QUIT || ev.type == AG_EV_KEY_DOWN) {
                ag_gfx_release();
                return 0;
            }
        }
        ag_delay(30);
    }
}
