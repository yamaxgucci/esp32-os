/*
 * ArgonOS port: bare metal - the machine itself.
 *
 * Small and mostly mechanical: how many cores, how fast, how much external RAM,
 * why did we restart, and how to restart on purpose.
 *
 * The one that needs thought is AG_PORT_NOINIT.  It marks memory that survives
 * a warm reset without being zeroed at startup, and the boot counter lives
 * there: a board that reboots in a loop has to notice that it is doing so and
 * come up in recovery, and it cannot ask a filesystem it may be failing to
 * mount.  If this machine has no such memory, define AG_PORT_NOINIT as nothing
 * and accept that repeated crashes are not counted - that loses recovery mode
 * and nothing else.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_SYS_H
#define ARGON_PORT_IMPL_SYS_H

#error "bare: nothing known about this machine.  See argon/port/sys.h."

#endif /* ARGON_PORT_IMPL_SYS_H */
