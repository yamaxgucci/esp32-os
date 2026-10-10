/*
 * ArgonOS - the node link, as the kernel reaches it.
 *
 * The same shape as the network provider next door (argon/netprov.h) and for
 * the same reason: one indirection, one bound device at a time, and everything
 * above it written against the indirection rather than against a driver.
 *
 * The difference is what is on the far end.  A network provider reaches
 * somebody else's machine; this reaches the other half of THIS machine - a node
 * of the same system, running the same image, whose processes appear in the
 * same `ps` and whose devices appear in the same `dev`.  What crosses it is
 * messages with a channel and a deadline, never bytes: see ag_link_ops_t.
 *
 * NOTHING IS BOUND BY DEFAULT and there is no built-in provider, which is the
 * other difference.  A board with no second node has no link, and every call
 * here answers -AG_ENODEV until `link use <dev>` says otherwise.  A built-in
 * fallback would be a loopback pretending to be a neighbour, and a cluster that
 * silently talks to itself is worse than one that says it is alone.
 *
 * PUMPING.  A loadable driver has no task (see nodelink.c), so the wire only
 * advances while somebody is inside one of these calls.  recv() with a timeout
 * and poll() are how a caller with nothing to say still lets the far end be
 * heard.  Until the ABI grows a service task, a node that must hear an
 * unsolicited request has to poll for it.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_LINKPROV_H
#define ARGON_LINKPROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

/* Bind an AG_DEV_LINK device by name, or "none" to let go of the one bound. */
ag_err_t ag_linkprov_use(const char *name);

/* The bound device's name, or "none". */
const char *ag_linkprov_active(void);
bool        ag_linkprov_bound(void);

/* Has anything been heard from the far end since the link came up. */
bool ag_linkprov_up(void);

/*
 * WHICH WIRE THESE MEAN, when a board has more than one neighbour.
 *
 * The wire this call is reading, if it is being made from a link's reader task
 * - so an answer goes back out the wire its question arrived on, and no
 * service above had to be told about wires at all.  Otherwise the selected
 * wire, the one `link use` last named.
 *
 * Only something that starts a NEW conversation with a named far node has to
 * choose, and that is what the _on and _at calls below are for.
 */
int32_t ag_linkprov_send(uint8_t chan, uint8_t cls, const void *buf,
                         size_t len);
void    ag_linkprov_poll(uint32_t ms);

/* ------------------------------------------------------------------------ */
/* Several wires: choosing one on purpose                                    */
/* ------------------------------------------------------------------------ */

/*
 * How many neighbours one board can hold at once.  Four rather than two
 * because the shape worth supporting is a star - one board with the screen and
 * the card, several small ones each working a core - and four leaves is
 * already more compute than the wire between them can feed.
 *
 * Each wire costs a reader task (4 KB of stack), so this is not free and is
 * not meant to grow without a measurement behind it.
 */
#define AG_LINK_WIRES 4u

/* Which wire this call means - the index behind the rule above, or -1 when
 * there is none.  For a service that must remember something PER NEIGHBOUR
 * while serving a message from one of them. */
int32_t ag_linkprov_current(void);

/* How many wires are bound, and the name of one by index (NULL past the end).
 * Indices are stable while a wire stays bound. */
int32_t     ag_linkprov_count(void);
const char *ag_linkprov_name_at(int32_t wire);
int32_t     ag_linkprov_index_of(const char *name);

/*
 * Bytes still unused on a wire's reader task, at its worst moment so far.
 *
 * Worth having at the prompt because this task serves the far node's file
 * reads and therefore runs the whole filesystem stack: when it overflowed, the
 * board did not report a fault, it REBOOTED - and a board that reboots cannot
 * tell you why.  A number that can be looked at turns that into a warning.
 */
uint32_t ag_linkprov_reader_stack_left(int32_t wire);

/* Let go of one named wire.  `ag_linkprov_use("none")` drops the selected one;
 * this drops the one asked for, whichever is selected. */
ag_err_t ag_linkprov_drop(const char *name);

/* Make a bound wire the selected one, as `link use` does for a wire already
 * bound - without starting its driver again. */
ag_err_t ag_linkprov_select(int32_t wire);

/* Send on a named wire rather than on the one this call would mean. */
int32_t ag_linkprov_send_on(int32_t wire, uint8_t chan, uint8_t cls,
                            const void *buf, size_t len);

/* ------------------------------------------------------------------------ */
/* Beyond the neighbour: the same frame, addressed                            */
/* ------------------------------------------------------------------------ */

/*
 * `dst` is ag_node_hash() of the node this frame is for; 0 means "whoever is
 * reading it", which is what every frame meant before a leaf had a neighbour
 * it could not reach directly.  A frame whose dst is neither 0 nor the
 * reader's own hash is RELAYED by the board in between (see nodelink.c), so
 * addressing a far leaf is the whole of what a sender does: name it, and let
 * the wire under it choose the first hop.
 *
 * The first hop is the wire this call would use anyway - the selected one, or
 * the one it is being made from - so a leaf that named the far node before
 * switching wires would address it out of the wrong one.  `link use` therefore
 * sets both.
 */
int32_t ag_linkprov_send_to(uint16_t dst, uint8_t chan, uint8_t cls,
                            const void *buf, size_t len);
int32_t ag_linkprov_send_to_on(int32_t wire, uint16_t dst, uint8_t chan,
                               uint8_t cls, const void *buf, size_t len);

/*
 * The node that sent the frame being handled, or 0.  Valid while a receive
 * callback runs, and it is what an ANSWER must be addressed to: a reply that
 * says "for whoever is on the wire" is taken by the board in between instead
 * of being passed home, and the far end then waits for an answer that arrived
 * somewhere else.
 */
uint16_t ag_linkprov_from(void);

/* This board's own name hash, so a service can tell "for me" from "for the one
 * beyond" without hashing its own name on every frame. */
uint16_t ag_linkprov_here(void);

/* The node that NEW conversations are addressed to, as `link use` last set it;
 * 0 until something beyond the neighbour is named.  ag_linkprov_send() stamps
 * exactly this, which is why no service above had to learn about addressing. */
uint16_t ag_linkprov_target(void);
void     ag_linkprov_set_target(uint16_t hash);

/* Let every wire move, not only the current one: for a caller that is waiting
 * on answers from several neighbours at once. */
void ag_linkprov_poll_all(uint32_t ms);

/* ------------------------------------------------------------------------ */
/* Receiving: one reader, and handlers per channel                           */
/* ------------------------------------------------------------------------ */
/*
 * THERE IS NO recv().  There was, and it was a mistake with a history: the link
 * hands back whatever arrived next, so two callers reading it steal from each
 * other.  The moment a second consumer appeared - a server for remote runs
 * beside the shell's own ping - six messages in twenty-four went to the wrong
 * one, and the shell reported them lost.  This tree has now made that mistake
 * on a UART twice and on a link once.
 *
 * So exactly one task reads, here, and hands each message to the handler for
 * its channel.  A channel is one conversation; a consumer takes one and hears
 * nothing else.
 *
 * A HANDLER RUNS ON THE READER TASK, which has two consequences worth knowing
 * rather than discovering.  It must not block for long: while it is inside,
 * nothing else on this link is read.  And it must not call ag_linkprov_listen.
 * Sending IS allowed and is the normal thing to do - that is how an answer is
 * given.
 */
typedef void (*ag_link_rx_fn)(uint8_t chan, const void *buf, uint16_t len,
                              void *ctx);

ag_err_t ag_linkprov_listen(uint8_t chan, ag_link_rx_fn fn, void *ctx);

/* Stop listening on a channel.  Safe on one nobody holds. */
void ag_linkprov_unlisten(uint8_t chan);

/* Is the reader task up.  It starts with the first bind and stops with the
 * last unbind. */
bool ag_linkprov_reading(void);

/*
 * True only on the reader task itself.  Anything that sends a request and then
 * waits for its answer must refuse to run here: the answer is delivered by
 * this task, so waiting on it waits for itself.
 */
bool ag_linkprov_in_reader(void);

ag_err_t ag_linkprov_stats(ag_link_stats_t *out);

/*
 * What a driver that does not implement info() is taken to be.
 *
 * These are the hand-soldered UART's own measured figures and not a guess: 232
 * bytes is what fits this tree's frame inside nlink's 256-byte quantum, and
 * 400 067 B/s is what `link send/recv` measured at 4 Mbaud between two boards.
 * A driver built before ABI 0.52 could only have been that UART, so answering
 * with its numbers is correct rather than merely safe.
 *
 * The same wire at 16 Mbaud carries 1 132 730 B/s of payload with a quarter of
 * the frames thrown away and re-sent - measured too, and 2.83x the useful rate.
 * It is not the default because the default must hold for the driver that has
 * not been asked; a driver that runs the line that fast says so through info().
 */
#define AG_LINK_DEFAULT_MTU 232u
#define AG_LINK_DEFAULT_RATE 400067u
#define AG_LINK_DEFAULT_RTT_US 3000u

/*
 * The smallest message every node service in this tree still works over.
 *
 * Those services size their own frames by hand - the remote drive at 232 bytes,
 * remote runs at 240, the job channel by asking - and a wire that cannot carry
 * that much would break them in the worst way available: a request refused deep
 * inside a driver, reported as whatever error happened to come back.
 *
 * So a bound link is measured against this ONCE, at the moment it is bound, and
 * a wire that is too small says so there.  It is not a refusal: the link still
 * binds and small traffic still works.  It is the difference between a stated
 * limitation and a mystery, and the day a quad SPI or a parallel bus appears,
 * this line is what tells its author which number matters.
 */
#define AG_LINK_MIN_MTU 248u

/* The bound wire's shape.  Never answers "unknown" - see the note above. */
ag_err_t ag_linkprov_info(ag_link_info_t *out);
/* The same for one wire by index, whichever this call would otherwise mean. */
ag_err_t ag_linkprov_info_on(int32_t wire, ag_link_info_t *out);

/*
 * Frames this board moved between two neighbours, frames it had to drop, and
 * how many times the reader left a wire unread because the queue out had no
 * room - the back-pressure, counted.
 */
/* Windows in which the reader spent its 2 ms and was dropped below
 * applications for the rest (see READER_PRIO_HIGH in linkprov.c). */
uint32_t ag_linkprov_reader_overruns(void);
/* The reader's slow messages since the last call (the probe, 1 Oct 2026): the
 * longest one took to route and its channel, and those of 1 ms or more. */
void     ag_linkprov_reader_slow(uint32_t *max_us, uint8_t *max_chan,
                                 uint32_t *slow, uint32_t *slow_us);
/* The same for the relay's task, which is on the same terms. */
uint32_t ag_linkprov_relay_overruns(void);
/* `link report on|off`: the link's second-by-second meters in the journal. */
/* Background's window: sends it held back, answers this board gave, counts
 * started again, and times this board, in the middle, held it for want of
 * time. */
void ag_linkprov_bg_counts(uint32_t *waits, uint32_t *credits,
                           uint32_t *resyncs, uint32_t *relay_held);
void ag_linkprov_report(bool on);
bool ag_linkprov_reporting(void);

void ag_linkprov_relay_counts(uint32_t *relayed, uint32_t *dropped,
                              uint32_t *held);

/*
 * `link flood`: meaningless bytes into the selected wire for `ms`, as fast as
 * it takes them, from a task at priority `prio`, in frames of `size` (0 or
 * too big: the wire's largest).  The far board's sink counts what arrives and
 * is asked at the end.  A tool for measuring the wire beside real traffic.
 */
typedef struct {
    uint32_t size;        /* bytes a frame                                 */
    uint32_t sent_bytes;
    uint32_t sent_frames;
    uint32_t sent_ms;
    uint32_t full;        /* times the wire had no room and the flood waited */
    int32_t  error;
    bool     answered;    /* the far end said what it got                  */
    uint32_t got_bytes;
    uint32_t got_frames;
    uint32_t got_ms;      /* first frame to last, by the far end's clock   */
    uint32_t got_order;   /* frames that arrived out of their turn         */
    uint32_t got_wrong;   /* frames with a byte other than the one sent    */
} ag_linkprov_flood_t;

/* cls: AG_LINK_BACKGROUND (the default) or AG_LINK_BULK - the second without
 * the background's end-to-end window, to measure the wire itself */
ag_err_t ag_linkprov_flood(uint32_t ms, uint32_t size, unsigned prio,
                           uint8_t cls, ag_linkprov_flood_t *out);

#endif /* ARGON_LINKPROV_H */
