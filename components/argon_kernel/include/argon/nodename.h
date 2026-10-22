/*
 * ArgonOS - what this node is called, and what the one next to it is called.
 *
 * A cluster needs names before it needs anything else that has to be addressed:
 * `n2.pcm0` is meaningless without them, and so is a `ps` listing that spans
 * boards.  Two nodes can get by on "here" and "the other one"; a third makes
 * that ambiguous, and addressing built without names has to be built twice.
 *
 * A name is NOT a number the two ends agree on at link-up.  It has to survive a
 * reboot of either board and mean the same thing afterwards, or a line in a
 * config file ("display = n2.fb0") points at a different board tomorrow.  So it
 * comes from the node itself: `[cluster] name` in SYSTEM.CFG, and failing that
 * from the chip's own identifier, which is unique without anyone configuring
 * anything.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NODENAME_H
#define ARGON_NODENAME_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Short enough to prefix a device name and still fit AG_DEV_NAME_MAX (24):
 * "n2." + "pcmnull" is eleven, and eight leaves room for a name someone chose.
 */
#define AG_NODE_NAME_MAX 9

/* This node's name.  Never NULL and never empty once the config stage is up. */
const char *ag_node_name(void);

/* Reads `[cluster] name`.  Called once the config stage is up. */
ag_err_t ag_node_name_init(void);

/* Renames this node.  Letters, digits, '-' and '_'; no dot - that separates a
 * node from a device in `n2.pcm0`.  Forgets the peer's name, which a rename
 * makes stale. */
ag_err_t ag_node_name_set(const char *name);

/*
 * The name the node at the other end of the link gave for itself, or NULL when
 * no link is bound or it has not said yet.
 */
const char *ag_node_peer_name(void);

/* Told by the link when a hello arrives.  Not for anyone else. */
void ag_node_set_peer_name(const char *name);

/*
 * What the far node said it is made of.  Cores of 0 means it has not said -
 * read that as one rather than as none, which is what a board that predates
 * this being in the hello will look like.
 */
void     ag_node_set_peer_caps(uint8_t cores, uint32_t cpu_hz);
uint8_t  ag_node_peer_cores(void);
uint32_t ag_node_peer_cpu_hz(void);

/*
 * The same three facts about the neighbour on a NAMED wire, for something
 * enumerating the whole cluster rather than serving one message.  The
 * unindexed calls above are these, asked about the wire in hand.
 */
const char *ag_node_peer_name_at(int32_t wire);
uint8_t     ag_node_peer_cores_at(int32_t wire);
/*
 * The neighbour of the wire in hand has just started (its hello was a
 * question); and how many times each wire's neighbour has, so a board beyond
 * it can tell that the handles it holds there are gone (26 Sep 2026).
 */
void        ag_node_peer_started(void);
uint8_t     ag_node_peer_starts_at(int32_t wire);
uint32_t    ag_node_peer_cpu_hz_at(int32_t wire);

/*
 * A name as a number small enough to travel in every frame.
 *
 * A name is the only identity that survives a reboot and means the same thing
 * on all three boards, but 9 bytes of it in a 24-byte header is a fifth of the
 * payload, so what travels is this hash.  FNV-1a over the bytes, with 0 kept
 * for "no node" - a name that hashes to 0 would be indistinguishable from the
 * "no destination, this frame is for whoever is reading it" case, and three
 * boards make that collision a curiosity rather than a risk.
 *
 * NULL and "" hash to 0.
 */
uint16_t ag_node_hash(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_NODENAME_H */
