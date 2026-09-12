/*
 * ArgonOS - a task a loadable driver may own.
 *
 * WHY IT EXISTS.  api->task->create refuses a caller with no current process,
 * and the reason is sound: a thread has to belong to something that can be made
 * to end, and until now a driver was not that.  So every loadable driver polls
 * from whatever callback the kernel happens to make - which is right for a
 * panel that is asked to blit, and wrong for anything whose job is to WATCH.
 *
 *   REMDISP says so in its own source: a receiver that reconnects while an
 *   application holds a still picture stays dark, because nothing calls the
 *   driver, and the workaround it proposes is a fake input device polled ten
 *   times a second.
 *
 *   The node link cannot use that workaround at all: ten polls a second is a
 *   ceiling of sixty kilobytes, and a node that must hear an unsolicited
 *   request has to be listening, not waiting to be asked.
 *
 * A service task belongs to the MODULE, which is the thing api->task->create
 * could not point at.  The kernel owns it, stops it when the module is
 * unloaded, and waits for it to actually leave before the image it is running
 * from is given back.
 *
 * PRIORITY IS NOT THE CALLER'S TO CHOOSE, and that is deliberate.  Applications
 * run at 2, 5 and 8; a service task runs at 1, below all of them.  A driver
 * task above applications has cost this project a game: the wire's own task sat
 * there, never blocked, and the frame rate went from 40 to unplayable until it
 * was moved down.  Anything that genuinely needs to run above applications
 * wants the thread budget (ag_thread_budget), not this.
 *
 * THE STOP PROTOCOL is cooperative, and it has to be: the task is executing
 * code inside the module's image, so the image cannot be freed under it.  The
 * kernel sets a flag, the task sees it through ag_svc_stopping() and returns,
 * and only then is the image unmapped.  A task that does not return is not
 * killed - it is complained about, and its module stays mapped for ever, which
 * is a leak and is enormously better than executing memory that has been given
 * back.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_SERVICE_H
#define ARGON_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

/* How long the kernel waits for a service task to notice it should go. */
#define AG_SVC_STOP_MS 2000u

/*
 * Start one.  Only valid from inside ag_driver_init, where the loading module
 * is known - the same rule ag_dev_add and ag_module_on_unload follow, and for
 * the same reason: outside it there is nothing to own the task.
 *
 * Returns NULL when there is no room, the stack cannot be had, or the caller is
 * not a module being loaded.
 */
ag_svc_t ag_svc_start(void (*fn)(void *), void *arg, const char *name,
                      size_t stack, uint32_t flags);

/* True once the kernel has asked this task to finish.  Check it in the loop;
 * a task that does not is a module that can never be unloaded. */
bool ag_svc_stopping(ag_svc_t s);

/*
 * Ask every service task belonging to `owner` to stop, and wait for them.
 * Returns false when one did not go, which means the image must not be freed.
 * Called by the module loader; nothing else should.
 */
bool ag_svc_stop_owner(const void *owner);

/* For `ps`-like reporting later; also how the tests see that one exists. */
uint32_t ag_svc_count(void);

#endif /* ARGON_SERVICE_H */
