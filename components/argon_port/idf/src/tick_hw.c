/*
 * ArgonOS port: ESP-IDF - the scheduler's own tick, handed upward.
 *
 * One callback per core, on every system tick, told which core it is and what
 * was running there.  It exists so the kernel can charge processor time to the
 * task that spent it - a millisecond at a time, which is coarse and free.  The
 * alternative, accounting on every context switch, is precise and costs a
 * timer read per switch on a machine that switches thousands of times a
 * second; for budgets measured in tens of milliseconds the tick is plenty.
 *
 * This runs in interrupt context, on both cores, including while a flash
 * write has the instruction cache switched off.  So the trampolines are in
 * IRAM and whatever is registered must be too, and must call nothing that is
 * not: a handler in flash brings the core down with "Cache disabled but cached
 * memory region accessed", which is how this file announced itself the first
 * time it was flashed.  It must also be short and must not block.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/task.h>

#include "esp_err.h"
#include "esp_freertos_hooks.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static ag_port_tick_fn s_tick_fn;

static void AG_PORT_IRAM tick_on_core0(void)
{
    ag_port_tick_fn fn = s_tick_fn;
    if (fn != NULL) {
        fn(0, xTaskGetCurrentTaskHandleForCore(0));
    }
}

static void AG_PORT_IRAM tick_on_core1(void)
{
    ag_port_tick_fn fn = s_tick_fn;
    if (fn != NULL) {
        fn(1, xTaskGetCurrentTaskHandleForCore(1));
    }
}

bool ag_port_tick_hook(ag_port_tick_fn fn)
{
    /*
     * Registered before the pointer is published, so a tick that lands between
     * the two calls finds either the old callback or none, never a half-set
     * one.  Unregistering is not offered: nothing in this system takes the
     * accounting back down, and a hook that can vanish under an interrupt is a
     * race for no gain.
     */
    if (fn == NULL) {
        return false;
    }
    s_tick_fn = fn;

    if (esp_register_freertos_tick_hook_for_cpu(tick_on_core0, 0) != ESP_OK) {
        s_tick_fn = NULL;
        return false;
    }
#if !CONFIG_FREERTOS_UNICORE
    if (esp_register_freertos_tick_hook_for_cpu(tick_on_core1, 1) != ESP_OK) {
        /* One core accounted for is worse than two and better than none: the
         * application core is where an overrun is felt, and it is core 1. */
        return true;
    }
#endif
    return true;
}
