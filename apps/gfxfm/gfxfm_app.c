/*
 * ArgonOS GFXFM - graphical file manager image header.
 *
 * Same panels/ops as apps/fm, drawn on soft gfx (see fm_ui_gfx.c).
 *
 *   python tools/mkaxe.py --arch xtensa --gcc xtensa-esp32s3-elf-gcc \
 *       --include sdk/include -D FM_GFX_BUILD \
 *       -o build/apps/GFXFM.AXE \
 *       apps/gfxfm/gfxfm_app.c apps/gfxfm/fm_ui_gfx.c \
 *       apps/fm/fm.c apps/fm/fmops.c
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

/* Heap: two panels × 512 entries (same budget as text fm). */
AG_APP_SIZED("GFXFM", "1.0", "argon", AG_AXE_NEEDS_GFX, 0, 512 * 1024);
