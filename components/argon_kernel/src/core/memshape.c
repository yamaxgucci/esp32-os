/*
 * ArgonOS - buffers that can be made again elsewhere (argon/memshape.h).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/memshape.h>

#include <stddef.h>

#include <argon/log.h>
#include <argon/port/mem.h>
#include <argon/port/sync.h>

#define RESHAPERS 8

typedef struct {
    const char   *name;
    ag_reshape_fn fn;
    void         *ctx;
} reshaper_t;

static reshaper_t       s_list[RESHAPERS];
static int              s_n;
static ag_port_mutex_t  s_lock;   /* one reshape at a time */

bool ag_mem_reshaper_add(const char *name, ag_reshape_fn fn, void *ctx)
{
    if (fn == NULL || s_n == RESHAPERS) {
        return false;
    }
    for (int i = 0; i < s_n; i++) {
        if (s_list[i].fn == fn && s_list[i].ctx == ctx) {
            return true;
        }
    }
    s_list[s_n].name = name;
    s_list[s_n].fn = fn;
    s_list[s_n].ctx = ctx;
    s_n++;
    return true;
}

long ag_mem_reshape(const char *why)
{
    if (s_n == 0) {
        return 0;
    }
    if (s_lock == NULL) {
        s_lock = ag_port_mutex_new();
        if (s_lock == NULL) {
            return 0;
        }
    }
    ag_port_mutex_take(s_lock, AG_PORT_FOREVER);
    const size_t before = ag_port_mem_largest(AG_MEM_FAST | AG_MEM_BYTE);
    const int n = s_n;
    for (int i = 0; i < n; i++) {
        s_list[i].fn(s_list[i].ctx);
    }
    const size_t after = ag_port_mem_largest(AG_MEM_FAST | AG_MEM_BYTE);
    ag_port_mutex_give(s_lock);
    const long gain = (long)after - (long)before;
    ag_log(AG_LOG_INFO, "mem", "reshape (%s): largest %u -> %u KB, %d asked",
           why != NULL ? why : "asked", (unsigned)(before / 1024u),
           (unsigned)(after / 1024u), n);
    return gain;
}
