/*
 * ArgonOS - the node link indirection.  See argon/linkprov.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/linkprov.h>

#include <stddef.h>
#include <string.h>

#include <argon/device.h>
#include <argon/log.h>

static const ag_link_ops_t *s_ops;
static ag_device_t         *s_dev;
static char                 s_name[AG_DEV_NAME_MAX] = "none";

/*
 * Every entry the kernel dials must be present.  The size test is against the
 * whole table because there is nothing optional in it yet - but it is written
 * as an offset rather than a sizeof on purpose, so that the first time this
 * vtable grows, a driver built before the growth still binds.  The net provider
 * next door learned that the expensive way.
 */
#define LINK_OPS_MIN                                                          \
    (offsetof(ag_link_ops_t, stats) + sizeof(((ag_link_ops_t *)0)->stats))

static bool ops_complete(const ag_link_ops_t *o)
{
    return o != NULL && o->size >= LINK_OPS_MIN && o->start && o->up &&
           o->send && o->recv && o->poll && o->stats;
}

const char *ag_linkprov_active(void) { return s_name; }
bool        ag_linkprov_bound(void) { return s_ops != NULL; }

/*
 * The device went away - its driver was unloaded, or the hardware did.  Let go
 * while the pointer still means something: a moment later it is an address in
 * an image that has been given back, and a link that answers out of freed
 * memory is worse than one that says it is not there.
 *
 * Called with the registry lock held, so nothing here may reach back into it.
 */
static void on_revoke(ag_device_t *dev)
{
    if (dev != NULL && dev == s_dev) {
        s_ops = NULL;
        s_dev = NULL;
        strcpy(s_name, "none");
    }
}

ag_err_t ag_linkprov_use(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return -AG_EINVAL;
    }
    /* Registered on the first bind rather than at boot: a board that never
     * binds a link never needs the watcher, and on_revoke is idempotent. */
    (void)ag_dev_on_revoke(on_revoke);

    if (strcmp(name, "none") == 0) {
        s_ops = NULL;
        s_dev = NULL;
        strcpy(s_name, "none");
        return AG_OK;
    }

    ag_device_t *dev = ag_dev_find(name);
    if (dev == NULL) {
        return -AG_ENODEV;
    }
    if (dev->cls != AG_DEV_LINK) {
        ag_log(AG_LOG_ERROR, "link", "%s is not a link device", name);
        return -AG_EINVAL;
    }
    const ag_link_ops_t *ops = (const ag_link_ops_t *)dev->class_ops;
    if (!ops_complete(ops)) {
        ag_log(AG_LOG_ERROR, "link", "%s: incomplete link ops", name);
        return -AG_ENOTSUP;
    }

    s_ops = ops;
    s_dev = dev;
    strncpy(s_name, dev->name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';

    const ag_err_t e = s_ops->start(s_dev);
    if (e != AG_OK) {
        ag_log(AG_LOG_ERROR, "link", "%s did not start: %d", s_name, (int)e);
        s_ops = NULL;
        s_dev = NULL;
        strcpy(s_name, "none");
        return e;
    }
    return AG_OK;
}

bool ag_linkprov_up(void)
{
    return (s_ops != NULL) && s_ops->up(s_dev);
}

int32_t ag_linkprov_send(uint8_t chan, uint8_t cls, const void *buf, size_t len)
{
    if (s_ops == NULL) {
        return -AG_ENODEV;
    }
    return s_ops->send(s_dev, chan, cls, buf, len);
}

int32_t ag_linkprov_recv(uint8_t *chan, void *buf, size_t max,
                         uint32_t timeout_ms)
{
    if (s_ops == NULL) {
        return -AG_ENODEV;
    }
    return s_ops->recv(s_dev, chan, buf, max, timeout_ms);
}

void ag_linkprov_poll(uint32_t ms)
{
    if (s_ops != NULL) {
        s_ops->poll(s_dev, ms);
    }
}

ag_err_t ag_linkprov_stats(ag_link_stats_t *out)
{
    if (s_ops == NULL) {
        return -AG_ENODEV;
    }
    return s_ops->stats(s_dev, out);
}
