/*
 * ArgonOS - a board that hands its work to a neighbour and goes quiet.
 *
 * The fourth of the shapes a cluster is actually wanted for: one board with
 * the clock, the screen and the current, and beside it one that costs almost
 * nothing to leave switched on.  While there is work, the fast board does it.
 * When there has been none for a while, the work MOVES to the small board and
 * the fast one drops to its lowest rung; when work comes back, so does the
 * work that left.
 *
 * WHAT COUNTS AS NO WORK is the whole of this file's judgement, and it is not
 * the idle timer's question.  The power rungs measure time since somebody
 * touched the keyboard, which is right for dimming a screen and wrong here: a
 * board grinding a long calculation with nobody watching is busy, and dozing
 * it would be a fault.  So this measures the PROCESSES - nothing above a few
 * per mille of a core, for long enough that a gap between two pieces of work
 * is not mistaken for the end of them.
 *
 * Nothing is killed and nothing is forced.  A process that cannot move says
 * so - that is what ag_migrate_away asks - and a board still holding one of
 * those does not go quiet, because its work is still here.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NODESTANDBY_H
#define ARGON_NODESTANDBY_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start watching, reading [cluster] standby_after and standby_busy out of
 * SYSTEM.CFG.  Idempotent, and a no-op when standby_after is 0 - which is the
 * default, because a board that moves its work somewhere else without being
 * asked to would be a surprise on every board that is not in this shape.
 */
ag_err_t ag_standby_start(void);

/* Hand the work over now, and fetch it back now: what the timer would do,
 * said by a person.  Both answer -AG_ENODEV when no wire is bound. */
ag_err_t ag_standby_handover(void);
ag_err_t ag_standby_fetch(void);

/* Is our work sitting on the neighbour at the moment. */
bool ag_standby_handed_over(void);

/* Seconds of quiet before the handover, and the per-mille of a core below
 * which a process counts as doing nothing.  0 seconds is off. */
void ag_standby_settings(uint32_t *after_s, uint32_t *busy_permille);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_NODESTANDBY_H */
