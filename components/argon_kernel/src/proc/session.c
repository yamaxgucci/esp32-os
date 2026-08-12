/*
 * ArgonOS - session slots (system shell + up to three user app slots).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/session.h>

#include <stdio.h>
#include <string.h>

#include <argon/console.h>
#include <argon/display.h>
#include <argon/log.h>
#include <argon/proc.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AG_BREAKIN_DOUBLE_US 1000000ll /* second Ctrl+\ within 1 s → kill */

static ag_session_slot_t s_slots[AG_SESSION_SLOTS];
static int               s_focused = AG_SESSION_SYSTEM;
static ag_pid_t          s_last_user = AG_PID_KERNEL;
static int64_t           s_last_breakin_us;

void ag_session_init(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    strncpy(s_slots[0].name, "system", sizeof(s_slots[0].name) - 1);
    s_slots[0].pid = AG_PID_KERNEL;
    s_focused = AG_SESSION_SYSTEM;
    s_last_user = AG_PID_KERNEL;
    s_last_breakin_us = 0;
    (void)ag_proc_set_foreground(AG_PID_KERNEL);
}

int ag_session_focused(void) { return s_focused; }

ag_pid_t ag_session_focused_pid(void)
{
    if (s_focused == AG_SESSION_SYSTEM) {
        return AG_PID_KERNEL;
    }
    return s_slots[s_focused].pid;
}

int ag_session_slot_of(ag_pid_t pid)
{
    if (pid == AG_PID_KERNEL) {
        return AG_SESSION_SYSTEM;
    }
    for (int i = 1; i < AG_SESSION_SLOTS; i++) {
        if (s_slots[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

ag_err_t ag_session_bind(ag_pid_t pid, const char *name)
{
    if (pid == AG_PID_KERNEL) {
        return -AG_EINVAL;
    }
    if (ag_session_slot_of(pid) >= 0) {
        return AG_OK;
    }
    for (int i = 1; i < AG_SESSION_SLOTS; i++) {
        if (s_slots[i].pid == AG_PID_KERNEL) {
            s_slots[i].pid = pid;
            memset(s_slots[i].name, 0, sizeof(s_slots[i].name));
            if (name != NULL) {
                strncpy(s_slots[i].name, name, sizeof(s_slots[i].name) - 1);
            }
            return AG_OK;
        }
    }
    return -AG_ENFILE;
}

void ag_session_unbind(ag_pid_t pid)
{
    const int slot = ag_session_slot_of(pid);
    if (slot <= 0) {
        return;
    }
    s_slots[slot].pid = AG_PID_KERNEL;
    s_slots[slot].name[0] = '\0';
    if (s_last_user == pid) {
        s_last_user = AG_PID_KERNEL;
    }
    if (s_focused == slot) {
        (void)ag_session_focus(AG_SESSION_SYSTEM);
    }
}

static void notify_focus(ag_pid_t prev_pid, ag_pid_t next_pid)
{
    if (prev_pid != AG_PID_KERNEL && prev_pid != next_pid) {
        ag_proc_post_focus_event(prev_pid, false);
    }
    if (next_pid != AG_PID_KERNEL && next_pid != prev_pid) {
        ag_proc_post_focus_event(next_pid, true);
    }
}

ag_err_t ag_session_focus(int slot)
{
    if (slot < 0 || slot >= AG_SESSION_SLOTS) {
        return -AG_EINVAL;
    }
    if (slot != AG_SESSION_SYSTEM && s_slots[slot].pid == AG_PID_KERNEL) {
        return -AG_ENOENT;
    }

    const ag_pid_t prev_pid = ag_session_focused_pid();
    const int      prev_slot = s_focused;

    if (slot == prev_slot) {
        return AG_OK;
    }

    const ag_pid_t next_pid =
        (slot == AG_SESSION_SYSTEM) ? AG_PID_KERNEL : s_slots[slot].pid;

    /* Freeze gfx when leaving its owner — not when returning to the same app. */
    if (ag_display_acquired() && ag_display_owner() != next_pid) {
        ag_display_force_release();
    }

    s_focused = slot;
    if (slot != AG_SESSION_SYSTEM) {
        s_last_user = next_pid;
    }

    (void)ag_proc_set_foreground(next_pid);
    notify_focus(prev_pid, next_pid);

    if (ag_console_ready()) {
        if (slot == AG_SESSION_SYSTEM) {
            ag_console_printf("\n[system shell]  slots / ps / kill / Alt+1..4\n");
        } else {
            ag_console_printf("\n[slot %d] %s (pid %u)\n", slot,
                              s_slots[slot].name[0] ? s_slots[slot].name : "?",
                              (unsigned)next_pid);
        }
    }

    ag_log(AG_LOG_INFO, "session", "focus %d -> %d (pid %u)", prev_slot, slot,
           (unsigned)next_pid);
    return AG_OK;
}

bool ag_session_enter_system(void)
{
    const int64_t now = esp_timer_get_time();
    const bool second =
        (s_focused == AG_SESSION_SYSTEM) &&
        (s_last_breakin_us != 0) &&
        (now - s_last_breakin_us) < AG_BREAKIN_DOUBLE_US &&
        (s_last_user != AG_PID_KERNEL);

    s_last_breakin_us = now;

    if (second) {
        const ag_pid_t victim = s_last_user;
        ag_console_printf("\nstopping pid %u...\n", (unsigned)victim);
        const ag_err_t err = ag_proc_kill(victim, "stopped from the keyboard");
        if (err == AG_OK) {
            ag_console_printf("pid %u stopped\n", (unsigned)victim);
            return true;
        }
        if (err == -AG_EBUSY) {
            ag_console_puts("busy (kernel I/O); cancelling — try again\n");
            (void)ag_proc_signal(victim);
            return false;
        }
        if (err != -AG_ENOENT) {
            ag_console_printf("could not stop pid %u: %d\n", (unsigned)victim,
                              (int)err);
        }
        return false;
    }

    if (s_focused != AG_SESSION_SYSTEM) {
        s_last_user = ag_session_focused_pid();
    } else if (ag_proc_foreground() != AG_PID_KERNEL) {
        s_last_user = ag_proc_foreground();
    }

    (void)ag_session_focus(AG_SESSION_SYSTEM);
    ag_console_puts("switched to system shell (Ctrl+\\ again = kill last app)\n");
    return false;
}

ag_err_t ag_session_alt_tab(void)
{
    int order[AG_SESSION_SLOTS];
    int n = 0;
    order[n++] = AG_SESSION_SYSTEM;
    for (int i = 1; i < AG_SESSION_SLOTS; i++) {
        if (s_slots[i].pid != AG_PID_KERNEL) {
            order[n++] = i;
        }
    }
    if (n <= 1) {
        return ag_session_focus(AG_SESSION_SYSTEM);
    }

    int cur = 0;
    for (int i = 0; i < n; i++) {
        if (order[i] == s_focused) {
            cur = i;
            break;
        }
    }
    const int next = order[(cur + 1) % n];

    char label[64];
    if (next == AG_SESSION_SYSTEM) {
        snprintf(label, sizeof(label), "Alt+Tab: 0 system");
    } else {
        snprintf(label, sizeof(label), "Alt+Tab: %d %s", next,
                 s_slots[next].name[0] ? s_slots[next].name : "app");
    }
    ag_display_show_overlay(label);
    vTaskDelay(pdMS_TO_TICKS(350));

    return ag_session_focus(next);
}

void ag_session_info(ag_session_slot_t out[AG_SESSION_SLOTS])
{
    if (out == NULL) {
        return;
    }
    memcpy(out, s_slots, sizeof(s_slots));
}
