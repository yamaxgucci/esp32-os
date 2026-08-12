/*
 * ArgonOS - platform detection (kernel private).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PLATFORM_H
#define ARGON_PLATFORM_H

#include <argon/abi.h>

typedef enum {
    AG_PROFILE_NANO = 0, /* no application loading                        */
    AG_PROFILE_LITE,     /* applications in internal SRAM only            */
    AG_PROFILE_FULL,     /* applications in executable PSRAM              */
} ag_profile_t;

typedef struct {
    ag_profile_t profile;
    size_t       psram_total;
    size_t       sram_total;
    size_t       sram_free_at_boot;
    bool         psram_present;
    bool         psram_executable; /* MMU can map PSRAM into the I-bus     */
    uint8_t      chip_revision;
} ag_platform_t;

ag_err_t ag_platform_init(void);
const ag_platform_t *ag_platform(void);
const char *ag_profile_name(ag_profile_t p);

/* Fills the public sysinfo structure; board name is patched in later. */
void ag_platform_fill_sysinfo(ag_sysinfo_t *out);

#endif /* ARGON_PLATFORM_H */
