/*
 * ArgonOS - what this node is called.  See argon/nodename.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/nodename.h>

#include <string.h>

#include <argon/cfg.h>
#include <argon/libc.h>
#include <argon/linkprov.h>
#include <argon/log.h>
#include "core/sysconfig.h"

#define TAG "node"

/*
 * The default, and why it is a word rather than something derived.
 *
 * A name taken from the chip would be unique without anyone configuring
 * anything, which is the right answer - but there is no chip identifier in the
 * port contract today, and adding one to every port for a default is a bigger
 * change than the thing it improves.  So the default is the same on every
 * board, and the COLLISION IS DETECTED AND SAID OUT LOUD when two of them meet.
 * That is the honest version of not having derived it: the bad case is noisy
 * rather than silent, and one command fixes it.
 */
static char s_name[AG_NODE_NAME_MAX] = "node";

/*
 * ONE NEIGHBOUR PER WIRE, not one neighbour.
 *
 * What a far board says about itself - its name, its cores, its clock - is
 * remembered against the wire it said it on.  With one wire that is exactly
 * what it used to be; with three it is the difference between a cluster and a
 * muddle, because the last board to say hello would otherwise overwrite the
 * other two and every one of them would answer to the same facts.
 *
 * Which wire a call means is not an argument here either: the hello arrives on
 * its wire's reader task, so ag_linkprov_current() names it.  The unindexed
 * calls below therefore keep meaning what they always did - "the neighbour of
 * the wire in hand" - and only something enumerating the whole cluster has to
 * say which.
 */
static char     s_peer[AG_LINK_WIRES][AG_NODE_NAME_MAX];
static uint8_t  s_peer_cores_at[AG_LINK_WIRES];
static uint32_t s_peer_cpu_hz_at[AG_LINK_WIRES];

/* The slot this call means: the wire in hand, and 0 when there is no wire at
 * all - so a board that never binds one still has somewhere to put what it
 * hears, which is what the host tests do. */
static uint32_t peer_slot(void)
{
    const int32_t w = ag_linkprov_current();
    return (w >= 0 && (uint32_t)w < AG_LINK_WIRES) ? (uint32_t)w : 0u;
}

/* A name goes into device names and paths, so keep it to what cannot confuse
 * either: letters and digits, no dot (that is the separator in `n2.pcm0`). */
static bool name_ok(const char *s)
{
    if (s == NULL || s[0] == '\0') {
        return false;
    }
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        const char c = s[n];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return n < AG_NODE_NAME_MAX;
}

const char *ag_node_name(void)
{
    return s_name;
}

ag_err_t ag_node_name_init(void)
{
    const ag_cfg_t *cfg = ag_sysconfig();
    if (cfg == NULL) {
        return AG_OK; /* no config is not an error; the default stands */
    }
    const char *want = ag_cfg_get(cfg, "cluster.name", NULL);
    if (want == NULL) {
        return AG_OK;
    }
    if (!name_ok(want)) {
        ag_log(AG_LOG_WARN, TAG,
               "[cluster] name '%s' is not usable; staying '%s'", want, s_name);
        return AG_OK;
    }
    ag_strlcpy(s_name, want, sizeof(s_name));
    return AG_OK;
}

ag_err_t ag_node_name_set(const char *name)
{
    if (!name_ok(name)) {
        return -AG_EINVAL;
    }
    ag_strlcpy(s_name, name, sizeof(s_name));
    /*
     * A name that changed makes what the far end was told stale, and a stale
     * peer name is worse than none: it addresses a node that no longer answers
     * to it.  The link says hello again and fills this back in.
     */
    for (uint32_t i = 0; i < AG_LINK_WIRES; i++) {
        s_peer[i][0] = '\0';
    }
    return AG_OK;
}

const char *ag_node_peer_name(void)
{
    return ag_node_peer_name_at((int32_t)peer_slot());
}

const char *ag_node_peer_name_at(int32_t wire)
{
    if (wire < 0 || (uint32_t)wire >= AG_LINK_WIRES) {
        return NULL;
    }
    return (s_peer[wire][0] != '\0') ? s_peer[wire] : NULL;
}

uint8_t ag_node_peer_cores_at(int32_t wire)
{
    if (wire < 0 || (uint32_t)wire >= AG_LINK_WIRES) {
        return 0;
    }
    return s_peer_cores_at[wire];
}

uint32_t ag_node_peer_cpu_hz_at(int32_t wire)
{
    if (wire < 0 || (uint32_t)wire >= AG_LINK_WIRES) {
        return 0;
    }
    return s_peer_cpu_hz_at[wire];
}

void ag_node_set_peer_name(const char *name)
{
    if (!name_ok(name)) {
        return;
    }
    /*
     * Two nodes with one name is not a cosmetic problem: every qualified name
     * (`n2.pcm0`) and every listing that says where something is becomes
     * ambiguous, and nothing downstream can detect it.  Said loudly, once, at
     * the moment the two meet - which is the only moment anyone can act on it.
     */
    if (strcmp(name, s_name) == 0) {
        ag_log(AG_LOG_ERROR, TAG,
               "the other node is also called '%s' - name one of them "
               "(`node name <x>`, or [cluster] name in SYSTEM.CFG)",
               name);
    }
    const uint32_t slot = peer_slot();
    ag_strlcpy(s_peer[slot], name, sizeof(s_peer[slot]));
}

/*
 * What the other board is made of, as it said in its hello.
 *
 * Here rather than in the link because it is the same kind of fact as the name:
 * something the far node states about itself, remembered until it says
 * otherwise.  A job that wants to use every core of every board cannot even
 * decide how many threads to start without it, and guessing "probably two"
 * would be wrong on the C6 the day it joins.
 *
 * Zero cores means it has not said - an older image, or nothing heard yet - and
 * the caller should read that as one, not as none.
 */
void ag_node_set_peer_caps(uint8_t cores, uint32_t cpu_hz)
{
    const uint32_t slot = peer_slot();
    if (cores > 0) {
        s_peer_cores_at[slot] = cores;
    }
    if (cpu_hz > 0) {
        s_peer_cpu_hz_at[slot] = cpu_hz;
    }
}

uint8_t  ag_node_peer_cores(void)
{
    return s_peer_cores_at[peer_slot()];
}

uint32_t ag_node_peer_cpu_hz(void)
{
    return s_peer_cpu_hz_at[peer_slot()];
}
