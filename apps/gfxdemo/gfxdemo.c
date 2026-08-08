/*
 * ArgonOS - soft graphics smoke test.
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
    ag_gfx_fill_rect(24, 72, 96, 64, 0x0020A060u);
    ag_gfx_fill_rect(140, 72, 96, 64, 0x00E0C040u);
    (void)ag_gfx_text(24, 24, "ArgonOS soft gfx", 0x00FFFFFFu, 0x00C04020u);
    (void)ag_gfx_text(24, 160, "RGB565 framebuffer", 0x00E0E0E0u, 0x00102040u);

    ag_gfx_flush(0, 0, info.width, info.height);
    ag_gfx_swap();
    ag_gfx_release();

    ag_printf("ok\n");
    return 0;
}
