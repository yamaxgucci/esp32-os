/*
 * ArgonOS - who owns which pin.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/ioclaim.h>

#include <string.h>

/* For AG_PID_KERNEL only: the table itself knows nothing about processes
 * beyond the fact that an owner has a pid. */
#include <argon/proc.h>

typedef struct {
    uint8_t  state;
    bool     isr;
    ag_pid_t owner;
    char     why[AG_IO_REASON_MAX];
} pin_t;

static pin_t s_pins[AG_IO_MAX_PINS];
static int   s_count;

/*
 * No lock.  Every caller is already inside the io layer, which serialises on
 * its own mutex, and a table that took a second lock would only add an order to
 * get wrong.  Said here because the absence of one is a decision, not an
 * oversight: nothing outside io.c may call this.
 */

ag_err_t ag_io_claims_init(int pins)
{
    if (pins < 0 || pins > AG_IO_MAX_PINS) {
        return -AG_EINVAL;
    }
    memset(s_pins, 0, sizeof(s_pins));
    s_count = pins;
    return AG_OK;
}

int ag_io_pin_count(void) { return s_count; }

static pin_t *pin_at(int pin)
{
    if (pin < 0 || pin >= s_count) {
        return NULL;
    }
    return &s_pins[pin];
}

static void set_reason(pin_t *p, const char *why)
{
    if (why == NULL) {
        p->why[0] = '\0';
        return;
    }
    strncpy(p->why, why, sizeof(p->why) - 1);
    p->why[sizeof(p->why) - 1] = '\0';
}

bool ag_io_reservable(int pin)
{
    const pin_t *p = pin_at(pin);
    return p != NULL && p->state != AG_PIN_HELD;
}

ag_err_t ag_io_reserve(int pin, const char *why)
{
    pin_t *p = pin_at(pin);
    if (p == NULL) {
        return -AG_ERANGE;
    }
    /*
     * Never over a running application.  The system's own pins are reserved
     * before anything is loaded, so this only refuses in the one case where
     * refusing is right: a bus being brought up on a pin an application is
     * already driving.  Taking it would be the shell and a driver having equal
     * claim on the same wire, which is the thing this table exists to prevent.
     */
    if (p->state == AG_PIN_HELD) {
        return -AG_EBUSY;
    }

    p->state = AG_PIN_RESERVED;
    p->owner = AG_PID_KERNEL;
    p->isr = false;
    set_reason(p, why);
    return AG_OK;
}

ag_err_t ag_io_claim(int pin, ag_pid_t owner, const char *why)
{
    pin_t *p = pin_at(pin);
    if (p == NULL) {
        return -AG_ERANGE;
    }
    if (p->state == AG_PIN_RESERVED) {
        return -AG_EACCES;
    }
    if (p->state == AG_PIN_HELD && p->owner != owner) {
        return -AG_EBUSY;
    }

    p->state = AG_PIN_HELD;
    p->owner = owner;
    set_reason(p, why);
    return AG_OK;
}

ag_err_t ag_io_release(int pin, ag_pid_t owner)
{
    pin_t *p = pin_at(pin);
    if (p == NULL) {
        return -AG_ERANGE;
    }
    if (p->state != AG_PIN_HELD) {
        return (p->state == AG_PIN_RESERVED) ? -AG_EACCES : -AG_ENOENT;
    }
    if (p->owner != owner) {
        return -AG_EPERM;
    }

    memset(p, 0, sizeof(*p));
    return AG_OK;
}

bool ag_io_held_by(int pin, ag_pid_t owner)
{
    const pin_t *p = pin_at(pin);
    return p != NULL && p->state == AG_PIN_HELD && p->owner == owner;
}

bool ag_io_writable_by(int pin, ag_pid_t owner)
{
    const pin_t *p = pin_at(pin);
    if (p == NULL || p->state == AG_PIN_RESERVED) {
        return false;
    }
    return p->state == AG_PIN_FREE || p->owner == owner;
}

ag_err_t ag_io_set_isr(int pin, ag_pid_t owner, bool installed)
{
    pin_t *p = pin_at(pin);
    if (p == NULL) {
        return -AG_ERANGE;
    }
    if (p->state != AG_PIN_HELD || p->owner != owner) {
        return -AG_EPERM;
    }
    p->isr = installed;
    return AG_OK;
}

uint32_t ag_io_release_owner(ag_pid_t owner, ag_io_release_fn release,
                             void *ctx)
{
    uint32_t freed = 0;

    for (int i = 0; i < s_count; i++) {
        pin_t *p = &s_pins[i];
        if (p->state != AG_PIN_HELD || p->owner != owner) {
            continue;
        }
        /*
         * The hardware is put back before the entry goes, not after: between
         * the two the pin is nobody's, and a handler still installed on a
         * nobody's pin is exactly what this call exists to prevent.
         */
        if (release != NULL) {
            release(i, p->isr, ctx);
        }
        memset(p, 0, sizeof(*p));
        freed++;
    }
    return freed;
}

ag_err_t ag_io_pin_info(int pin, ag_pin_info_t *out)
{
    const pin_t *p = pin_at(pin);
    if (p == NULL || out == NULL) {
        return -AG_ERANGE;
    }

    memset(out, 0, sizeof(*out));
    out->state = (ag_pin_state_t)p->state;
    out->owner = p->owner;
    out->isr = p->isr;
    memcpy(out->why, p->why, sizeof(out->why));
    return AG_OK;
}

uint32_t ag_io_claimed_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < s_count; i++) {
        if (s_pins[i].state != AG_PIN_FREE) {
            n++;
        }
    }
    return n;
}
