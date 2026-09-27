/*
 * ArgonOS - the other node's devices, in this node's registry.
 *
 * This is the piece the whole cluster was asked for: Wi-Fi on one board, sound
 * on another, and a system in which they are simply there.  A device on the far
 * node is registered HERE, under `<node>.<device>`, with the ordinary device
 * vtable - so `dev` lists it, an application opens it by name, and nothing
 * above the registry learns that a wire is involved.
 *
 * The qualified name is the node's, not an index.  `n2.pcm0` and not `pcm1`:
 * a number assigned in order of arrival means a config file written yesterday
 * points at a different board today, and that is the one failure nobody can
 * debug from the symptom.
 *
 * This does NOT make the far device as good as a local one, and is not meant
 * to.  The placement rule stands: a process should go to its data, and a
 * program that reaches across the wire for every sample is paying a round trip
 * for each of them.  This is the escape hatch the rule needs in order to be
 * advice rather than a prohibition - a rule that forbids gets worked around.
 *
 * Every call here is a QUESTION that waits for its answer, so all of it rides
 * the reliable class.  The link's stream class exists for sound and is
 * deliberately allowed to drop frames; pouring samples at a far converter
 * belongs there, and it is a different mechanism from this one, not a faster
 * setting of it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NODERDEV_H
#define ARGON_NODERDEV_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Answer the other node's device calls against this node's registry. */
ag_err_t ag_noderdev_serve(void);

/*
 * Ask the far node what it has and put proxies for it in this registry, named
 * `<peer>.<device>`.  Needs the peer's name, so it needs the hello to have been
 * answered; returns -AG_EAGAIN when it has not.  Returns how many appeared.
 */
int32_t ag_noderdev_attach(uint32_t timeout_ms);
/* The same for the neighbour on one wire; the others' devices stay. */
int32_t ag_noderdev_attach_on(int32_t wire, uint32_t timeout_ms);

/* Take them all out again, every neighbour's.  Open handles are revoked, not
 * waited for. */
uint32_t ag_noderdev_detach(void);

/*
 * Docking: from ag_noderdev_auto(true) on - which the boot-time bind says -
 * a neighbour's devices attach by themselves whenever it names itself on a
 * wire (ag_noderdev_peer_named, from the hello, on the reader).  A wire bound
 * by hand still waits for `link devs`.
 */
void ag_noderdev_auto(bool on);

/*
 * The wire to the neighbour best placed for a program with these .AXE flags,
 * or -1 for "no preference": for one that needs a screen, the neighbour whose
 * screen is the one this board draws on - a panel before a framebuffer in
 * memory.
 */
int32_t ag_noderdev_wire_for(uint32_t axe_flags);
/* `again`: the neighbour started over (it asked), so what it lent before is
 * gone even if its devices are here; otherwise only a wire with none yet. */
void ag_noderdev_peer_named(int32_t wire, bool again);
/* A node beyond a neighbour, heard of through wire `via` (the cluster's list
 * of far nodes): its devices too, addressed to it, the neighbour passing them
 * on. */
void ag_noderdev_far_named(int32_t via, const char *name, uint8_t starts);

/* What was poured at this board and what came of it, since it started: the
 * pour task's own report is said only when a pour pauses, and sound that
 * never pauses never says it (`link` prints these). */
void ag_noderdev_pour_counts(uint32_t *periods, uint32_t *lost,
                             uint32_t *full, uint32_t *paced);

/* `link report on|off`: the screen's second-by-second journal lines. */
void ag_noderdev_report(bool on);
/* Times the reader waited for room for the picture; periods of sound that
 * came more than 30 ms after the one before. */
void ag_noderdev_wait_counts(uint32_t *reader_waits, uint32_t *late);
/* Pieces of a picture not drawn here: no device for their handle, nothing
 * drawn (the last one's op and length), and the drawing's faults. */
void ag_noderdev_draw_misses(uint32_t *nodev, uint32_t *nopx, uint32_t *op,
                             uint32_t *len, uint32_t *faults);
/* A neighbour's picture here since the board started: passes drawn, pieces
 * not drawn, the longest wait between two passes and between two periods of
 * sound, and the fullest the sound queue got, of how many. */
void ag_noderdev_screen_counts(uint32_t *passes, uint32_t *lost,
                               uint32_t *gap_ms, uint32_t *snd_gap_ms,
                               uint32_t *snd_q_max, uint32_t *snd_q_cap);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_NODERDEV_H */
