/*
 * ArgonOS - session slots (per-slot shell + optional app) and system shell.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/session.h>

#include <stdio.h>
#include <string.h>

#include <argon/console.h>
#include <argon/display.h>
#include <argon/log.h>
#include <argon/proc.h>
#include <argon/screen.h>

#include "proc/supervisor.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AG_BREAKIN_DOUBLE_US 1000000ll

typedef struct {
    ag_pid_t pid;
    char     name[32];
    char     cwd[AG_PATH_MAX];
} slot_t;

/* cwd[] is large; keep the table out of scarce internal DRAM. */
static slot_t  *s_slots;
static slot_t  *s_sys;
static int      s_focused = 0; /* boot: user slot 1 (index 0) */
static int      s_last_user_slot = 0;
static ag_pid_t s_last_user = AG_PID_KERNEL;
static int64_t  s_last_breakin_us;

static void cwd_set_default(char *cwd, size_t n)
{
    strncpy(cwd, "/", n - 1);
    cwd[n - 1] = '\0';
}

static bool is_user_slot(int slot)
{
    return slot >= 0 && slot < AG_SESSION_SLOTS;
}

static ag_pid_t slot_pid(int slot)
{
    if (slot == AG_SESSION_SYSTEM) {
        return AG_PID_KERNEL;
    }
    if (!is_user_slot(slot) || s_slots == NULL) {
        return AG_PID_KERNEL;
    }
    return s_slots[slot].pid;
}

void ag_session_init(void)
{
    if (s_slots == NULL) {
        s_slots = (slot_t *)heap_caps_calloc(
            AG_SESSION_SLOTS, sizeof(slot_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_sys == NULL) {
        s_sys = (slot_t *)heap_caps_calloc(1, sizeof(slot_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_slots == NULL || s_sys == NULL) {
        /* Last resort: internal DRAM (boot must not proceed half-init). */
        if (s_slots == NULL) {
            s_slots = (slot_t *)heap_caps_calloc(
                AG_SESSION_SLOTS, sizeof(slot_t),
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (s_sys == NULL) {
            s_sys = (slot_t *)heap_caps_calloc(
                1, sizeof(slot_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    if (s_slots == NULL || s_sys == NULL) {
        ag_log(AG_LOG_ERROR, "session", "no memory for session slots");
        abort();
    }

    memset(s_slots, 0, AG_SESSION_SLOTS * sizeof(slot_t));
    memset(s_sys, 0, sizeof(*s_sys));
    for (int i = 0; i < AG_SESSION_SLOTS; i++) {
        cwd_set_default(s_slots[i].cwd, sizeof(s_slots[i].cwd));
    }
    cwd_set_default(s_sys->cwd, sizeof(s_sys->cwd));
    strncpy(s_sys->name, "system", sizeof(s_sys->name) - 1);

    s_focused = 0;
    s_last_user_slot = 0;
    s_last_user = AG_PID_KERNEL;
    s_last_breakin_us = 0;
    (void)ag_proc_set_foreground(AG_PID_KERNEL);
}

int ag_session_focused(void) { return s_focused; }

bool ag_session_is_system(void) { return s_focused == AG_SESSION_SYSTEM; }

int ag_session_display_number(int slot)
{
    if (!is_user_slot(slot)) {
        return 0;
    }
    return slot + 1;
}

ag_pid_t ag_session_focused_pid(void) { return slot_pid(s_focused); }

bool ag_session_shell_owns_keyboard(void)
{
    if (s_focused == AG_SESSION_SYSTEM) {
        return true;
    }
    return is_user_slot(s_focused) && s_slots[s_focused].pid == AG_PID_KERNEL;
}

int ag_session_slot_of(ag_pid_t pid)
{
    if (pid == AG_PID_KERNEL) {
        return -1;
    }
    for (int i = 0; i < AG_SESSION_SLOTS; i++) {
        if (s_slots[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

const char *ag_session_cwd(int slot)
{
    if (slot == AG_SESSION_SYSTEM) {
        return (s_sys != NULL) ? s_sys->cwd : "/";
    }
    if (!is_user_slot(slot) || s_slots == NULL) {
        return "/";
    }
    return s_slots[slot].cwd;
}

ag_err_t ag_session_set_cwd(int slot, const char *absolute_path)
{
    char  *cwd;
    size_t cap;

    if (absolute_path == NULL || absolute_path[0] != '/' || s_slots == NULL ||
        s_sys == NULL) {
        return -AG_EINVAL;
    }
    if (slot == AG_SESSION_SYSTEM) {
        cwd = s_sys->cwd;
        cap = sizeof(s_sys->cwd);
    } else if (is_user_slot(slot)) {
        cwd = s_slots[slot].cwd;
        cap = sizeof(s_slots[slot].cwd);
    } else {
        return -AG_EINVAL;
    }
    if (strlen(absolute_path) >= cap) {
        return -AG_ERANGE;
    }
    strcpy(cwd, absolute_path);
    return AG_OK;
}

static void clear_pid_from_slots(ag_pid_t pid)
{
    for (int i = 0; i < AG_SESSION_SLOTS; i++) {
        if (s_slots[i].pid == pid) {
            s_slots[i].pid = AG_PID_KERNEL;
            s_slots[i].name[0] = '\0';
        }
    }
}

ag_err_t ag_session_bind_to(ag_pid_t pid, const char *name, int slot)
{
    if (pid == AG_PID_KERNEL || !is_user_slot(slot)) {
        return -AG_EINVAL;
    }
    if (s_slots[slot].pid != AG_PID_KERNEL && s_slots[slot].pid != pid) {
        return -AG_EBUSY;
    }
    clear_pid_from_slots(pid);
    s_slots[slot].pid = pid;
    memset(s_slots[slot].name, 0, sizeof(s_slots[slot].name));
    if (name != NULL) {
        strncpy(s_slots[slot].name, name, sizeof(s_slots[slot].name) - 1);
    }
    return AG_OK;
}

ag_err_t ag_session_bind(ag_pid_t pid, const char *name)
{
    if (pid == AG_PID_KERNEL) {
        return -AG_EINVAL;
    }
    if (ag_session_slot_of(pid) >= 0) {
        return AG_OK;
    }
    /* Prefer the focused free user slot so spawners match where the user typed. */
    if (is_user_slot(s_focused) && s_slots[s_focused].pid == AG_PID_KERNEL) {
        return ag_session_bind_to(pid, name, s_focused);
    }
    for (int i = 0; i < AG_SESSION_SLOTS; i++) {
        if (s_slots[i].pid == AG_PID_KERNEL) {
            return ag_session_bind_to(pid, name, i);
        }
    }
    return -AG_ENFILE;
}

void ag_session_unbind(ag_pid_t pid)
{
    const int slot = ag_session_slot_of(pid);
    if (slot < 0) {
        return;
    }
    ag_log(AG_LOG_INFO, "session", "unbind pid %u from slot %d (was %s)",
           (unsigned)pid, ag_session_display_number(slot),
           s_slots[slot].name[0] ? s_slots[slot].name : "?");
    s_slots[slot].pid = AG_PID_KERNEL;
    s_slots[slot].name[0] = '\0';
    if (s_last_user == pid) {
        s_last_user = AG_PID_KERNEL;
    }
    if (s_focused == slot) {
        (void)ag_proc_set_foreground(AG_PID_KERNEL);
        ag_supervisor_raise_shell_interrupt();
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

static void enter_shell_view(int slot)
{
    if (slot == AG_SESSION_SYSTEM) {
        ag_log(AG_LOG_INFO, "session", "enter_shell_view sys (inject QUIT)");
    } else {
        ag_log(AG_LOG_INFO, "session", "enter_shell_view slot %d (inject QUIT)",
               ag_session_display_number(slot));
    }

    if (ag_display_acquired()) {
        ag_display_force_release();
    }

    if (ag_console_ready()) {
        ag_console_lock();
        ag_screen_cls(ag_console_screen());
        ag_screen_set_attr(ag_console_screen(), AG_ATTR_DEFAULT);
        ag_screen_set_cursor(ag_console_screen(), true);
        ag_console_unlock();
        if (slot == AG_SESSION_SYSTEM) {
            ag_console_puts("system shell\n");
        } else {
            ag_console_printf("slot %d\n", ag_session_display_number(slot));
        }
    }

    ag_supervisor_raise_shell_interrupt();
    {
        ag_event_t quit = {0};
        quit.type = AG_EV_QUIT;
        (void)ag_console_inject_event(&quit);
    }
}

/*
 * Bound AXE still reading its image: the process owns the slot (so Alt+N can
 * return here) but cannot paint yet.  Without this, focus looks like a hang.
 */
static void enter_loading_view(int slot, ag_pid_t pid)
{
    const char *name = s_slots[slot].name[0] ? s_slots[slot].name : "app";

    ag_log(AG_LOG_INFO, "session", "enter_loading_view slot %d pid %u (%s)",
           ag_session_display_number(slot), (unsigned)pid, name);

    if (ag_display_acquired()) {
        ag_display_force_release();
    }

    if (ag_console_ready()) {
        ag_console_lock();
        ag_screen_cls(ag_console_screen());
        ag_screen_set_attr(ag_console_screen(), AG_ATTR_DEFAULT);
        ag_screen_set_cursor(ag_console_screen(), true);
        ag_console_unlock();
        ag_console_printf("slot %d - loading %s (pid %u)\n",
                          ag_session_display_number(slot), name, (unsigned)pid);
        ag_console_puts("please wait; Alt+1..4 switches slots\n");
    }
}

static bool proc_is_loading(ag_pid_t pid)
{
    ag_proc_state_t st;
    return pid != AG_PID_KERNEL && ag_proc_state_of(pid, &st) == AG_OK &&
           st == AG_PS_LOADING;
}

ag_err_t ag_session_focus(int slot)
{
    if (slot != AG_SESSION_SYSTEM && !is_user_slot(slot)) {
        return -AG_EINVAL;
    }

    const ag_pid_t prev_pid = ag_session_focused_pid();
    const int      prev_slot = s_focused;
    const ag_pid_t next_pid = slot_pid(slot);

    /*
     * Same user slot after bind_to(app): index unchanged but the occupant went
     * from shell → process.  Still need foreground + FOCUS_GAINED.
     */
    if (slot == prev_slot) {
        if (is_user_slot(slot) && next_pid != AG_PID_KERNEL &&
            ag_proc_foreground() != next_pid) {
            (void)ag_proc_set_foreground(next_pid);
            s_last_user = next_pid;
            if (ag_console_ready()) {
                ag_console_flush_input();
            }
            notify_focus(AG_PID_KERNEL, next_pid);
            if (proc_is_loading(next_pid)) {
                enter_loading_view(slot, next_pid);
            }
            ag_log(AG_LOG_INFO, "session", "adopt pid %u in slot %d",
                   (unsigned)next_pid, ag_session_display_number(slot));
        }
        return AG_OK;
    }

    if (ag_display_acquired() && ag_display_owner() != next_pid) {
        ag_display_force_release();
    }

    if (is_user_slot(prev_slot)) {
        s_last_user_slot = prev_slot;
    }

    s_focused = slot;
    if (is_user_slot(slot)) {
        s_last_user_slot = slot;
    }
    if (next_pid != AG_PID_KERNEL) {
        s_last_user = next_pid;
    }

    (void)ag_proc_set_foreground(next_pid);

    /* Drop shell QUIT/keys so the app does not treat them as its own exit. */
    if (next_pid != AG_PID_KERNEL && ag_console_ready()) {
        ag_console_flush_input();
    }

    notify_focus(prev_pid, next_pid);

    if (next_pid == AG_PID_KERNEL) {
        enter_shell_view(slot);
    } else if (proc_is_loading(next_pid)) {
        enter_loading_view(slot, next_pid);
    }
    /* Ready apps own the screen — no console banner over them. */

    ag_log(AG_LOG_INFO, "session", "focus %d -> %d (pid %u)", prev_slot, slot,
           (unsigned)next_pid);
    return AG_OK;
}

bool ag_session_enter_system(void)
{
    const int64_t now = esp_timer_get_time();
    const bool second =
        (s_focused == AG_SESSION_SYSTEM) && (s_last_breakin_us != 0) &&
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

    if (ag_session_focused_pid() != AG_PID_KERNEL) {
        s_last_user = ag_session_focused_pid();
    } else if (ag_proc_foreground() != AG_PID_KERNEL) {
        s_last_user = ag_proc_foreground();
    }

    (void)ag_session_focus(AG_SESSION_SYSTEM);
    ag_console_puts("Ctrl+\\ again = kill last app\n");
    return false;
}

ag_err_t ag_session_alt_tab(void)
{
    int next;
    if (s_focused == AG_SESSION_SYSTEM) {
        next = is_user_slot(s_last_user_slot) ? s_last_user_slot : 0;
    } else {
        next = (s_focused + 1) % AG_SESSION_SLOTS;
    }

    char label[64];
    if (s_slots[next].pid == AG_PID_KERNEL) {
        snprintf(label, sizeof(label), "Alt+Tab: (%d) shell",
                 ag_session_display_number(next));
    } else {
        snprintf(label, sizeof(label), "Alt+Tab: (%d) %s",
                 ag_session_display_number(next),
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
    for (int i = 0; i < AG_SESSION_SLOTS; i++) {
        out[i].pid = s_slots[i].pid;
        memcpy(out[i].name, s_slots[i].name, sizeof(out[i].name));
        memcpy(out[i].cwd, s_slots[i].cwd, sizeof(out[i].cwd));
    }
}
