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

/*
 * The code arena on the ESP32-P4 below revision 3.
 *
 * IDF splits L2 memory in two there: sram_low, 179 KB holding the image's own
 * code, data and .iram.bss, and sram_high, 384 KB taking the .bss that does not
 * fit and then the heap.  A 192 KB arena in .iram.bss does not fit sram_low
 * beside the kernel, and the link fails.  As ordinary .bss it lands in
 * sram_high, which is executable as well with CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT
 * off: the PMP then gives the whole of L2 memory one read-write-execute entry
 * (cpu_region_protect.c).  The linker calls the region RW; the processor does
 * not ask the linker.
 */
#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_ESP32P4_SELECTS_REV_LESS_V3
#if CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT
#error "the P4 code arena needs CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=n"
#endif
#define AG_PORT_CODE_ARENA_BSS __attribute__((section(".bss.ag_app_arena")))
#endif

#endif /* ARGON_PORT_IMPL_PLACE_H */
