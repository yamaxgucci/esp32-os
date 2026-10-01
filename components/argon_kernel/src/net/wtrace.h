/*
 * wtrace - the wire's events in time, for `link trace` (1 Oct 2026).
 *
 * Counters said where the time went in all and not when: the S3's picture
 * left a 16 Mbaud wire quiet 44 times a second, and every total that could
 * have explained it - the painter's waits, the far end's room, the
 * acknowledgements' round trip - looked well inside what the wire needed.
 * This keeps the events themselves for a few hundred milliseconds, each with
 * its microsecond, so what comes before a quiet wire can be read off.
 *
 * Off, a note costs one load of a flag.  On, a slot taken by an atomic add,
 * from any task on either core; full, it stops - the first events are the
 * ones asked for.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_NET_WTRACE_H
#define ARGON_NET_WTRACE_H

#include <stdbool.h>
#include <stdint.h>

enum {
    WT_PASS_IN = 1, /* the painter begins a pass: its number */
    WT_PASS_OUT,    /* ...and ends it: messages sent in it */
    WT_SNAP_IN,     /* a snapshot begins */
    WT_SNAP_OUT,    /* ...and is taken */
    WT_ROOM_IN,     /* a writer waits for room in the link's ring: bytes in
                     * it sent and not answered, and (<< 16) not sent */
    WT_ROOM_OUT,    /* ...and has it */
    WT_WRITE,       /* a message into the link: bytes */
    WT_SUBMIT,      /* a frame handed to the wire: bytes */
    WT_QUIET,       /* ...after the wire had been quiet this many us */
    WT_ACK,         /* an answer freed this many bytes of the ring */
    WT_WAKE_IN,     /* the painter waits for anything marked */
    WT_WAKE_OUT,    /* ...and is woken */
    WT_NOTHING,     /* the sending task stops: the far end's leave, and
                     * (<< 16) the bytes it has not sent */
    WT_RX,          /* a frame's bytes taken into the receive ring */
    WT_TAKE,        /* the reader took this many bytes out of it */
    WT_TXWAKE,      /* the sending task looks: bytes it has not answered */
    WT_SETTLED,     /* ...has a transmit buffer back */
    WT_BUILT,       /* ...has built the frame: its payload */
    WT_LOCKING,     /* a frame's build asks for the link's lock */
    WT_LOCKED,      /* ...and has it */
};

#ifdef ARGON_NO_WTRACE
/* the host tests: the protocol and the painter without the trace */
static inline void wt_note(uint8_t ev, uint32_t a)
{
    (void)ev;
    (void)a;
}
#else
extern volatile bool wt_on;

void wt_note_(uint8_t ev, uint32_t a);

static inline void wt_note(uint8_t ev, uint32_t a)
{
    if (wt_on) {
        wt_note_(ev, a);
    }
}
#endif

/* Recording from now, for `ms` at most; false with no memory for it. */
bool     wt_start(uint32_t ms);
void     wt_stop(void);
uint32_t wt_count(void);
/* Event i of those kept: microseconds from the start, the event, its value. */
bool     wt_get(uint32_t i, uint32_t *us, uint8_t *ev, uint32_t *a);
const char *wt_name(uint8_t ev);

#endif /* ARGON_NET_WTRACE_H */
