/*
 * ArgonOS - the supervisor.
 *
 * A high-priority task on the system core whose whole purpose is to still be
 * able to act when the application cannot: to hear the key that stops it, to
 * collect what has finished, and to write down what happened.
 *
 * It is deliberately small.  Everything it does could be done by whoever calls
 * it, except for one thing: being scheduled at all while an application is
 * spinning on the other core.  That is what it is for.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "proc/supervisor.h"

#include <stdio.h>
#include <string.h>

#include <argon/console.h>
#include <argon/journal.h>
#include <argon/keys.h>
#include <argon/log.h>
#include <argon/proc.h>
#include <argon/session.h>
#include <argon/vfs.h>

#include "proc/proc_internal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Above the shell and any application, below ESP-IDF's own service tasks (the
 * timer task is 22, Wi-Fi is 23).  High enough to preempt a runaway application
 * immediately; low enough not to interfere with drivers.
 */
#define AG_SUP_PRIORITY 15

/*
 * 4 KB: the deepest thing it does is write a crash record, which is a filesystem
 * call with a line buffer on this stack.  A supervisor that overflows its own
 * stack while reporting somebody else's failure would be a poor joke.
 */
#define AG_SUP_STACK 4096

/* Nothing needs doing most of the time; a wake-up also collects zombies. */
#define AG_SUP_TICK_MS 250

static TaskHandle_t      s_task;
static volatile bool     s_stop_request;
static volatile bool     s_interrupt_request;
static volatile bool     s_shell_interrupt;
static volatile bool     s_alt_tab_request;
static volatile int      s_focus_slot_request; /* -1 = none, 0..3 = slot */
static volatile ag_pid_t s_kill_request;
static uint32_t          s_stops;

void ag_supervisor_kill_request(ag_pid_t pid)
{
    s_kill_request = pid;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

static void request_soft_interrupt(ag_event_t *ev)
{
    if (ag_proc_foreground() != AG_PID_KERNEL) {
        s_interrupt_request = true;
        if (s_task != NULL) {
            xTaskNotifyGive(s_task);
        }
        ev->type = AG_EV_QUIT;
    } else {
        s_shell_interrupt = true;
    }
}

/*
 * Runs on the console task, so it decides and returns; the work happens on the
 * supervisor's own task (except session focus, which is safe here).
 */
static bool hotkeys(ag_event_t *ev)
{
    if (ev->type != AG_EV_KEY_DOWN) {
        return false;
    }

    const uint16_t mods = ev->key.mods;
    const uint16_t key = ev->key.keycode;

    /*
     * Break-in: Ctrl-Alt-Del (USB HID) or Ctrl+\ (terminal).  Swallowed.
     * First press → system shell; second within 1 s → kill last user app.
     */
    const bool ctrl_alt_del = (mods & AG_MOD_CTRL) && (mods & AG_MOD_ALT) &&
                              key == AG_KEY_DELETE;
    const bool ctrl_backslash = (mods & AG_MOD_CTRL) && !(mods & AG_MOD_ALT) &&
                                key == AG_KEY_BACKSLASH;

    if (ctrl_alt_del || ctrl_backslash) {
        s_stop_request = true;
        if (s_task != NULL) {
            xTaskNotifyGive(s_task);
        }
        return true;
    }

    /* Alt+1..4 → user slot; Alt+Tab → cycle user slots.  Sys is Ctrl+\ only. */
    if (mods & AG_MOD_ALT) {
        if (key >= AG_KEY_1 && key <= AG_KEY_4) {
            s_focus_slot_request = (int)(key - AG_KEY_1);
            if (s_task != NULL) {
                xTaskNotifyGive(s_task);
            }
            return true;
        }
        if (key == AG_KEY_TAB) {
            s_alt_tab_request = true;
            if (s_task != NULL) {
                xTaskNotifyGive(s_task);
            }
            return true;
        }
    }

    /*
     * Soft stop: Ctrl+C (often eaten by the QEMU host) and F12 (reliable guest
     * path under -Gfx).
     */
    if (((mods & AG_MOD_CTRL) && key == AG_KEY_C) || key == AG_KEY_F12) {
        request_soft_interrupt(ev);
        /* F12 is swallowed; Ctrl+C may become AG_EV_QUIT for the app. */
        return key == AG_KEY_F12;
    }

    return false;
}

static void interrupt_foreground(void)
{
    const ag_pid_t fg = ag_proc_foreground();
    if (fg != AG_PID_KERNEL) {
        (void)ag_proc_signal(fg);
    }
}

/*
 * The crash record on disk.  Written here rather than where the crash happened:
 * that task may be one instruction from ceasing to exist, or short of stack, and
 * writing to a filesystem is not the thing to attempt in either state.
 *
 * The journal tail goes in with it, because the useful part of a post-mortem is
 * usually not the fault itself but what the application was saying just before.
 */
#define AG_CRASH_PATH "/sys/crash.log"
#define AG_CRASH_KEEP_BYTES (32u * 1024u)
#define AG_CRASH_TAIL_LINES 20

static void write_crash_record(void)
{
    char text[512];

    if (!ag_proc_take_crash_record(text, sizeof(text))) {
        return;
    }

    /*
     * Bounded rather than rotated: a system that crashes often must not fill its
     * own filesystem, and the recent records are the ones anybody reads.
     */
    uint32_t  flags = AG_O_WRONLY | AG_O_CREATE;
    ag_stat_t st;
    if (ag_vfs_stat(AG_CRASH_PATH, NULL, &st) == AG_OK &&
        st.size > AG_CRASH_KEEP_BYTES) {
        flags |= AG_O_TRUNC;
    }

    const ag_handle_t h = ag_vfs_open(AG_CRASH_PATH, NULL, flags);
    if (h < 0) {
        ag_log(AG_LOG_WARN, "supervisor", "no crash record on disk: %s is %d",
               AG_CRASH_PATH, (int)h);
        return;
    }
    (void)ag_vfs_seek(h, 0, AG_SEEK_END);

    (void)ag_vfs_write(h, "\n--- crash ---\n", 15);
    (void)ag_vfs_write(h, text, strlen(text));
    (void)ag_vfs_write(h, "journal:\n", 9);

    const ag_journal_t *journal = ag_log_journal();
    const uint32_t      held = ag_journal_count(journal);
    const uint32_t      skip =
        (held > AG_CRASH_TAIL_LINES) ? (held - AG_CRASH_TAIL_LINES) : 0;

    ag_journal_iter_t it;
    ag_journal_begin(journal, &it);

    char     line[AG_JOURNAL_LINE_MAX];
    uint32_t index = 0;
    while (ag_journal_next(journal, &it, line, sizeof(line))) {
        if (index++ < skip) {
            continue;
        }
        (void)ag_vfs_write(h, "  ", 2);
        (void)ag_vfs_write(h, line, strlen(line));
        (void)ag_vfs_write(h, "\n", 1);
    }

    (void)ag_vfs_sync(h);
    (void)ag_vfs_close(h);

    ag_log(AG_LOG_INFO, "supervisor", "crash record appended to %s",
           AG_CRASH_PATH);
}

static void supervisor_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* Woken by a hotkey, or on the tick to collect what has finished. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(AG_SUP_TICK_MS));

        if (s_interrupt_request) {
            s_interrupt_request = false;
            interrupt_foreground();
        }
        if (s_stop_request) {
            s_stop_request = false;
            if (ag_session_enter_system()) {
                s_stops++;
            }
        }
        if (s_alt_tab_request) {
            s_alt_tab_request = false;
            (void)ag_session_alt_tab();
        }
        {
            const int slot = s_focus_slot_request;
            if (slot >= 0) {
                s_focus_slot_request = -1;
                (void)ag_session_focus(slot);
            }
        }
        /*
         * A process that asked to be ended from inside itself - a thread that
         * faulted, say.  Its crash record is already written; what is left is the
         * part it could not do for itself.
         */
        const ag_pid_t victim = s_kill_request;
        if (victim != AG_PID_KERNEL) {
            s_kill_request = AG_PID_KERNEL;
            (void)ag_proc_kill(victim, "a thread of it faulted");
        }

        /*
         * A process that promised to report progress and stopped.  Checked here
         * rather than by a timer, because this task is the one that is certain to
         * be scheduled while an application is spinning on the other core.
         */
        uint32_t       late = 0;
        const ag_pid_t hung = ag_proc_overdue(&late);
        if (hung != AG_PID_KERNEL) {
            char reason[64];
            snprintf(reason, sizeof(reason), "no heartbeat, %u ms past its own "
                                             "deadline",
                     (unsigned)late);
            (void)ag_proc_kill(hung, reason);
        }

        (void)ag_proc_reap_finished();

        /* Last, so that the journal tail it writes includes everything above. */
        write_crash_record();
    }
}

uint32_t ag_supervisor_stops(void) { return s_stops; }

bool ag_supervisor_running(void) { return s_task != NULL; }

bool ag_supervisor_interrupted(void) { return s_shell_interrupt; }

void ag_supervisor_clear_interrupt(void) { s_shell_interrupt = false; }

void ag_supervisor_raise_shell_interrupt(void) { s_shell_interrupt = true; }

ag_err_t ag_supervisor_init(void)
{
    if (s_task != NULL) {
        return AG_OK;
    }

    ag_err_t err = ag_proc_init();
    if (err != AG_OK) {
        return err;
    }

    ag_session_init();
    s_focus_slot_request = -1;

    /*
     * Before any application can run: from here on a fault in one costs that
     * application rather than the machine.
     */
    err = ag_fault_init();
    if (err != AG_OK) {
        ag_log(AG_LOG_WARN, "supervisor",
               "faults will not be caught (%d): an application that faults takes "
               "the system with it",
               (int)err);
    }

    if (xTaskCreatePinnedToCore(supervisor_task, "ag_super", AG_SUP_STACK, NULL,
                                AG_SUP_PRIORITY, &s_task, 0) != pdPASS) {
        return -AG_ENOMEM;
    }

    ag_console_set_hotkeys(hotkeys);
    return AG_OK;
}
