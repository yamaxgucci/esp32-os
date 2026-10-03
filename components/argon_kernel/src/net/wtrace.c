/*
 * wtrace - the wire's events in time (see wtrace.h).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "net/wtrace.h"

#include <stddef.h>

#include <argon/port/mem.h>
#include <argon/port/time.h>

#define WT_SLOTS 4096u

typedef struct {
    uint32_t us;
    uint32_t a;
    uint8_t  ev;
} wt_slot_t;

volatile bool              wt_on;
static wt_slot_t          *s_slot;
static uint32_t            s_slots; /* as many as there was memory for */
static volatile uint32_t   s_next;
static int64_t             s_t0;
static int64_t             s_until;

static uint32_t s_mask;

void wt_only(uint32_t mask)
{
    s_mask = mask;
}

void wt_note_(uint8_t ev, uint32_t a)
{
    if (s_mask != 0u && (ev >= 32u || (s_mask & (1u << ev)) == 0u)) {
        return;
    }
    const int64_t now = ag_port_us();
    if (now > s_until) {
        wt_on = false;
        return;
    }
    const uint32_t i = __atomic_fetch_add(&s_next, 1u, __ATOMIC_RELAXED);
    if (i >= s_slots) {
        wt_on = false;
        return;
    }
    s_slot[i].us = (uint32_t)(now - s_t0);
    s_slot[i].a = a;
    s_slot[i].ev = ev;
}

bool wt_start(uint32_t ms)
{
    wt_on = false;
    /* the most that fits, down to a sixteenth: the CYD has no 48 KB */
    for (uint32_t n = WT_SLOTS; s_slot == NULL && n >= WT_SLOTS / 16u;
         n /= 2u) {
        s_slot = (wt_slot_t *)ag_port_alloc(n * sizeof(wt_slot_t),
                                            AG_MEM_BYTE);
        s_slots = (s_slot != NULL) ? n : 0u;
    }
    if (s_slot == NULL) {
        return false;
    }
    s_next = 0;
    s_t0 = ag_port_us();
    s_until = s_t0 + (int64_t)ms * 1000;
    wt_on = true;
    return true;
}

void wt_stop(void)
{
    wt_on = false;
}

uint32_t wt_count(void)
{
    return (s_next < s_slots) ? s_next : s_slots;
}

bool wt_get(uint32_t i, uint32_t *us, uint8_t *ev, uint32_t *a)
{
    if (s_slot == NULL || i >= wt_count()) {
        return false;
    }
    *us = s_slot[i].us;
    *ev = s_slot[i].ev;
    *a = s_slot[i].a;
    return true;
}

const char *wt_name(uint8_t ev)
{
    static const char *const k[] = {"?",      "pass",   "pass.",  "snap",
                                    "snap.",  "room",   "room.",  "write",
                                    "submit", "quiet",  "ack",    "wake",
                                    "wake.",  "nothing", "rx",     "take",
                                    "txwake", "settled", "built",  "locking",
                                    "locked", "scrfull", "scrroom", "draw",
                                    "draw.",  "sndq"};
    return (ev < sizeof(k) / sizeof(k[0])) ? k[ev] : "?";
}
