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

/*
 * The default arena, not a named size.
 *
 * A named size is *required* - the loader refuses the application when it
 * cannot be had - and half a megabyte cannot be had on a board with 320 KB of
 * SRAM in total.  What that looked like was GFXFM starting and returning
 * instantly with nothing printed, which is a poor way to say "there is not
 * enough memory".
 *
 * The panels size themselves to what the arena will give (fm_read_panel halves
 * its entry count until it fits), so asking for the default is not a compromise
 * here: it is the thing that lets one image run on both machines.
 */
AG_APP("GFXFM", "1.0", "argon", AG_AXE_NEEDS_GFX);
