/*
 * ArgonOS port: ESP-IDF - the list of tasks, for "who holds the memory".
 *
 * FreeRTOS's own list (uxTaskGetSystemState) needs the trace facility, which
 * this build leaves off.  IDF's task iterator and snapshot, which its panic
 * handler uses, need nothing: walked with the scheduler held so no task can
 * be deleted while it is being read.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/task.h>

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_private/freertos_debug.h"
#include "esp_memory_utils.h"

/*
 * One walk of the task lists: each task once, however many lists it passed
 * through while being walked.
 */
static uint32_t walk(ag_port_task_info_t *out, uint32_t max, uint32_t *seen,
                     bool stacks)
{
    uint32_t       n = 0;
    uint32_t       all = 0;
    TaskIterator_t it;
    memset(&it, 0, sizeof(it));

    /*
     * AND NEVER FOR EVER (29 Sep 2026).  A task the other core moves while it
     * is being walked takes the walk into the list it moved to, whose end is
     * not the one the walk is looking for: it went round that list with this
     * core's scheduler held, and the console that asked never answered again.
     * The CYD's console "hung" exactly so - every time, with `cpu 3` typed on
     * it while the picture kept its other core busy.  A walk stops after
     * enough steps for every task several times over; what it found by then
     * is the answer.
     */
    const uint32_t most = 4u * (uint32_t)uxTaskGetNumberOfTasks() + 32u;
    uint32_t       steps = 0;
    vTaskSuspendAll();
    while (++steps <= most && xTaskGetNext(&it) != -1) {
        const TaskHandle_t t = it.pxTaskHandle;
        if (t == NULL) {
            continue;
        }
        bool again = false;
        for (uint32_t k = 0; k < n; k++) {
            if (out[k].task == t) {
                again = true;
                break;
            }
        }
        if (again) {
            continue;
        }
        all++;
        if (n < max) {
            ag_port_task_info_t *const o = &out[n];
            memset(o, 0, sizeof(*o));
            strncpy(o->name, pcTaskGetName(t), sizeof(o->name) - 1u);
            TaskSnapshot_t snap;
            const uint8_t *const base = stacks ? pxTaskGetStackStart(t) : NULL;
            if (base != NULL && vTaskGetSnapshot(t, &snap) == pdTRUE) {
                const uint8_t *const end = (const uint8_t *)snap.pxEndOfStack;
                o->stack_bytes = (end > base)
                                     ? (uint32_t)(end - base) + sizeof(StackType_t)
                                     : (uint32_t)(base - end) + sizeof(StackType_t);
            }
            if (stacks) {
                o->stack_unused = (uint32_t)uxTaskGetStackHighWaterMark(t);
            }
            o->stack_slow = base != NULL && esp_ptr_external_ram(base);
            o->prio = (unsigned)uxTaskPriorityGet(t);
            o->task = t;
            n++;
        }
    }
    (void)xTaskResumeAll();
    *seen = all;
    return n;
}

/*
 * AND WALKED AGAIN WHEN IT MISSED ONE (29 Sep 2026).  Holding the scheduler
 * holds this core's only: on the other the tasks go on moving between the
 * lists being walked, and one that leaves a list not yet reached for one
 * already passed is not seen at all.  The CYD's `cpu` called its busiest
 * task - the wire's reader, woken thousands of times a second - "(ended)",
 * one run the reader and the next its sender.  So the walk is repeated,
 * up to four times, until it finds as many tasks as the kernel counts.
 */
static uint32_t task_list(ag_port_task_info_t *out, uint32_t max, bool stacks)
{
    if (out == NULL || max == 0u) {
        return (uint32_t)uxTaskGetNumberOfTasks();
    }
    uint32_t seen = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        (void)walk(out, max, &seen, stacks);
        if (seen >= (uint32_t)uxTaskGetNumberOfTasks()) {
            break;
        }
    }
    return seen;
}

uint32_t ag_port_task_list(ag_port_task_info_t *out, uint32_t max)
{
    return task_list(out, max, true);
}

/*
 * WITHOUT THE STACKS (30 Sep 2026).  How much of a stack was never touched is
 * found by reading it from the far end until the fill pattern stops - every
 * task's, byte by byte, with this core's scheduler held, up to four walks.
 * `cpu` wants names and priorities only, and on the CYD the scan held the
 * sound's task off its core for long enough to hear.
 */
uint32_t ag_port_task_names(ag_port_task_info_t *out, uint32_t max)
{
    return task_list(out, max, false);
}
