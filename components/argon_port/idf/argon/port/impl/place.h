/*
 * ArgonOS port: ESP-IDF - where a static lives.
 *
 * A kernel table in PSRAM (8 Oct 2026).  The kernel's own statics were 39 KB
 * of internal SRAM on the S3-Zero, every one of them held from boot whether
 * the feature was used or not, on a board with 2 MB of PSRAM beside it.  IDF
 * places .ext_ram.bss in PSRAM and zeroes it before app_main when
 * CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is set; without it the attribute
 * is nothing and the table stays in internal memory, as before.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_PLACE_H
#define ARGON_PORT_IMPL_PLACE_H

#include "sdkconfig.h"

#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#include "esp_attr.h"
#define AG_PORT_SLOW_BSS EXT_RAM_BSS_ATTR
#endif

#endif /* ARGON_PORT_IMPL_PLACE_H */
