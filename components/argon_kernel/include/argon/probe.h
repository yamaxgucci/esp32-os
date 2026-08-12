/*
 * ArgonOS - matching loadable .SYS drivers to chips on a bus.
 *
 * The table lives in SYSTEM.CFG, not in the image header: config is the source
 * of truth for what is wired to this board (architecture §8.4).  Each line names
 * a bus, an address, an optional ID register/value, and the module to load when
 * that address answers.
 *
 *     [modules]
 *     device = t:\echo.sys
 *     probe  = 0:0x76:a:\drv\bme280.sys
 *     probe  = 0:0x76:0xD0=0x60:a:\drv\bme280.sys
 *
 * SPI panels are deliberately absent: they do not ACK a scan, so their type and
 * pins stay in BOARD.CFG.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PROBE_H
#define ARGON_PROBE_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AG_PROBE_MAX 16

typedef struct {
    ag_probe_hint_t hint;
    char            path[AG_PATH_MAX];
} ag_probe_entry_t;

/*
 * Parses one `modules.probe` value.  Forms:
 *   <bus>:<addr>:<path>
 *   <bus>:<addr>:<idreg>=<idval>:<path>
 *
 * Numbers accept a leading 0x.  The path may contain colons (t:\drv\x.sys).
 * Returns -AG_EINVAL when the line is not that shape.
 */
ag_err_t ag_probe_parse(const char *line, ag_probe_entry_t *out);

/*
 * Walks modules.probe in SYSTEM.CFG.  For each entry: skip if that path is
 * already loaded; ask the bus; optionally read the ID register; on a match,
 * load the module with a probe hint.  Missing chips are not errors.
 *
 * `loaded` / `missed` may be NULL.  missed counts entries whose chip was absent
 * or whose ID did not match; parse and load failures are logged separately.
 */
ag_err_t ag_probe_run(uint32_t *loaded, uint32_t *missed);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_PROBE_H */
