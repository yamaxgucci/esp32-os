/*
 * ArgonOS port: bare metal - time.
 *
 * A free-running counter and a way to read the CPU's cycle count.  The first is
 * a hardware timer; the second is usually a single instruction, and if this
 * machine has no such counter then say so by returning 0 rather than returning
 * microseconds again - a clock that quietly answers in the wrong unit is worse
 * than one that is absent, and this project has already paid for that lesson
 * (pitfall 33 in docs/05-status.md).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_TIME_H
#define ARGON_PORT_IMPL_TIME_H

#error "bare: no clock yet.  See argon/port/time.h for the contract."

#endif /* ARGON_PORT_IMPL_TIME_H */
