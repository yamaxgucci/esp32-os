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

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_private/freertos_debug.h"

uint32_t ag_port_task_list(ag_port_task_info_t *out, uint32_t max)
{
    uint32_t       n = 0;
    TaskIterator_t it;
    memset(&it, 0, sizeof(it));

    vTaskSuspendAll();
    while (xTaskGetNext(&it) != -1) {
        const TaskHandle_t t = it.pxTaskHandle;
        if (t == NULL) {
            continue;
        }
        if (out != NULL && n < max) {
            ag_port_task_info_t *const o = &out[n];
            memset(o, 0, sizeof(*o));
            strncpy(o->name, pcTaskGetName(t), sizeof(o->name) - 1u);
            TaskSnapshot_t snap;
            const uint8_t *const base = pxTaskGetStackStart(t);
            if (base != NULL && vTaskGetSnapshot(t, &snap) == pdTRUE) {
                const uint8_t *const end = (const uint8_t *)snap.pxEndOfStack;
                o->stack_bytes = (end > base)
                                     ? (uint32_t)(end - base) + sizeof(StackType_t)
                                     : (uint32_t)(base - end) + sizeof(StackType_t);
            }
            o->stack_unused = (uint32_t)uxTaskGetStackHighWaterMark(t);
            o->prio = (unsigned)uxTaskPriorityGet(t);
            o->task = t;
        }
        n++;
    }
    (void)xTaskResumeAll();
    return n;
}
