/*
 * ArgonOS - the attribute a kernel static carries for the [memory] section's
 * build-time pieces (argon/place.h): `kernel.static` and `dev.table`.
 *
 * Empty unless the build asked for PSRAM (CONFIG_ARGON_PLACE_*_PSRAM, written
 * from the board pack by tools/argon.ps1) and the port can do it; then the
 * linker puts the table in PSRAM.  Only for tables no interrupt, flash write
 * or DMA engine touches.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_CORE_PLACE_BSS_H
#define ARGON_CORE_PLACE_BSS_H

#include <argon/port/config.h>
#include <argon/port/place.h>

#if defined(CONFIG_ARGON_PLACE_KERNEL_STATIC_PSRAM) && CONFIG_ARGON_PLACE_KERNEL_STATIC_PSRAM
#define AG_KERNEL_STATIC AG_PORT_SLOW_BSS
#else
#define AG_KERNEL_STATIC
#endif

#if defined(CONFIG_ARGON_PLACE_DEV_TABLE_PSRAM) && CONFIG_ARGON_PLACE_DEV_TABLE_PSRAM
#define AG_DEV_TABLE_BSS AG_PORT_SLOW_BSS
#else
#define AG_DEV_TABLE_BSS
#endif

#endif /* ARGON_CORE_PLACE_BSS_H */
