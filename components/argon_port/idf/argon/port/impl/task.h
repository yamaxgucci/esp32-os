/*
 * ArgonOS port: ESP-IDF - tasks.
 *
 * ag_port_ticks_t is FreeRTOS's own TickType_t and AG_PORT_FOREVER is portMAX_DELAY,
 * for the reason given in impl/mem.h: where the two agree the wrapper must cost
 * nothing.  pdMS_TO_TICKS is a compile-time expression and stays one here.
 *
 * The stack-capability variant is prvTaskCreateDynamicPinnedToCoreWithCaps and
 * not xTaskCreatePinnedToCoreWithCaps, which looks like the obvious choice and
 * is not: the WithCaps family must be torn down with vTaskDeleteWithCaps, and
 * every process and thread exit path in this kernel ends in a plain delete.  A
 * task created one way and deleted the other leaks its stack, and on a system
 * that runs applications for a living that is a leak per run.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_TASK_H
#define ARGON_PORT_IMPL_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#include "esp_private/freertos_idf_additions_priv.h"
#define AG_PORT_TASK_STACK_CAPS 1
#else
#define AG_PORT_TASK_STACK_CAPS 0
#endif

typedef TaskHandle_t  ag_port_task_t;
typedef TickType_t    ag_port_ticks_t;
typedef TaskFunction_t ag_port_task_fn;

#define AG_PORT_FOREVER  portMAX_DELAY
#define AG_PORT_ANY_CORE tskNO_AFFINITY

static inline ag_port_ticks_t ag_port_ms_to_ticks(uint32_t ms)
{
    return pdMS_TO_TICKS(ms);
}

static inline ag_port_ticks_t ag_port_ticks(void) { return xTaskGetTickCount(); }

/*
 * stack_caps == 0 means "wherever the port normally puts a stack".  With caps,
 * the stack is placed in memory of that kind and nowhere else - the fallback to
 * another kind is the kernel's decision to make, not this layer's, because the
 * kernel is the one that has to say so in the log.
 */
static inline bool ag_port_task_create(ag_port_task_fn fn, const char *name,
                                       uint32_t stack_bytes, void *arg,
                                       unsigned prio, int core,
                                       unsigned stack_caps, ag_port_task_t *out)
{
#if AG_PORT_TASK_STACK_CAPS
    if (stack_caps != 0u) {
        return prvTaskCreateDynamicPinnedToCoreWithCaps(
                   fn, name, stack_bytes, arg, (UBaseType_t)prio,
                   (BaseType_t)core, stack_caps, out) == pdPASS;
    }
#else
    (void)stack_caps;
#endif
    return xTaskCreatePinnedToCore(fn, name, stack_bytes, arg,
                                   (UBaseType_t)prio, out,
                                   (BaseType_t)core) == pdPASS;
}

static inline void ag_port_task_delete(ag_port_task_t t) { vTaskDelete(t); }

static inline void ag_port_task_delay(ag_port_ticks_t ticks) { vTaskDelay(ticks); }

static inline ag_port_task_t ag_port_task_self(void)
{
    return xTaskGetCurrentTaskHandle();
}

static inline void ag_port_task_prio_set(ag_port_task_t t, unsigned prio)
{
    vTaskPrioritySet(t, (UBaseType_t)prio);
}

static inline uint32_t ag_port_task_stack_unused(ag_port_task_t t)
{
    return (uint32_t)uxTaskGetStackHighWaterMark(t);
}

static inline void ag_port_notify_give(ag_port_task_t t) { xTaskNotifyGive(t); }

static inline void ag_port_notify_take(bool clear, ag_port_ticks_t ticks)
{
    (void)ulTaskNotifyTake(clear ? pdTRUE : pdFALSE, ticks);
}

static inline void ag_port_task_yield(void) { taskYIELD(); }

static inline void ag_port_sched_lock(void) { vTaskSuspendAll(); }

static inline void ag_port_sched_unlock(void) { (void)xTaskResumeAll(); }

#endif /* ARGON_PORT_IMPL_TASK_H */
