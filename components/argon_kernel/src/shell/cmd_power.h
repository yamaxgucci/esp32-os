/*
 * ArgonOS - the `power` command (kernel private).
 *
 * Turns words into one call into src/core/powerctl.c and prints what the
 * machine and the applications on it had to say about it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_CMD_POWER_H
#define ARGON_CMD_POWER_H

int ag_cmd_power(int argc, char **argv);

#endif /* ARGON_CMD_POWER_H */
