/*
 * ArgonOS - service tasks owned by a module.  See argon/service.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/service.h>

#include <string.h>

#include <argon/log.h>
#include <argon/module.h>

#include <argon/port/task.h>
#include <argon/port/time.h>

/*
 * Few, and static.  A service task is a driver's whole reason to be resident,
 * so a board runs a handful: a wire, a panel that repaints itself, later a
 * radio.  A table avoids an allocation on a path that runs while a module is
 * being loaded, which is exactly when memory is tightest.
 */
#define AG_SVC_MAX 6

/*
 * Below every application: 2, 5 and 8 are AG_PRIO_LOW, NORMAL and HIGH.
 *
 * Not a parameter, and the history is the argument.  A driver's task that sat
 * above applications and never blocked took a game from 40 fps to unplayable,
 * and the cure was to move it below them.  A driver that thinks it needs to be
 * above an application is a driver that has not yet met the application.
 */
#define AG_SVC_PRIORITY 1u

#define AG_SVC_STACK_MIN 2048u
#define AG_SVC_STACK_DEF 4096u

typedef struct {
    bool             used;
    const void      *owner; /* the module cookie */
    void           (*fn)(void *);
    void            *arg;
    volatile bool    stopping;
    volatile bool    finished;
    ag_port_task_t   task;
    char             name[16];
} svc_t;

static svc_t s_svc[AG_SVC_MAX];

/*
 * The trampoline.  It exists so that "the task has left" is a fact the kernel
 * owns rather than something the driver is trusted to report: the flag is set
 * here, after the driver's function has returned and before the task deletes
 * itself, so a driver cannot forget to say goodbye.
 */
static void svc_trampoline(void *arg)
{
    svc_t *s = (svc_t *)arg;
    s->fn(s->arg);
    s->finished = true;
    ag_port_task_delete(NULL);
}

ag_svc_t ag_svc_start(void (*fn)(void *), void *arg, const char *name,
                      size_t stack, uint32_t flags)
{
    (void)flags; /* reserved: core choice, and a budget above applications */

    const void *owner = ag_module_loading();
    if (fn == NULL || owner == NULL) {
        /*
         * Outside ag_driver_init there is no module to own this, and an
         * unowned task is one nothing can ever stop - the failure this whole
         * file exists to avoid.
         */
        return NULL;
    }

    size_t want = (stack != 0) ? stack : AG_SVC_STACK_DEF;
    if (want < AG_SVC_STACK_MIN) {
        want = AG_SVC_STACK_MIN;
    }

    for (int i = 0; i < AG_SVC_MAX; i++) {
        svc_t *s = &s_svc[i];
        if (s->used) {
            continue;
        }
        memset(s, 0, sizeof(*s));
        s->used = true;
        s->owner = owner;
        s->fn = fn;
        s->arg = arg;
        snprintf(s->name, sizeof(s->name), "%s", (name != NULL) ? name : "svc");

        if (!ag_port_task_create(svc_trampoline, s->name, (uint32_t)want, s,
                                 AG_SVC_PRIORITY, 0, 0, &s->task)) {
            memset(s, 0, sizeof(*s));
            return NULL;
        }
        return (ag_svc_t)s;
    }

    ag_log(AG_LOG_WARN, "svc", "no room for another service task");
    return NULL;
}

bool ag_svc_stopping(ag_svc_t h)
{
    const svc_t *s = (const svc_t *)h;
    return (s == NULL) || !s->used || s->stopping;
}

uint32_t ag_svc_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < AG_SVC_MAX; i++) {
        if (s_svc[i].used) {
            n++;
        }
    }
    return n;
}

bool ag_svc_stop_owner(const void *owner)
{
    bool all_gone = true;

    /* Ask first, all of them, then wait once.  Asking and waiting one at a
     * time would add up the timeouts for no reason. */
    bool any = false;
    for (int i = 0; i < AG_SVC_MAX; i++) {
        if (s_svc[i].used && s_svc[i].owner == owner) {
            s_svc[i].stopping = true;
            any = true;
        }
    }
    if (!any) {
        return true;
    }

    const int64_t deadline = ag_port_us() + (int64_t)AG_SVC_STOP_MS * 1000;
    for (;;) {
        bool waiting = false;
        for (int i = 0; i < AG_SVC_MAX; i++) {
            if (s_svc[i].used && s_svc[i].owner == owner &&
                !s_svc[i].finished) {
                waiting = true;
            }
        }
        if (!waiting) {
            break;
        }
        if (ag_port_us() >= deadline) {
            break;
        }
        ag_port_task_delay(ag_port_ms_to_ticks(5));
    }

    for (int i = 0; i < AG_SVC_MAX; i++) {
        svc_t *s = &s_svc[i];
        if (!s->used || s->owner != owner) {
            continue;
        }
        if (!s->finished) {
            /*
             * Not killed.  It is running code in the module's image, and
             * killing it would leave whatever it held - a mutex over the wire,
             * a half-written frame - held for ever, in an image about to be
             * freed.  Said loudly instead, and the caller keeps the image.
             */
            ag_log(AG_LOG_ERROR, "svc",
                   "%s did not stop in %u ms; its module stays mapped",
                   s->name, (unsigned)AG_SVC_STOP_MS);
            all_gone = false;
            continue;
        }
        memset(s, 0, sizeof(*s));
    }
    return all_gone;
}
