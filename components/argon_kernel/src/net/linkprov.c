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

#include <argon/port/task.h>

static const ag_link_ops_t *s_ops;
static ag_device_t         *s_dev;
static char                 s_name[AG_DEV_NAME_MAX] = "none";

/*
 * Channels, and who is listening on each.  Sixteen is the link's own limit; the
 * table is static because registering happens while a driver is being bound,
 * which is not a moment to be asking for memory.
 */
#define LINK_CHANS 16u
static ag_link_rx_fn s_rx_fn[LINK_CHANS];
static void         *s_rx_ctx[LINK_CHANS];

static ag_port_task_t s_reader;
static volatile bool  s_reader_stop;

ag_err_t ag_linkprov_listen(uint8_t chan, ag_link_rx_fn fn, void *ctx)
{
    if (chan >= LINK_CHANS || fn == NULL) {
        return -AG_EINVAL;
    }
    if (s_rx_fn[chan] != NULL && s_rx_fn[chan] != fn) {
        /* Two consumers of one channel is the bug this design exists to
         * prevent, so it is refused rather than silently chained. */
        return -AG_EBUSY;
    }
    s_rx_fn[chan] = fn;
    s_rx_ctx[chan] = ctx;
    return AG_OK;
}

void ag_linkprov_unlisten(uint8_t chan)
{
    if (chan < LINK_CHANS) {
        s_rx_fn[chan] = NULL;
        s_rx_ctx[chan] = NULL;
    }
}

bool ag_linkprov_reading(void) { return s_reader != NULL; }

/*
 * Am I the reader?  A different question from the one above, and the two are
 * easy to mistake for each other: anything that sends a request and then waits
 * for the answer must refuse to run on this task, because the answer arrives
 * on it.  Asking "is there a reader" instead refuses ALWAYS - which is what the
 * remote drive did, and it failed as -EBUSY, a plausible-looking error.
 */
bool ag_linkprov_in_reader(void)
{
    return s_reader != NULL && ag_port_task_self() == s_reader;
}

/*
 * The one reader.  Everything that arrives goes to the handler for its channel
 * and nowhere else; a message on a channel nobody holds is dropped, which is
 * the honest thing to do with a conversation nobody is having.
 */
static void reader_task(void *arg)
{
    (void)arg;
    uint8_t buf[288];
    while (!s_reader_stop) {
        if (s_ops == NULL) {
            ag_port_task_delay(ag_port_ms_to_ticks(20));
            continue;
        }
        uint8_t       chan = 0;
        const int32_t n = s_ops->recv(s_dev, &chan, buf, sizeof(buf), 50);
        if (n <= 0) {
            continue;
        }
        if (chan < LINK_CHANS && s_rx_fn[chan] != NULL) {
            s_rx_fn[chan](chan, buf, (uint16_t)n, s_rx_ctx[chan]);
        }
    }
    s_reader = NULL;
    ag_port_task_delete(NULL);
}

static void reader_start(void)
{
    if (s_reader != NULL) {
        return;
    }
    s_reader_stop = false;
    /* Priority 1: below every application, for the reason this tree keeps
     * repeating - a link's task above them took a game from 40 fps to
     * unplayable. */
    if (!ag_port_task_create(reader_task, "ag_link", 4096, NULL, 1, 0, 0,
                             &s_reader)) {
        ag_log(AG_LOG_ERROR, "link", "no reader task: nothing will be heard");
    }
}

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

    reader_start();

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
