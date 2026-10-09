/*
 * ArgonOS - a neighbour's screen, kept up to date by what changed.
 *
 * The screen proxy's side of drawing on a board across the cluster link
 * (net/noderdev.c).  An application draws and presents as it does on any
 * screen; nothing it calls knows the screen is across a wire.  What the proxy
 * hands here is only "these rows of this surface were presented" - under the
 * device registry, so it is noted and nothing more - and a task of the
 * painter's own does the rest outside every lock:
 *
 *   - it keeps ONE copy of the screen - no second frame buffer - and sends
 *     only what differs from it: per row, from the first changed pixel to the
 *     last (a moving pointer is a few rows of a few pixels, not a band);
 *   - it packs what it sends (net/rdcodec.h): flat stretches almost vanish,
 *     noise costs a byte in 257;
 *   - it sends reliably, so the two screens cannot drift apart; a message
 *     that could not be sent makes the whole screen go again;
 *   - it is always painting the LATEST picture: presents that come while it
 *     is sending are folded into the next pass, so a wire slower than the
 *     application costs frames, never lag.
 *
 * The receiving board needs no framebuffer: rdp_draw unpacks each run into
 * one row's worth of pixels and hands it to its panel.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NET_RDPAINT_H
#define ARGON_NET_RDPAINT_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

/* Sends one packed payload to the far screen, reliably; AG_OK or why not. */
typedef ag_err_t (*rdp_send_fn)(uint32_t handle, const uint8_t *pay,
                                uint16_t len);

/*
 * A presented rectangle to bring the far screen up to.  `room` is the most
 * one payload may be.  Called under the device registry: compares it with
 * the painter's one picture, copies in what differs, marks it, wakes the
 * painter and returns.  False when there is no memory for the picture (or
 * the surface outgrew it): the caller then sends the pixels itself, raw.
 */
bool rdp_damage(const ag_blit_t *b, uint32_t handle, uint16_t room,
                rdp_send_fn send);

/* One payload onto a local screen; the pixels drawn, 0 if it was not one. */
uint32_t rdp_draw(const ag_display_ops_t *d, const uint8_t *pay, uint16_t len);
/* Payloads rdp_draw could not draw to the end: a run it could not read.  The
 * rest of such a payload is lost, and the screen with it until repainted. */
uint32_t rdp_draw_faults(void);

/*
 * THE FAR SCREEN'S PACE.  Called by the painter after every pass it sent,
 * and returns when the far end has what the pass carried (or has given up
 * waiting).  Without it the painter kept the pace of the FIRST wire: a
 * neighbour's screen two hops away, behind a 4 Mbaud wire, was fed at
 * 16 Mbaud, and the board in the middle stood in its relay with a queue it
 * could not empty - hearing nothing else, its own answers included (26 Sep
 * 2026).  Waiting a pass costs frames, not the picture: what changes
 * meanwhile goes in the next pass, which is what the painter is for.
 */
typedef void (*rdp_sync_fn)(uint32_t handle);
void rdp_set_sync(rdp_sync_fn sync);

/*
 * The far screen no longer shows what the painter sent: it gave its glass
 * back to its own console meanwhile.  Every row goes again on the next pass.
 */
void rdp_forget(void);

/* Since the board started: frames that went whole (a snapshot's every row),
 * the passes they took - passes are not frames, a big one is several - and
 * how long the far end took to say a pass was drawn, on average. */
void rdp_counts(uint32_t *frames, uint32_t *passes, uint32_t *wait_us);

/* For the sync: whether the pass just sent was the last of its frame - the
 * far end counts the frames it has drawn whole, which is what a person sees. */
bool rdp_pass_whole(void);

/* The far screen said it draws a rectangle sent as one run; this screen
 * does (it has the buffer to unpack one into).  See rdpaint.c. */
void rdp_far_rects(bool yes);
bool rdp_draws_rects(void);
/* Whether rdp_draw holds pixels not yet on the glass. */
bool rdp_draw_holds(void);
/* Why a strip gathered by rdp_draw went to the glass. */
enum {
    RDP_FL_EVICT, /* full, or neither slot took the next piece       */
    RDP_FL_UNDER, /* a new piece lands on it                         */
    RDP_FL_FRAME, /* a frame's end                                   */
    RDP_FL_IDLE,  /* nothing more arrived                            */
    RDP_FL_OTHER, /* another screen, or a piece bigger than a slot   */
    RDP_FL_N
};
/* Time rdp_draw spent unpacking and blitting, its blits, and why its strips
 * were drawn, since asked. */
void rdp_draw_times(uint32_t *unpack_us, uint32_t *blit_us, uint32_t *blits,
                    uint32_t why[RDP_FL_N]);
/* Draw what rdp_draw is still gathering, for the reason `why` (RDP_FL_*). */
void rdp_draw_flush(unsigned why);

/* Times every row went again: the wire refused a pass, or the far screen
 * said it no longer shows what was sent. */
uint32_t rdp_redraws(void);

/* The painter's line a second in the journal: off unless asked. */
void rdp_report(bool on);

#endif /* ARGON_NET_RDPAINT_H */
