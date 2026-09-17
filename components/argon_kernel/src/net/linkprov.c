/*
 * ArgonOS - the node link indirection.  See argon/linkprov.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/linkprov.h>

#include <stddef.h>
#include <string.h>

#include <argon/device.h>
#include <argon/libc.h>
#include <argon/log.h>

#include <argon/port/task.h>

/*
 * SEVERAL WIRES, AND THE ONE THIS CALL MEANS.
 *
 * A board used to have exactly one neighbour, so "the link" was a pointer and
 * every service above simply sent into it.  Three boards need more than that,
 * and the job layer was already written for it - every ag_clu_transport_t call
 * takes a node index - so what was missing was only down here.
 *
 * Which wire a call means is NOT a new argument on nine files' worth of call
 * sites.  It is derived: each wire has its own reader task, a handler runs on
 * the task of the wire its message arrived on, so a reply goes back out THAT
 * wire without anybody saying so.  Everything else - a shell command, a
 * migration, a program starting a job - means the selected wire, the one
 * `link use` last named.
 *
 * That rule is worth stating the other way round, because it is the whole
 * design: an answer travels home the way its question came, and only a new
 * conversation has to choose.
 */
#define LINK_WIRES AG_LINK_WIRES

typedef struct {
    const ag_link_ops_t *ops;
    ag_device_t         *dev;
    char                 name[AG_DEV_NAME_MAX];
    ag_port_task_t       reader;
    volatile bool        reader_stop;
} wire_t;

static wire_t s_wire[LINK_WIRES];
static int    s_sel = -1; /* what `link use` last bound; -1 is none */

static const char s_none[] = "none";

/* The wire a call on this task means: the one it is reading, else the
 * selected one.  Both can be absent, and then there is no wire at all. */
static wire_t *current(void)
{
    const ag_port_task_t self = ag_port_task_self();
    for (unsigned i = 0; i < LINK_WIRES; i++) {
        if (s_wire[i].ops != NULL && s_wire[i].reader != NULL &&
            s_wire[i].reader == self) {
            return &s_wire[i];
        }
    }
    if (s_sel >= 0 && s_wire[s_sel].ops != NULL) {
        return &s_wire[s_sel];
    }
    return NULL;
}

/*
 * Channels, and who is listening on each.  Sixteen is the link's own limit; the
 * table is static because registering happens while a driver is being bound,
 * which is not a moment to be asking for memory.
 */
#define LINK_CHANS 16u
static ag_link_rx_fn s_rx_fn[LINK_CHANS];
static void         *s_rx_ctx[LINK_CHANS];

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

bool ag_linkprov_reading(void)
{
    for (unsigned i = 0; i < LINK_WIRES; i++) {
        if (s_wire[i].reader != NULL) {
            return true;
        }
    }
    return false;
}

/*
 * Am I the reader?  A different question from the one above, and the two are
 * easy to mistake for each other: anything that sends a request and then waits
 * for the answer must refuse to run on this task, because the answer arrives
 * on it.  Asking "is there a reader" instead refuses ALWAYS - which is what the
 * remote drive did, and it failed as -EBUSY, a plausible-looking error.
 */
bool ag_linkprov_in_reader(void)
{
    const ag_port_task_t self = ag_port_task_self();
    for (unsigned i = 0; i < LINK_WIRES; i++) {
        if (s_wire[i].reader != NULL && s_wire[i].reader == self) {
            return true;
        }
    }
    return false;
}

/*
 * The one reader.  Everything that arrives goes to the handler for its channel
 * and nowhere else; a message on a channel nobody holds is dropped, which is
 * the honest thing to do with a conversation nobody is having.
 */
static void reader_task(void *arg)
{
    wire_t *w = (wire_t *)arg;
    uint8_t buf[288];
    while (!w->reader_stop) {
        if (w->ops == NULL) {
            ag_port_task_delay(ag_port_ms_to_ticks(20));
            continue;
        }
        uint8_t       chan = 0;
        const int32_t n = w->ops->recv(w->dev, &chan, buf, sizeof(buf), 50);
        if (n <= 0) {
            continue;
        }
        /*
         * The handler runs HERE, on this wire's own task, and that is what
         * makes a reply go back the way the question came: current() finds
         * this wire by asking which task is running.
         */
        if (chan < LINK_CHANS && s_rx_fn[chan] != NULL) {
            s_rx_fn[chan](chan, buf, (uint16_t)n, s_rx_ctx[chan]);
        }
    }
    w->reader = NULL;
    ag_port_task_delete(NULL);
}

static void reader_start(wire_t *w)
{
    if (w->reader != NULL) {
        return;
    }
    w->reader_stop = false;
    /* Priority 1: below every application, for the reason this tree keeps
     * repeating - a link's task above them took a game from 40 fps to
     * unplayable. */
    /*
     * EIGHT KILOBYTES, AND IT IS NOT A ROUND NUMBER PICKED FOR COMFORT.
     *
     * This task serves the far node's file reads, and a read goes down
     * through the VFS into FAT and the SD driver, which carry sector buffers
     * of their own.  At 4096 that overflowed and took the WHOLE BOARD with
     * it: `type n:\sys\autoexec.bat` on the neighbour rebooted the board
     * being read from, every time, in fifteen seconds of silence followed by
     * "error" at the far end.  Nothing said "stack" - a board that reboots
     * loses the journal entry that would have.
     *
     * It survived for as long as it did because no check ever read a FILE
     * across the wire on real hardware: directory listings stay shallow, and
     * the emulator's flash driver is nothing like as deep as FAT on SD.
     * Migration never worked on the boards for this reason and nobody knew,
     * because the thing that failed was three layers above the fault.
     *
     * `link` prints what is left of this, so the next change that eats into
     * it is visible before it is fatal rather than after.
     */
    if (!ag_port_task_create(reader_task, "ag_link", 8192, w, 1, 0, 0,
                             &w->reader)) {
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

const char *ag_linkprov_active(void)
{
    const wire_t *w = current();
    return (w != NULL) ? w->name : s_none;
}

bool ag_linkprov_bound(void) { return current() != NULL; }

ag_err_t ag_linkprov_drop(const char *name)
{
    const int32_t i = ag_linkprov_index_of(name);
    if (i < 0) {
        return -AG_ENODEV;
    }
    /*
     * The reader is told to stop and the wire is let go now rather than
     * waited for: the task is inside a recv() with its own timeout, and
     * holding the shell until that returns would look like a hang.  It reads
     * ops == NULL on its next lap and ends itself.
     */
    s_wire[i].reader_stop = true;
    s_wire[i].ops = NULL;
    s_wire[i].dev = NULL;
    strcpy(s_wire[i].name, s_none);
    if (s_sel == i) {
        s_sel = -1;
        for (unsigned k = 0; k < LINK_WIRES; k++) {
            if (s_wire[k].ops != NULL) {
                s_sel = (int)k;
                break;
            }
        }
    }
    return AG_OK;
}

uint32_t ag_linkprov_reader_stack_left(int32_t wire)
{
    if (wire < 0 || (unsigned)wire >= LINK_WIRES ||
        s_wire[wire].reader == NULL) {
        return 0;
    }
    return ag_port_task_stack_unused(s_wire[wire].reader);
}

int32_t ag_linkprov_current(void)
{
    const wire_t *w = current();
    if (w == NULL) {
        return -1;
    }
    return (int32_t)(w - &s_wire[0]);
}

int32_t ag_linkprov_count(void)
{
    int32_t n = 0;
    for (unsigned i = 0; i < LINK_WIRES; i++) {
        if (s_wire[i].ops != NULL) {
            n++;
        }
    }
    return n;
}

const char *ag_linkprov_name_at(int32_t wire)
{
    if (wire < 0 || (unsigned)wire >= LINK_WIRES ||
        s_wire[wire].ops == NULL) {
        return NULL;
    }
    return s_wire[wire].name;
}

int32_t ag_linkprov_index_of(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (unsigned i = 0; i < LINK_WIRES; i++) {
        if (s_wire[i].ops != NULL && strcmp(s_wire[i].name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t ag_linkprov_send_on(int32_t wire, uint8_t chan, uint8_t cls,
                            const void *buf, size_t len)
{
    if (wire < 0 || (unsigned)wire >= LINK_WIRES ||
        s_wire[wire].ops == NULL) {
        return -AG_ENODEV;
    }
    return s_wire[wire].ops->send(s_wire[wire].dev, chan, cls, buf, len);
}

void ag_linkprov_poll_all(uint32_t ms)
{
    for (unsigned i = 0; i < LINK_WIRES; i++) {
        if (s_wire[i].ops != NULL) {
            s_wire[i].ops->poll(s_wire[i].dev, ms);
        }
    }
}

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
    if (dev == NULL) {
        return;
    }
    for (unsigned i = 0; i < LINK_WIRES; i++) {
        if (s_wire[i].dev == dev) {
            s_wire[i].ops = NULL;
            s_wire[i].dev = NULL;
            strcpy(s_wire[i].name, s_none);
            if (s_sel == (int)i) {
                s_sel = -1;
            }
        }
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

    /*
     * `link use none` lets go of the SELECTED wire only, not of every wire.
     * On a board with one neighbour that is what it always meant; on a board
     * with three, dropping all of them because one was named would be a
     * surprise, and there is `link drop <name>` for saying it on purpose.
     */
    if (strcmp(name, s_none) == 0) {
        if (s_sel >= 0) {
            s_wire[s_sel].reader_stop = true;
            s_wire[s_sel].ops = NULL;
            s_wire[s_sel].dev = NULL;
            strcpy(s_wire[s_sel].name, s_none);
            s_sel = -1;
        }
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

    /*
     * Already bound is a SELECTION, not a second binding.  Typing `link use
     * node0` twice - which a script does every time it is re-run - must not
     * start a second reader on the same device, and both readers would then
     * take turns eating each other's messages.
     */
    int slot = ag_linkprov_index_of(dev->name);
    if (slot < 0) {
        for (unsigned i = 0; i < LINK_WIRES && slot < 0; i++) {
            if (s_wire[i].ops == NULL) {
                slot = (int)i;
            }
        }
    }
    if (slot < 0) {
        ag_log(AG_LOG_ERROR, "link",
               "%s: this board already holds %u wires; drop one first", name,
               (unsigned)LINK_WIRES);
        return -AG_ENOSPC;
    }

    wire_t *w = &s_wire[slot];
    w->ops = ops;
    w->dev = dev;
    ag_strlcpy(w->name, dev->name, sizeof(w->name));
    s_sel = slot;

    reader_start(w);

    const ag_err_t e = w->ops->start(w->dev);
    if (e != AG_OK) {
        ag_log(AG_LOG_ERROR, "link", "%s did not start: %d", w->name, (int)e);
        w->reader_stop = true;
        w->ops = NULL;
        w->dev = NULL;
        strcpy(w->name, s_none);
        s_sel = -1;
        return e;
    }

    /*
     * Say what this wire is, and whether everything above it fits on it.  Once,
     * here, rather than as a surprise inside a driver later - see
     * AG_LINK_MIN_MTU.
     */
    ag_link_info_t info;
    if (ag_linkprov_info(&info) == AG_OK) {
        ag_log(AG_LOG_INFO, "link", "%s: %s, %u B per message, %u B/s", w->name,
               info.wire, (unsigned)info.mtu, (unsigned)info.bytes_per_sec);
        if (info.mtu < AG_LINK_MIN_MTU) {
            ag_log(AG_LOG_WARN, "link",
                   "%s carries only %u bytes a message; the node services want "
                   "%u and will refuse the requests that do not fit",
                   w->name, (unsigned)info.mtu, (unsigned)AG_LINK_MIN_MTU);
        }
    }
    return AG_OK;
}

bool ag_linkprov_up(void)
{
    const wire_t *w = current();
    return (w != NULL) && w->ops->up(w->dev);
}

int32_t ag_linkprov_send(uint8_t chan, uint8_t cls, const void *buf, size_t len)
{
    wire_t *w = current();
    if (w == NULL) {
        return -AG_ENODEV;
    }
    return w->ops->send(w->dev, chan, cls, buf, len);
}

void ag_linkprov_poll(uint32_t ms)
{
    wire_t *w = current();
    if (w != NULL) {
        w->ops->poll(w->dev, ms);
    }
}

ag_err_t ag_linkprov_stats(ag_link_stats_t *out)
{
    wire_t *w = current();
    if (w == NULL) {
        return -AG_ENODEV;
    }
    return w->ops->stats(w->dev, out);
}

/*
 * What the wire IS.  Answered for every bound link, including one whose driver
 * predates info() - that one gets the UART's own measured numbers, because
 * every driver that could exist before this entry was a UART.
 *
 * The caller therefore never has to handle "I do not know", which matters: the
 * numbers decide message sizes and timeouts, and a caller that had to guess
 * when they were missing would be a second set of constants in a second place,
 * which is exactly the thing this exists to remove.
 */
ag_err_t ag_linkprov_info(ag_link_info_t *out)
{
    if (out == NULL) {
        return -AG_EINVAL;
    }
    wire_t *w = current();
    if (w == NULL) {
        return -AG_ENODEV;
    }

    memset(out, 0, sizeof(*out));
    const size_t need =
        offsetof(ag_link_ops_t, info) + sizeof(((ag_link_ops_t *)0)->info);
    if (w->ops->size >= need && w->ops->info != NULL &&
        w->ops->info(w->dev, out) == AG_OK && out->mtu > 0) {
        if (out->bytes_per_sec == 0) {
            out->bytes_per_sec = AG_LINK_DEFAULT_RATE;
        }
        if (out->rtt_us == 0) {
            out->rtt_us = AG_LINK_DEFAULT_RTT_US;
        }
        return AG_OK;
    }

    out->mtu = AG_LINK_DEFAULT_MTU;
    out->bytes_per_sec = AG_LINK_DEFAULT_RATE;
    out->rtt_us = AG_LINK_DEFAULT_RTT_US;
    strcpy(out->wire, "uart");
    return AG_OK;
}
