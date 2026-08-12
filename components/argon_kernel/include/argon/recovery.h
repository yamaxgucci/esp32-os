/*
 * ArgonOS - safe boot / recovery profile.
 *
 * After a streak of unclean resets (or an explicit marker), the kernel skips
 * loadable modules and AUTOEXEC so a bad DEVICE= cannot brick the board.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_RECOVERY_H
#define ARGON_RECOVERY_H

#include <stdbool.h>

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AG_BOOT_SAFE_PATH "/sys/boot.safe"

/*
 * Decide whether this boot is recovery.  Call once after storage + config,
 * before the modules stage.  Unclean resets bump a counter; power-on and a
 * deliberate reboot clear the streak when the shell later comes up cleanly.
 */
void ag_boot_recovery_begin(void);

/* True when modules and AUTOEXEC must be skipped this boot. */
bool ag_boot_in_recovery(void);

/* Why recovery is active (for the banner); "" when not in recovery. */
const char *ag_boot_recovery_reason(void);

/* Call when the interactive shell is up — clears the unclean-reset streak. */
void ag_boot_recovery_shell_ok(void);

/* Create / remove the sticky /sys/boot.safe marker. */
ag_err_t ag_boot_recovery_set_marker(bool on);

bool ag_boot_recovery_marker_present(void);

/* Unclean-reset streak counter (0 after a clean shell arrival). */
unsigned ag_boot_recovery_attempts(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_RECOVERY_H */
