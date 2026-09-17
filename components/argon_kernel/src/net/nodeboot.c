/*
 * ArgonOS - joining a cluster.  See argon/nodeboot.h for the two settings and
 * why neither of them is a role.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/nodeboot.h>

#include <string.h>

#include <argon/cfg.h>
#include <argon/console.h>
#include <argon/device.h>
#include <argon/libc.h>
#include <argon/linkprov.h>
#include <argon/log.h>
#include <argon/nodeclu.h>
#include <argon/nodedev.h>
#include <argon/nodedrv.h>
#include <argon/nodefs.h>
#include <argon/nodehello.h>
#include <argon/nodeproc.h>
#include <argon/nodeps.h>
#include <argon/noderdev.h>
#include <argon/nodestandby.h>
#include <argon/path.h>

#include "core/sysconfig.h"

#define TAG "cluster"

bool ag_cluster_allowed(void)
{
    const ag_cfg_t *cfg = ag_sysconfig();
    if (cfg == NULL) {
        return true;
    }
    const char *v = ag_cfg_get(cfg, "cluster.multiboard", NULL);
    if (v == NULL) {
        return true;
    }
    /*
     * "none" is the word Maxim chose and the one the setting is documented
     * with; "no" and "off" are accepted beside it because a person who types
     * a setting from memory types the word that means no in their head, and
     * refusing that spelling would silently leave the board in a cluster.
     */
    return !(ag_path_icmp(v, "none") == 0 || ag_path_icmp(v, "no") == 0 ||
             ag_path_icmp(v, "off") == 0 || ag_path_icmp(v, "0") == 0);
}

/*
 * Everything that answers a neighbour, started in one place.
 *
 * It was seven calls inlined in `link use`, which was fine while that was the
 * only way a wire was ever bound.  The boot-time bind is a second way, and two
 * copies of a list like this drift the first time a service is added to one of
 * them - so there is one copy and both callers use it.
 */
static void start_services(bool say)
{
    struct {
        const char *what;
        ag_err_t (*start)(void);
    } const svc[] = {
        { "not serving remote runs", ag_nodeproc_start },
        { "not answering device listings", ag_nodedev_start },
        { "not answering file fetches", ag_nodefs_start },
        /*
         * Answer file requests from the moment a link exists, without waiting
         * to be asked: the other node may mount us before we mount it, and a
         * drive that appears only after both sides have run a command is a
         * drive nobody can rely on.
         */
        { "not serving our card", ag_nodedrv_serve },
        /* Names, before anything that has to be addressed by one. */
        { "did not say hello", ag_nodehello_start },
        { "not serving our devices", ag_noderdev_serve },
        { "not answering `ps`", ag_nodeps_serve },
        /*
         * Taking threads of a job, from the moment there is a wire: a node
         * that has not opened this channel refuses the first job it is
         * offered - and the neighbour has no way to tell that from "this
         * board cannot do it".
         */
        { "not taking threads of a job", ag_nodeclu_start },
    };

    for (unsigned i = 0; i < sizeof(svc) / sizeof(svc[0]); i++) {
        const ag_err_t e = svc[i].start();
        if (e == AG_OK) {
            continue;
        }
        if (say) {
            ag_console_printf("  (%s: %d)\n", svc[i].what, (int)e);
        } else {
            ag_log(AG_LOG_WARN, TAG, "%s: %d", svc[i].what, (int)e);
        }
    }

    /* The standby watcher, which does nothing at all unless SYSTEM.CFG asked
     * for it - so it is not in the list above, where every entry that returns
     * anything but AG_OK is worth a line. */
    (void)ag_standby_start();
}

ag_err_t ag_node_bind(const char *dev, bool say)
{
    if (!ag_cluster_allowed()) {
        return -AG_EPERM;
    }
    const ag_err_t e = ag_linkprov_use(dev);
    if (e != AG_OK) {
        return e;
    }
    start_services(say);
    return AG_OK;
}

void ag_node_autobind(void)
{
    const ag_cfg_t *cfg = ag_sysconfig();
    if (cfg == NULL) {
        return;
    }
    const char *want = ag_cfg_get(cfg, "link.bind", NULL);
    if (want == NULL || want[0] == '\0') {
        /*
         * Said, not returned in silence.  "No wire was asked for" and "the
         * wire I asked for did not bind" look identical from the prompt - the
         * board simply has no link - and the first one cost an afternoon of
         * looking at the second.
         */
        ag_log(AG_LOG_INFO, TAG,
               "no [link] bind in SYSTEM.CFG; not joining a cluster by itself");
        return;
    }
    if (!ag_cluster_allowed()) {
        ag_log(AG_LOG_INFO, TAG,
               "[link] bind names %s, but [cluster] multiboard = none - "
               "staying out of any cluster",
               want);
        return;
    }

    /*
     * `auto` means the only link device there is.  Only the ONLY one: with two
     * of them there is a right answer and a wrong one, and a board that picked
     * for itself would be wrong half the time and silent about it.
     */
    char chosen[AG_DEV_NAME_MAX];
    if (ag_path_icmp(want, "auto") == 0) {
        uint32_t found = 0;
        for (uint32_t i = 0;; i++) {
            ag_devinfo_t di;
            if (ag_dev_info(i, AG_DEV_LINK, &di) != AG_OK) {
                break;
            }
            found++;
            if (found == 1u) {
                ag_strlcpy(chosen, di.name, sizeof(chosen));
            }
        }
        if (found == 0u) {
            ag_log(AG_LOG_WARN, TAG,
                   "[link] bind = auto, but no link device is loaded - is the "
                   "wire driver in [modules]?");
            return;
        }
        if (found > 1u) {
            ag_log(AG_LOG_WARN, TAG,
                   "[link] bind = auto, but this board has %u link devices - "
                   "name the one you mean",
                   (unsigned)found);
            return;
        }
    } else {
        ag_strlcpy(chosen, want, sizeof(chosen));
    }

    const ag_err_t e = ag_node_bind(chosen, false);
    if (e != AG_OK) {
        ag_log(AG_LOG_WARN, TAG, "could not bind %s at boot: %d", chosen,
               (int)e);
    } else {
        ag_log(AG_LOG_INFO, TAG, "%s bound at boot, as [link] bind asked",
               chosen);
    }
}
