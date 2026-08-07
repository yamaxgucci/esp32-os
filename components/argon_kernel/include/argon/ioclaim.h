/*
 * ArgonOS - who owns which pin.
 *
 * Direct access to the hardware is deliberate policy (docs/00-architecture.md
 * §8.1): an application may drive a pin without asking a driver's permission.
 * What direct access must not mean is two pieces of code driving the same pin
 * without knowing about each other, because that failure does not look like a
 * failure - it looks like hardware that behaves strangely on Tuesdays.
 *
 * So there is exactly one table, and it answers two questions: which pins the
 * system itself needs and will not give up, and who took each of the rest.
 *
 * The rule, in one line: **a reserved pin refuses configuration and writing, a
 * pin held by somebody else refuses the same, and anybody may read any pin.**
 * Reading is harmless and is how you find out what is going on; writing is what
 * breaks things.
 *
 * No dependency on FreeRTOS or the chip, so this is built and tested on the
 * host - which matters, because "who is allowed to do what" is the kind of rule
 * that is easy to get almost right.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_IOCLAIM_H
#define ARGON_IOCLAIM_H

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The S3 goes up to GPIO 48; 64 covers the family without being a guess. */
#define AG_IO_MAX_PINS 64

/* Why a pin is held, short enough to fit a column of the `io` listing. */
#define AG_IO_REASON_MAX 16

typedef enum {
    AG_PIN_FREE = 0,
    AG_PIN_RESERVED, /* the system needs it: console, card, flash, PSRAM   */
    AG_PIN_HELD,     /* somebody claimed it through io                     */
} ag_pin_state_t;

typedef struct {
    ag_pin_state_t state;
    ag_pid_t       owner; /* AG_PID_KERNEL when the kernel holds it        */
    char           why[AG_IO_REASON_MAX];
    bool           isr; /* an interrupt handler is installed on it         */
} ag_pin_info_t;

/*
 * `pins` is how many the chip has; claims above that are -AG_ERANGE.  Clears
 * everything, so the boot stage calls it once.
 */
ag_err_t ag_io_claims_init(int pins);

int ag_io_pin_count(void);

/*
 * The system's own pins: the console, the card, the flash, and the pins of a
 * bus once that bus is running.  Reserved beats a claim - the answer to anyone
 * asking for one is -AG_EACCES rather than -AG_EBUSY, because "wait for it" and
 * "you will never get this" are different answers and only one is worth
 * retrying.
 *
 * -AG_EBUSY when an application is holding the pin.  The kernel does not take
 * a pin from a running application, and cannot be allowed to: a bus and the
 * shell are both "the kernel", so a reservation that overrode a claim would
 * make the shell able to pull SDA out from under a driver mid-transaction and
 * call it ownership.
 *
 * ag_io_reservable() is the question to ask first when several pins have to be
 * taken together, so that a bus does not reserve half of what it needs and then
 * find the other half taken.
 */
bool     ag_io_reservable(int pin);
ag_err_t ag_io_reserve(int pin, const char *why);

/*
 * Takes a pin.  -AG_EACCES for a reserved one, -AG_EBUSY for one somebody else
 * holds, -AG_ERANGE for a pin the chip does not have.  Claiming a pin you
 * already hold succeeds and updates the reason: reconfiguring your own pin is
 * an ordinary thing to do.
 */
ag_err_t ag_io_claim(int pin, ag_pid_t owner, const char *why);

/* -AG_EPERM when the caller is not the holder: releasing somebody else's pin
 * would be a way to take it. */
ag_err_t ag_io_release(int pin, ag_pid_t owner);

bool ag_io_held_by(int pin, ag_pid_t owner);

/* True when the pin may be driven by this owner - free to claim, or already
 * theirs.  Reserved pins are never writable. */
bool ag_io_writable_by(int pin, ag_pid_t owner);

/* Remembers that a handler is installed, so that releasing the pin can remove
 * it.  -AG_EPERM when the caller does not hold the pin. */
ag_err_t ag_io_set_isr(int pin, ag_pid_t owner, bool installed);

/*
 * Gives back everything one owner holds; the callback undoes whatever the
 * hardware needs undone, per pin, before the entry is cleared.
 *
 * This is the call that keeps a process from outliving its interrupt handler.
 * A handler whose code has been freed is not a leak - it is a board that stops
 * at the next edge on that pin, with nothing in the journal to say why.
 */
typedef void (*ag_io_release_fn)(int pin, bool had_isr, void *ctx);

uint32_t ag_io_release_owner(ag_pid_t owner, ag_io_release_fn release,
                             void *ctx);

/* -AG_ERANGE for a pin the chip does not have. */
ag_err_t ag_io_pin_info(int pin, ag_pin_info_t *out);

/* How many pins are reserved or held, for `io` and for leak checks. */
uint32_t ag_io_claimed_count(void);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_IOCLAIM_H */
