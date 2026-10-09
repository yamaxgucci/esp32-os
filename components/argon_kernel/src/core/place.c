/*
 * ArgonOS - where things live: the running half of the [memory] section.
 * The why is in argon/place.h; the values are parsed by place_parse.c.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/place.h>

#include <stdio.h>
#include <string.h>

#include <argon/cfg.h>
#include <argon/log.h>

#include <argon/port/mem.h>
#include <argon/port/place.h>
#include <argon/port/task.h>

#include "core/sysconfig.h"

#define TAG "memory"

#define K_IRAM  AG_PLACE_BIT(AG_PLACE_IRAM)
#define K_PSRAM AG_PLACE_BIT(AG_PLACE_PSRAM)
#define K_FLASH AG_PLACE_BIT(AG_PLACE_FLASH)
#define K_ROM   AG_PLACE_BIT(AG_PLACE_ROM)
#define K_DATA  (K_IRAM | K_PSRAM)
#define K_CODE  (K_IRAM | K_FLASH)

typedef struct {
    const char *name;
    uint32_t    allowed;
    /* The default: where the piece went before [memory] existed.  NULL for a
     * build-time piece, whose default is whatever was built. */
    const char *dflt;
    bool        build;   /* placed by the linker: a change needs a rebuild */
    const char *what;
} piece_t;

/*
 * The pieces.  A new one is a line here and a call to ag_place_alloc /
 * ag_place_task where it is made - nothing else knows the list.
 */
static const piece_t k_pieces[] = {
    {"shell.stack",   K_DATA, "iram", false, "the first prompt's stack"},
    {"console",       K_DATA, "iram", false, "console screen and its task"},
    {"supervisor",    K_DATA, "only iram", false, "supervisor's stack"},
    {"link.tasks",    K_DATA, "iram, psram", false, "the wires' tasks"},
    {"cluster.tasks", K_DATA, "iram, psram", false, "remote runs, files, jobs"},
    {"kernel.tables", K_DATA, "iram", false, "tables made when first needed"},
    {"app.code",      K_IRAM | K_PSRAM | K_FLASH, "iram, psram, flash", false,
     "a program's code"},
    {"app.data",      K_DATA, "psram, iram", false, "a program's data"},
    {"app.stack",     K_DATA, "iram, psram", false, "a program's stacks"},

    {"freertos",      K_CODE, NULL, true, "FreeRTOS calls from tasks"},
    {"heap.code",     K_CODE, NULL, true, "the allocator's code"},
    {"ringbuf.code",  K_CODE, NULL, true, "ring buffers' code"},
    {"wifi.fast",     K_CODE, NULL, true, "Wi-Fi fast paths"},
    {"phy.fast",      K_CODE, NULL, true, "radio PHY fast paths"},
    {"libc.locks",    K_CODE, NULL, true, "libc locks"},
    {"driver.isr",    K_CODE, NULL, true, "I2C/timer/PWM/RMT interrupts"},
    {"flash.driver",  K_IRAM | K_ROM, NULL, true, "the flash driver"},
    {"kernel.static", K_DATA, NULL, true, "static kernel tables"},
    {"dev.table",     K_DATA, NULL, true, "the device table"},
};
#define PIECES (sizeof(k_pieces) / sizeof(k_pieces[0]))

/* Keys of [memory] that are not pieces but belong there. */
static const char *const k_other_keys[] = {"app_arena_kb"};

typedef struct {
    ag_place_policy_t want;
    bool              from_config;
    ag_place_policy_t built;     /* build-time pieces: what the port reports */
    bool              built_known;
    int32_t           bytes[AG_PLACE_KINDS];
    bool              warned;    /* one "went elsewhere" line per piece */
} state_t;

static state_t s_state[PIECES];
static bool    s_ready;

/*
 * The tasks each piece made, so `mem /p` can count their stacks from the
 * running list: a task that ended - finished, killed, faulted - is simply not
 * there any more, which no bookkeeping at each of those exits would get right.
 */
#define PLACE_TASKS 48
static struct {
    void   *task;
    uint8_t piece;
} s_tasks[PLACE_TASKS];

static void remember(int piece, void *task)
{
    if (piece < 0 || task == NULL) {
        return;
    }
    int free_at = -1;
    for (int k = 0; k < PLACE_TASKS; k++) {
        if (s_tasks[k].task == task) { /* a handle the scheduler reused */
            s_tasks[k].piece = (uint8_t)piece;
            return;
        }
        if (s_tasks[k].task == NULL && free_at < 0) {
            free_at = k;
        }
    }
    if (free_at < 0) {
        free_at = 0; /* full: the oldest record goes; `mem /p` undercounts */
    }
    s_tasks[free_at].task = task;
    s_tasks[free_at].piece = (uint8_t)piece;
}

static int find(const char *piece)
{
    for (size_t i = 0; i < PIECES; i++) {
        if (strcmp(k_pieces[i].name, piece) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void defaults(void)
{
    if (s_ready) {
        return;
    }
    for (size_t i = 0; i < PIECES; i++) {
        state_t *const st = &s_state[i];
        memset(st, 0, sizeof(*st));
        const char *built = k_pieces[i].build
                                ? ag_port_place_built(k_pieces[i].name)
                                : NULL;
        if (built != NULL &&
            ag_place_parse(built, k_pieces[i].allowed, &st->built, NULL, 0) ==
                AG_OK) {
            st->built_known = true;
        }
        const char *d = k_pieces[i].build ? built : k_pieces[i].dflt;
        if (d == NULL || ag_place_parse(d, k_pieces[i].allowed, &st->want,
                                        NULL, 0) != AG_OK) {
            (void)ag_place_parse("iram", K_IRAM, &st->want, NULL, 0);
        }
    }
    s_ready = true;
}

static bool have_psram(void) { return ag_port_mem_total(AG_MEM_SLOW) > 0u; }

void ag_place_configure(void)
{
    defaults();
    const ag_cfg_t *cfg = ag_sysconfig();

    for (size_t i = 0; i < PIECES; i++) {
        char key[40];
        snprintf(key, sizeof(key), "memory.%s", k_pieces[i].name);
        const char *text = ag_cfg_get(cfg, key, NULL);
        if (text == NULL) {
            continue;
        }
        ag_place_policy_t p;
        char              why[80];
        if (ag_place_parse(text, k_pieces[i].allowed, &p, why, sizeof(why)) !=
            AG_OK) {
            ag_log(AG_LOG_WARN, TAG, "%s = %s: %s; keeping the default",
                   k_pieces[i].name, text, why);
            continue;
        }
        /* `only psram` on a board that has none can never be met: say so at
         * boot, not at the first allocation. */
        if (p.only && !have_psram() && !k_pieces[i].build) {
            bool any = false;
            for (uint8_t k = 0; k < p.count; k++) {
                any |= p.kind[k] != AG_PLACE_PSRAM;
            }
            if (!any) {
                ag_log(AG_LOG_ERROR, TAG,
                       "%s = %s, and this board has no PSRAM", k_pieces[i].name,
                       text);
            }
        }
        s_state[i].want = p;
        s_state[i].from_config = true;
    }

    /* A misspelt piece is a setting that silently does nothing - say so. */
    for (uint16_t e = 0; e < cfg->count; e++) {
        const ag_cfg_entry_t *en = &cfg->entries[e];
        if (strcmp(en->section, "memory") != 0) {
            continue;
        }
        bool known = find(en->key) >= 0;
        for (size_t o = 0; !known && o < sizeof(k_other_keys) /
                                            sizeof(k_other_keys[0]); o++) {
            known = strcmp(en->key, k_other_keys[o]) == 0;
        }
        if (!known) {
            ag_log(AG_LOG_WARN, TAG, "[memory] %s: no such piece (`mem /p`)",
                   en->key);
        }
    }

    const unsigned bad = ag_place_mismatches();
    if (bad > 0) {
        ag_log(AG_LOG_WARN, TAG,
               "%u piece(s) built elsewhere than [memory] asks - rebuild "
               "(`mem /p`)", bad);
    }
}

const ag_place_policy_t *ag_place_policy(const char *piece)
{
    defaults();
    const int i = find(piece);
    return (i < 0) ? NULL : &s_state[i].want;
}

void ag_place_adopt(const char *piece, void *task)
{
    remember(find(piece), task);
}

void ag_place_note(const char *piece, ag_place_kind_t kind, int32_t bytes)
{
    const int i = find(piece);
    if (i >= 0 && kind > AG_PLACE_NONE && kind < AG_PLACE_KINDS) {
        s_state[i].bytes[kind] += bytes;
    }
}

/* The kinds to try, in order: the policy's own, then - unless `only` - every
 * other kind this piece may live in. */
static uint8_t order(int i, uint8_t *out)
{
    const ag_place_policy_t *p = &s_state[i].want;
    uint8_t                  n = 0;
    for (uint8_t k = 0; k < p->count; k++) {
        out[n++] = p->kind[k];
    }
    if (!p->only) {
        for (uint8_t kind = AG_PLACE_IRAM; kind < AG_PLACE_KINDS; kind++) {
            bool listed = false;
            for (uint8_t k = 0; k < n; k++) {
                listed |= out[k] == kind;
            }
            if (!listed && (k_pieces[i].allowed & AG_PLACE_BIT(kind)) != 0u) {
                out[n++] = kind;
            }
        }
    }
    return n;
}

unsigned ag_place_order(const char *piece, ag_place_kind_t *out)
{
    defaults();
    const int i = find(piece);
    if (i < 0 || out == NULL) {
        return 0;
    }
    uint8_t kinds[AG_PLACE_KINDS];
    const uint8_t n = order(i, kinds);
    for (uint8_t k = 0; k < n; k++) {
        out[k] = (ag_place_kind_t)kinds[k];
    }
    return n;
}

ag_place_kind_t ag_place_first(const char *piece)
{
    defaults();
    const int i = find(piece);
    if (i < 0) {
        return AG_PLACE_IRAM;
    }
    uint8_t kinds[AG_PLACE_KINDS];
    const uint8_t n = order(i, kinds);
    for (uint8_t k = 0; k < n; k++) {
        if (kinds[k] == AG_PLACE_PSRAM && !have_psram()) {
            continue;
        }
        return (ag_place_kind_t)kinds[k];
    }
    return AG_PLACE_NONE;
}

static void went_elsewhere(int i, uint8_t got, size_t bytes)
{
    if (!s_state[i].warned && got != s_state[i].want.kind[0]) {
        s_state[i].warned = true;
        ag_log(AG_LOG_INFO, TAG, "%s: %u bytes went to %s, not %s",
               k_pieces[i].name, (unsigned)bytes,
               ag_place_kind_name((ag_place_kind_t)got),
               ag_place_kind_name((ag_place_kind_t)s_state[i].want.kind[0]));
    }
}

static unsigned caps_of(uint8_t kind)
{
    return (kind == AG_PLACE_PSRAM) ? (AG_MEM_SLOW | AG_MEM_BYTE)
                                    : (AG_MEM_FAST | AG_MEM_BYTE);
}

void *ag_place_alloc(const char *piece, size_t bytes)
{
    defaults();
    const int i = find(piece);
    if (i < 0) {
        return ag_port_alloc(bytes, AG_MEM_FAST | AG_MEM_BYTE);
    }
    uint8_t kinds[AG_PLACE_KINDS];
    const uint8_t n = order(i, kinds);
    for (uint8_t k = 0; k < n; k++) {
        if (kinds[k] != AG_PLACE_IRAM && kinds[k] != AG_PLACE_PSRAM) {
            continue;
        }
        void *p = ag_port_alloc(bytes, caps_of(kinds[k]));
        if (p != NULL) {
            s_state[i].bytes[kinds[k]] += (int32_t)ag_port_alloc_size(p);
            went_elsewhere(i, kinds[k], bytes);
            return p;
        }
    }
    ag_log(AG_LOG_ERROR, TAG, "%s: no room for %u bytes", piece,
           (unsigned)bytes);
    return NULL;
}

void ag_place_free(const char *piece, void *p)
{
    if (p == NULL) {
        return;
    }
    const int i = find(piece);
    if (i >= 0) {
        const uint8_t kind = ag_port_ptr_is_slow(p) ? AG_PLACE_PSRAM
                                                    : AG_PLACE_IRAM;
        s_state[i].bytes[kind] -= (int32_t)ag_port_alloc_size(p);
    }
    ag_port_free(p);
}

bool ag_place_task(const char *piece, void (*fn)(void *), const char *name,
                   uint32_t stack_bytes, void *arg, unsigned prio, int core,
                   void **out_task, ag_place_kind_t *got)
{
    defaults();
    const int i = find(piece);
    uint8_t   kinds[AG_PLACE_KINDS];
    uint8_t   n = 0;
    if (i >= 0) {
        n = order(i, kinds);
    } else {
        kinds[n++] = AG_PLACE_IRAM;
    }
    for (uint8_t k = 0; k < n; k++) {
        if (kinds[k] != AG_PLACE_IRAM && kinds[k] != AG_PLACE_PSRAM) {
            continue;
        }
        if (kinds[k] == AG_PLACE_PSRAM && !AG_PORT_TASK_STACK_CAPS) {
            continue; /* this build cannot put a stack there */
        }
        /* The handle is written by the scheduler before the task can run - a
         * task that asks "am I the reader?" first thing must find it set. */
        ag_port_task_t  local = NULL;
        ag_port_task_t *t = (out_task != NULL) ? (ag_port_task_t *)out_task
                                               : &local;
        const unsigned caps =
            AG_PORT_TASK_STACK_CAPS ? caps_of(kinds[k]) : 0u;
        if (ag_port_task_create((ag_port_task_fn)fn, name, stack_bytes, arg,
                                prio, core, caps, t)) {
            if (i >= 0) {
                remember(i, (void *)*t);
                went_elsewhere(i, kinds[k], stack_bytes);
            }
            if (got != NULL) {
                *got = (ag_place_kind_t)kinds[k];
            }
            return true;
        }
    }
    ag_log(AG_LOG_ERROR, TAG, "%s: no room for %s's %u-byte stack", piece,
           name, (unsigned)stack_bytes);
    return false;
}

static bool mismatch(size_t i)
{
    const state_t *st = &s_state[i];
    if (!k_pieces[i].build || !st->from_config || !st->built_known) {
        return false;
    }
    /* Built in the first kind the configuration names - or, without `only`,
     * in a later one because the first cannot exist on this board (psram
     * asked of a board with none). */
    const uint8_t built = st->built.kind[0];
    if (built == st->want.kind[0]) {
        return false;
    }
    if (st->want.kind[0] == AG_PLACE_PSRAM && !have_psram()) {
        for (uint8_t k = 1; k < st->want.count; k++) {
            if (st->want.kind[k] == built) {
                return false;
            }
        }
    }
    return true;
}

unsigned ag_place_mismatches(void)
{
    defaults();
    unsigned n = 0;
    for (size_t i = 0; i < PIECES; i++) {
        n += mismatch(i) ? 1u : 0u;
    }
    return n;
}

void ag_place_report(void (*out)(const char *line, void *ctx), void *ctx)
{
    defaults();
    char line[120];

    /* The tasks' stacks, now. */
    int32_t live[PIECES][AG_PLACE_KINDS];
    memset(live, 0, sizeof(live));
    ag_port_task_info_t *t = (ag_port_task_info_t *)ag_port_alloc(
        sizeof(*t) * PLACE_TASKS, AG_MEM_BYTE);
    if (t != NULL) {
        uint32_t n = ag_port_task_list(t, PLACE_TASKS);
        if (n > PLACE_TASKS) {
            n = PLACE_TASKS;
        }
        for (int k = 0; k < PLACE_TASKS; k++) {
            if (s_tasks[k].task == NULL) {
                continue;
            }
            bool alive = false;
            for (uint32_t j = 0; j < n; j++) {
                if ((void *)t[j].task == s_tasks[k].task) {
                    const int kind = t[j].stack_slow ? AG_PLACE_PSRAM
                                                     : AG_PLACE_IRAM;
                    live[s_tasks[k].piece][kind] += (int32_t)t[j].stack_bytes;
                    alive = true;
                    break;
                }
            }
            if (!alive) {
                s_tasks[k].task = NULL; /* ended: forget it */
            }
        }
        ag_port_free(t);
    }

    out(" piece          asked                 now\n", ctx);
    for (size_t i = 0; i < PIECES; i++) {
        const state_t *st = &s_state[i];
        char           want[32];
        char           now[48];
        ag_place_format(&st->want, want, sizeof(want));
        if (!st->from_config) {
            snprintf(want + strlen(want), sizeof(want) - strlen(want), "%s",
                     k_pieces[i].build ? "" : " (default)");
        }
        now[0] = '\0';
        if (k_pieces[i].build) {
            if (st->built_known) {
                snprintf(now, sizeof(now), "built: %s%s",
                         ag_place_kind_name((ag_place_kind_t)st->built.kind[0]),
                         mismatch(i) ? "  <- rebuild" : "");
            } else {
                snprintf(now, sizeof(now), "not in this build");
            }
        } else {
            size_t len = 0;
            for (int k = AG_PLACE_IRAM; k < AG_PLACE_KINDS; k++) {
                const int32_t b = st->bytes[k] + live[i][k];
                if (b > 0 && len < sizeof(now)) {
                    len += (size_t)snprintf(
                        now + len, sizeof(now) - len, "%s%u KB %s",
                        (len > 0) ? ", " : "",
                        (unsigned)((b + 1023) / 1024),
                        ag_place_kind_name((ag_place_kind_t)k));
                }
            }
            if (len == 0) {
                snprintf(now, sizeof(now), "-");
            }
        }
        snprintf(line, sizeof(line), " %-14s %-21s %s\n", k_pieces[i].name,
                 want, now);
        out(line, ctx);
    }
}
