/*
 * ArgonOS port contract - where a static lives.
 *
 *   AG_PORT_SLOW_BSS   an attribute for a zero-initialised static: put it in
 *                      external memory (PSRAM) where the port can, and leave
 *                      it in ordinary memory where it cannot.
 *
 * For tables that only tasks read - never an interrupt, never code that runs
 * while a flash write has the cache off, never a DMA engine.  Internal SRAM is
 * the scarce memory on every chip this system runs on, and a table held from
 * boot for a feature the board may never use is paid for by every program
 * (docs/13-memory.md).
 *
 * A separate header from mem.h on purpose: sources that need no allocator -
 * the VFS, the device table - include this one, and a port with no allocator
 * yet (bare) can still build them.  A port that does not define the attribute
 * gets the empty one below, which is always correct.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_PLACE_H
#define ARGON_PORT_PLACE_H

#include <stdbool.h>

#include <argon/port/impl/place.h>

#ifndef AG_PORT_SLOW_BSS
#define AG_PORT_SLOW_BSS
#endif

/*
 *   AG_PORT_CODE_ARENA_BSS   the attribute that puts the loader's link-time
 *                      code arena in memory both writable and executable.
 *                      .iram.bss unless the port says otherwise: that is where
 *                      ESP-IDF keeps zeroed memory the instruction bus reaches.
 */
#ifndef AG_PORT_CODE_ARENA_BSS
#define AG_PORT_CODE_ARENA_BSS __attribute__((section(".iram.bss.ag_app_arena")))
#endif

/*
 * What the linker did with a build-time piece of the [memory] section
 * (argon/place.h): "iram", "flash", "rom" or "psram", or NULL for a piece this
 * port does not build at all.  Read from the build's own configuration, so it
 * is the truth about this image, not about any file.
 */
const char *ag_port_place_built(const char *piece);

/* Is this pointer into external memory (PSRAM)? */
bool ag_port_ptr_is_slow(const void *p);

#endif /* ARGON_PORT_PLACE_H */
