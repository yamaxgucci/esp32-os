/*
 * ArgonOS - the wire format for cluster threads, and nothing else.
 *
 * Split out of nodeclu.c on purpose.  Everything else in that file needs a
 * link, two boards and a scheduler before it will do anything at all, and the
 * part most likely to be wrong - cutting a message into frames and putting it
 * back together while several senders interleave - needs none of those.  So it
 * lives here, depends on nothing above the C library, and is tested on the
 * development machine where a failure is a line number instead of a board that
 * stopped.
 *
 * REASSEMBLY IS KEYED BY (from, to), which is the whole reason this is not
 * three lines.  The link delivers the frames of one channel in order, so the
 * fragments of one message arrive in order and no offset needs to travel with
 * them.  But two threads on the far board sending at the same time put their
 * fragments on the SAME channel, interleaved, and a single accumulating buffer
 * would splice one message into the other - silently, since both are only
 * bytes.  One slot per conversation fixes it, and four slots is more
 * conversations than two boards can have in flight.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_CLUFRAME_H
#define ARGON_CLUFRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Header on every frame.  Fixed, so a payload's size is the frame's less this. */
#define AG_CLUF_HDR 16u

enum {
    /*
     * a = tid to create, b = length of the argument blob.  The payload is the
     * entry name, then the image path, then the argument - the first two
     * terminated.  Three variable things in one frame because a spawn must fit
     * ONE frame: a request that could itself be fragmented would need
     * reassembly to be keyed by a thread that does not exist yet.
     */
    AG_CLUF_SPAWN = 1,
    /* a = tid, b = AG_OK or the negative reason it could not start. */
    AG_CLUF_SPAWN_OK = 2,
    /* a = sender tid, b = destination tid, c = total bytes of the message. */
    AG_CLUF_DATA = 3,
    /* a = tid whose mailbox was drained, b = how many messages came out. */
    AG_CLUF_CREDIT = 4,
    /* a = tid that has returned. */
    AG_CLUF_EXIT = 5,
    /* The job is over: stop everything that belongs to it. */
    AG_CLUF_END = 6,
};

/* On AG_CLUF_DATA only. */
enum {
    AG_CLUF_F_FIRST = 1u << 0,
    AG_CLUF_F_LAST = 1u << 1,
};

typedef struct ag_cluf_hdr {
    uint8_t  op;
    uint8_t  flags;
    uint16_t len; /* payload bytes in THIS frame */
    int32_t  a;
    int32_t  b;
    uint32_t c;
} ag_cluf_hdr_t;

/*
 * Writes the header and `h->len` payload bytes into `buf`, which must have room
 * for AG_CLUF_HDR + h->len.  Returns what was written.
 */
size_t ag_cluf_put(uint8_t *buf, const ag_cluf_hdr_t *h, const void *payload);

/*
 * Reads one frame.  False when it is too short to be one, or claims more
 * payload than arrived - which is a corrupt frame and not a fragment, because
 * the link delivers whole messages or none.
 */
bool ag_cluf_get(const uint8_t *buf, uint16_t n, ag_cluf_hdr_t *h,
                 const uint8_t **payload);

/* Conversations reassembling at once.  See the note at the top of the file. */
#define AG_CLUF_SLOTS 4u

typedef struct ag_cluf_slot {
    bool     used;
    int32_t  from;
    int32_t  to;
    uint32_t total;
    uint32_t got;
    uint8_t *buf;
} ag_cluf_slot_t;

/*
 * The allocator is handed in rather than assumed, for the same reason the whole
 * file is separate: the kernel's heap is not there where this is tested.
 * `free_fn` is called for a message that is abandoned, never for one that has
 * been handed over - a completed message belongs to whoever took it.
 */
typedef struct ag_cluf_rx {
    ag_cluf_slot_t slot[AG_CLUF_SLOTS];
    void *(*alloc_fn)(uint32_t bytes, void *ctx);
    void (*free_fn)(void *p, void *ctx);
    void    *ctx;
    uint32_t dropped; /* messages abandoned: no slot, no memory, bad frame */
} ag_cluf_rx_t;

/*
 * Feeds one AG_CLUF_DATA frame in.
 *
 * Returns 1 when a whole message is ready, and *out_buf is then the caller's to
 * free; 0 when more is expected; a negative ag_errno when the frame could not
 * be taken - which abandons whatever had accumulated for that conversation,
 * since half a message is worse than none.
 */
int ag_cluf_rx_data(ag_cluf_rx_t *rx, const ag_cluf_hdr_t *h,
                    const uint8_t *payload, uint8_t **out_buf,
                    uint32_t *out_len, int32_t *from, int32_t *to);

/* Abandons everything half-received.  What the end of a job does. */
void ag_cluf_rx_reset(ag_cluf_rx_t *rx);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_CLUFRAME_H */
