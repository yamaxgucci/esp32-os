/*
 * ArgonOS - where things live: the [memory] section.
 *
 * Every piece of the system that can live in more than one kind of memory has
 * a name, and the configuration says where it should go:
 *
 *     [memory]
 *     shell.stack   = psram, iram     ; try psram, then iram
 *     supervisor    = only iram       ; iram or fail
 *     freertos      = flash           ; placed by the linker: needs a rebuild
 *
 * A list is an order of preference.  Without `only`, a piece that fits none of
 * the kinds listed goes wherever it can, and the journal says so; with `only`
 * it goes nowhere else, and the request fails.
 *
 * Two kinds of piece.  DATA - stacks, buffers, tables allocated after the
 * configuration is read, a program's code, data and stack - is placed at boot
 * from C:\BOARD.CFG and then C:\SYSTEM.CFG, so a change takes a reboot.  CODE
 * and the static tables needed before the configuration is read are placed
 * by the linker: `argon target` reads the same section from the board pack's
 * BOARD.CFG and turns it into sdkconfig lines, so a change takes a rebuild,
 * and `mem /p` says when what was built is not what the file asks for.
 *
 * With no [memory] section every piece goes where it went before the section
 * existed (8 Oct 2026); the defaults below are that, not a new policy.
 *
 * This header and place_parse.c are free of kernel and ESP-IDF dependencies so
 * the parser can be unit tested on a host.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PLACE_H
#define ARGON_PLACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AG_PLACE_NONE = 0,
    AG_PLACE_IRAM,  /* internal SRAM - "iram", "sram" or "internal"     */
    AG_PLACE_PSRAM, /* external RAM                                      */
    AG_PLACE_FLASH, /* run in place from flash, through the cache        */
    AG_PLACE_ROM,   /* the chip's mask ROM (the flash driver, nothing else) */
    AG_PLACE_KINDS
} ag_place_kind_t;

#define AG_PLACE_BIT(k) (1u << (unsigned)(k))

typedef struct {
    uint8_t kind[AG_PLACE_KINDS]; /* in order of preference */
    uint8_t count;
    bool    only;                 /* nowhere but these      */
} ag_place_policy_t;

/*
 * "psram, iram", "only iram", "flash".  Words are separated by commas or
 * spaces; `only` may come first.  `allowed` is a mask of AG_PLACE_BIT() of the
 * kinds this piece can live in.  On failure `why` says what was wrong.
 */
ag_err_t ag_place_parse(const char *text, uint32_t allowed,
                        ag_place_policy_t *out, char *why, size_t why_len);

/* "psram, iram" / "only iram" back again; returns the length written. */
size_t ag_place_format(const ag_place_policy_t *p, char *out, size_t cap);

const char *ag_place_kind_name(ag_place_kind_t k);

/* ---- the running system (place.c) ---------------------------------------- */

#ifndef AG_PLACE_HOST_ONLY

/*
 * Reads [memory] from the system configuration.  Called once, at the end of
 * the config stage; before that every piece follows its default.
 */
void ag_place_configure(void);

/* The policy a piece follows now; never NULL for a known piece. */
const ag_place_policy_t *ag_place_policy(const char *piece);

/* Data: memory for `piece`, in the kinds its policy names; NULL if none. */
void *ag_place_alloc(const char *piece, size_t bytes);
void  ag_place_free(const char *piece, void *p);

/*
 * A task whose stack is `piece`'s.  The TCB is always internal, which is
 * FreeRTOS's rule, not ours.  `out_task` (may be NULL) is an ag_port_task_t,
 * written before the task first runs; `got` (may be NULL) receives where the
 * stack went.
 */
bool ag_place_task(const char *piece, void (*fn)(void *), const char *name,
                   uint32_t stack_bytes, void *arg, unsigned prio, int core,
                   void **out_task, ag_place_kind_t *got);

/*
 * The kinds to try for `piece`, in order: its policy's own, then - unless
 * `only` - every other kind it may live in.  For a piece that places itself
 * (the loader, a program's stacks).  Returns how many; `out` holds
 * AG_PLACE_KINDS.
 */
unsigned ag_place_order(const char *piece, ag_place_kind_t *out);

/*
 * The first kind in `piece`'s order that can exist on this board at all -
 * psram is skipped where there is none.  For a piece already placed, to ask
 * whether it should move.
 */
ag_place_kind_t ag_place_first(const char *piece);

/* A task a piece made by other means (the main task, a program's own): its
 * stack is counted for `piece` for as long as it runs. */
void ag_place_adopt(const char *piece, void *task);

/* Bookkeeping for a piece that places itself (the loader): `bytes` went to
 * `kind`; negative bytes give them back. */
void ag_place_note(const char *piece, ag_place_kind_t kind, int32_t bytes);

/* How many build-time pieces were built somewhere the configuration does not
 * ask for - nonzero means "rebuild". */
unsigned ag_place_mismatches(void);

/* `mem /p`: one line per piece through `out`. */
void ag_place_report(void (*out)(const char *line, void *ctx), void *ctx);

#endif /* AG_PLACE_HOST_ONLY */

#ifdef __cplusplus
}
#endif

#endif /* ARGON_PLACE_H */
