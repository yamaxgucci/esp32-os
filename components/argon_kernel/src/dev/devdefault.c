/*
 * ArgonOS - which device a class uses when nobody named one.
 *
 * A cluster has one default screen, one default sound output, one default
 * network interface, however many boards it is made of.  Something has to
 * decide which, and until now the decision was a list of names written inside
 * the mixer in the expectation that there would only ever be one board.
 *
 * The knowledge is split on purpose, and the split is the whole design:
 *
 *   the CLASS knows which of its devices is better.  That a real converter
 *   beats the null one is a fact about sound, not about registries, and it
 *   cannot be derived from anything the registry holds.
 *
 *   this file knows where to look.  Here first, then on the node next door,
 *   under the qualified name - and an operator's choice before either.
 *
 * The order between those two is the part that is easy to get backwards.
 * Quality outranks distance: a real output on the other board beats a local
 * device that swallows sound, because "it plays, slowly" and "it does not play"
 * are not two points on one scale.  So the preference list is walked
 * outermost, and each entry is tried locally before remotely - which makes
 * distance the tie-break it should be, rather than the question.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/devdefault.h>

#include <stdio.h>
#include <string.h>

#include <argon/cfg.h>
#include <argon/device.h>
#include <argon/linkprov.h>
#include <argon/log.h>
#include <argon/nodename.h>

#include "core/sysconfig.h"

#define TAG "devdef"

/*
 * The key an operator writes under [cluster].  Named for the class rather than
 * for a device, because the thing being chosen is "what sound comes out of",
 * not "which of these two identical chips".
 */
static const char *cfg_key(ag_dev_class_t cls)
{
    switch (cls) {
    case AG_DEV_AUDIO:
        return "cluster.audio";
    case AG_DEV_DISPLAY:
        return "cluster.display";
    case AG_DEV_NET:
        return "cluster.net";
    case AG_DEV_INPUT:
        return "cluster.input";
    default:
        return NULL;
    }
}

/*
 * What the operator chose at run time, per class that has a default.  Before
 * the config: a line typed now means now, and a setting in a file means "when
 * nobody has said otherwise".
 */
#define DEFAULT_CLASSES 4
static char s_chosen[DEFAULT_CLASSES][AG_DEV_NAME_MAX];

static int chosen_slot(ag_dev_class_t cls)
{
    switch (cls) {
    case AG_DEV_AUDIO:
        return 0;
    case AG_DEV_DISPLAY:
        return 1;
    case AG_DEV_NET:
        return 2;
    case AG_DEV_INPUT:
        return 3;
    default:
        return -1;
    }
}

ag_err_t ag_dev_default_choose(ag_dev_class_t cls, const char *name)
{
    const int slot = chosen_slot(cls);
    if (slot < 0) {
        return -AG_EINVAL;
    }
    if (name == NULL || name[0] == '\0') {
        s_chosen[slot][0] = '\0';
        return AG_OK;
    }
    if (strlen(name) >= sizeof(s_chosen[slot])) {
        return -AG_EINVAL; /* no device could be registered under it */
    }
    strcpy(s_chosen[slot], name);
    return AG_OK;
}

const char *ag_dev_default_chosen(ag_dev_class_t cls)
{
    const int slot = chosen_slot(cls);
    return (slot >= 0 && s_chosen[slot][0] != '\0') ? s_chosen[slot] : NULL;
}

static ag_device_t *named_in_config(ag_dev_class_t cls)
{
    const char *key = cfg_key(cls);
    if (key == NULL) {
        return NULL;
    }
    const ag_cfg_t *cfg = ag_sysconfig();
    if (cfg == NULL) {
        return NULL;
    }
    const char *want = ag_cfg_get(cfg, key, NULL);
    if (want == NULL) {
        return NULL;
    }
    ag_device_t *d = ag_dev_find(want);
    if (d == NULL) {
        /*
         * Said, not silently ignored.  A configured name that does not resolve
         * is usually a node that has not come up yet or a device that was
         * renamed, and the symptom without this line is the system quietly
         * using something else - which looks like the setting being ignored,
         * which is what it is.
         */
        ag_log(AG_LOG_WARN, TAG, "%s = %s: no such device (yet)", key, want);
    }
    return d;
}

ag_device_t *ag_dev_find_anywhere(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    ag_device_t *d = ag_dev_find(name);
    if (d != NULL) {
        return d;
    }
    /* Every neighbour's, in wire order: a board knows nothing about wires,
     * only that the device is on a board next to it. */
    for (int32_t w = 0; w < (int32_t)AG_LINK_WIRES; w++) {
        const char *peer = ag_node_peer_name_at(w);
        if (peer == NULL) {
            continue;
        }
        char      qualified[AG_DEV_NAME_MAX];
        const int n = snprintf(qualified, sizeof(qualified), "%s.%s", peer, name);
        if (n < 0 || (size_t)n >= sizeof(qualified)) {
            continue; /* it could not have been registered under that name */
        }
        d = ag_dev_find(qualified);
        if (d != NULL) {
            return d;
        }
    }
    return NULL;
}

ag_device_t *ag_dev_default(ag_dev_class_t cls, const char *const *prefer)
{
    /*
     * The chosen device, or nothing: a choice that does not resolve is not
     * quietly replaced by another device, because "I said the CYD and it
     * played on the Zero" is the one answer worse than silence.  `dev default`
     * says so when it is set; the mixer says "nowhere to send sound".
     */
    const char *chosen = ag_dev_default_chosen(cls);
    if (chosen != NULL) {
        return ag_dev_find(chosen);
    }
    ag_device_t *d = named_in_config(cls);
    if (d != NULL) {
        return d;
    }
    if (prefer == NULL) {
        return NULL;
    }
    for (int i = 0; prefer[i] != NULL; i++) {
        d = ag_dev_find_anywhere(prefer[i]);
        if (d != NULL) {
            return d;
        }
    }
    return NULL;
}
