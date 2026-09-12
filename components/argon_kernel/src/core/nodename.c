/*
 * ArgonOS - what this node is called.  See argon/nodename.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/nodename.h>

#include <string.h>

#include <argon/cfg.h>
#include <argon/libc.h>
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
static char s_peer[AG_NODE_NAME_MAX];

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
    s_peer[0] = '\0';
    return AG_OK;
}

const char *ag_node_peer_name(void)
{
    return (s_peer[0] != '\0') ? s_peer : NULL;
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
    ag_strlcpy(s_peer, name, sizeof(s_peer));
}
