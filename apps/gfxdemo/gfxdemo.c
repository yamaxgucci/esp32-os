/*
 * ArgonOS - soft graphics smoke test (rects, text, draw primitives).
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -o GFXDEMO.AXE apps/gfxdemo/gfxdemo.c
 *   run t:\gfxdemo.axe
 *   gfxdump t:\shot.ppm
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

AG_APP("GFXDEMO", "1.0", "argon", AG_AXE_NEEDS_GFX);

int ag_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (ag_api()->gfx == NULL) {
        ag_printf("no gfx\n");
        return 1;
    }

    ag_gfxinfo_t info;
    const ag_err_t err = ag_gfx_acquire(&info);
    if (err != AG_OK) {
        ag_printf("acquire failed: %s\n", ag_strerror(err));
        return 1;
    }

    ag_printf("gfx %ux%u fmt=%u stride=%u db=%d direct=%d\n",
              (unsigned)info.width, (unsigned)info.height, (unsigned)info.fmt,
              (unsigned)info.stride, info.double_buf ? 1 : 0,
              info.direct ? 1 : 0);

    ag_gfx_clear(0x00102040u);
    ag_gfx_fill_rect(16, 16, (uint16_t)(info.width - 32), 40, 0x00C04020u);
    (void)ag_gfx_text(24, 24, "ArgonOS soft gfx", 0x00FFFFFFu, 0x00C04020u);

    ag_gfx_line(24, 80, (int16_t)(info.width - 24), 80, 0x00E0E0E0u);
    ag_gfx_circle(120, 200, 48, 0x0020A0FFu);
    ag_gfx_fill_circle(260, 200, 40, 0x00E0C040u);

    ag_gfx_poly_begin();
    (void)ag_gfx_poly_vertex(400, 140);
    (void)ag_gfx_poly_vertex(480, 260);
    (void)ag_gfx_poly_vertex(320, 260);
    ag_gfx_poly_fill(0x0020A060u);

    ag_gfx_poly_begin();
    (void)ag_gfx_poly_vertex(520, 120);
    (void)ag_gfx_poly_vertex(600, 160);
    (void)ag_gfx_poly_vertex(560, 240);
    (void)ag_gfx_poly_vertex(480, 200);
    ag_gfx_poly_stroke(0x00FF80FFu);

    ag_gfx_pixel(40, 300, 0x00FFFFFFu);
    (void)ag_gfx_text(24, 320, "pixel/line/circle/poly", 0x00E0E0E0u,
                      0x00102040u);

    ag_gfx_flush(0, 0, info.width, info.height);
    ag_gfx_swap();
    ag_gfx_release();

    ag_printf("ok\n");
    return 0;
}
