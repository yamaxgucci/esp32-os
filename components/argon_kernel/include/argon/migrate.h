/*
 * ArgonOS - moving a running program to another board.
 *
 * MIGRATION IS CONSENT, NOT TRANSPARENCY, and that was decided before any of
 * this was written.  There is no memory shared between two boards, so a state
 * taken from a program without its knowledge could not be put back faithfully:
 * a pointer means nothing over there, an open handle names a different object,
 * and a system that pretended otherwise would produce programs that work until
 * one day they do not.  So a program PROMISES it can be moved
 * (AG_AXE_CAN_MIGRATE), is ASKED, decides where in its own work it is safe to
 * agree, hands over exactly what it wants carried, and reopens its own files
 * and devices on the far side.
 *
 * That last part is the operator's own ruling, and it is what makes level 1
 * small: if a program says it can migrate, it can reopen what it needs.
 * Nothing here tries to move a handle.
 *
 * WHAT TRAVELS: the bytes the program hands to migrate_save, and nothing else.
 * Not the arena, not the handles, not the screen, not its threads.  That is a
 * floor chosen on arithmetic rather than a first draft - four megabytes of
 * arena at five megabytes a second is 0.8 s of a frozen program, so a level
 * that moved everything would be a level nobody would use.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_MIGRATE_H
#define ARGON_MIGRATE_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Ask `pid` to move to the node at the other end of the link.
 *
 * Refuses, before disturbing anything: -AG_ENOENT for no such process,
 * -AG_ENOTSUP for one that never promised (no AG_AXE_CAN_MIGRATE), -AG_ENODEV
 * with no link bound, -AG_EBUSY while another migration is in flight.
 *
 * Then it asks, waits for the program to agree by ending, and carries whatever
 * it left to the far node, which starts the same image with that state
 * attached.  A program that ends WITHOUT saving has declined, and declining is
 * a legitimate answer: -AG_EAGAIN, and it stays where it was... except that it
 * has already ended, which is why `migrate` says so plainly rather than
 * pretending nothing happened.
 *
 * `timeout_ms` bounds the waiting for it to agree.
 */
ag_err_t ag_migrate_away(ag_pid_t pid, uint32_t timeout_ms);

/* What the syscall table forwards here. */
bool     ag_migrate_wanted(void);
ag_err_t ag_migrate_save(const void *state, uint32_t len);
int32_t  ag_migrate_restore(void *out, uint32_t cap);

/*
 * Told by the process layer: this pid is being asked to move, or is no longer.
 * Between the two, interrupted() means "move" for that process.
 */
void ag_migrate_asking(ag_pid_t pid);
void ag_migrate_done(void);

/*
 * The state a starting process should find, set by whoever started it with one
 * - the far node's end of a migration.  Cleared when it is taken.
 */
void ag_migrate_arriving(const void *state, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_MIGRATE_H */
