/*
 * ArgonOS - execution budgets.
 *
 * Strict priorities are a contract, not a ranking.  A task above the
 * applications is not saying "I matter more"; it is saying "give me the core
 * and I will give it straight back".  Nothing in a strict-priority scheduler
 * checks that, and on 5 September 2026 a display driver broke it by accident:
 * its sender ran at 6, applications run at 5, and it stayed runnable for most
 * of every frame it sent.  Fallout fell from 40 frames a second to 1.5, and
 * six other explanations were measured and buried before the priority was
 * looked at.  See [[argonos-wire-starves-apps]] in the project notes.
 *
 * So the contract is enforced here.  A task that wants to run above the
 * applications declares how long it will hold the core; it does not declare,
 * it gets AG_BUDGET_DEFAULT_MS.  The kernel charges it a millisecond at a time
 * from the system tick, and when it spends more than its budget inside one
 * window it is dropped below the applications until the window turns over.
 *
 * Only module tasks are watched - the drivers, which is where code from
 * outside this tree runs.  The kernel's own tasks are audited by reading them.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_BUDGET_H
#define ARGON_BUDGET_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The ABI before the port, and it is not a matter of taste: abi.h spells its
 * memory capabilities as an enum - AG_MEM_FAST and the rest - and the port
 * spells the same names as macros over the IDF's own bits.  Include the port
 * first and the macro is substituted into the enum, which fails to compile in
 * a way that names neither file.  See [[argonos-mem-caps-bug]].
 */
#include <argon/abi.h>

#include <argon/port/task.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a task gets if it never says otherwise, and how long a window is. */
#define AG_BUDGET_DEFAULT_MS 20u
#define AG_BUDGET_WINDOW_MS  100u

/*
 * The band a task is dropped into when it overruns: above applications that
 * asked to be low (2), below the ordinary ones (5).  A driver being punished
 * should still outrun something that volunteered to be last.
 */
#define AG_BUDGET_DEMOTED_PRIO 3u

/* Hook the system tick.  Called once, from the kernel's own start-up. */
void ag_budget_init(void);

/*
 * Watch a task, or stop watching it.  `base_prio` is the priority it is
 * restored to; `budget_ms` of 0 means AG_BUDGET_DEFAULT_MS.  A task at or
 * below the ordinary application priority is not watched at all - it cannot
 * starve what it does not outrank - and watching one is harmless and ignored.
 */
void ag_budget_watch(ag_port_task_t task, const char *name, unsigned base_prio,
                     uint32_t budget_ms);
void ag_budget_forget(ag_port_task_t task);

/*
 * Apply what the tick decided: demote whoever overran, restore whoever has a
 * fresh window.  Priorities cannot be changed from an interrupt, so the tick
 * only marks and the supervisor calls this.  Returns true if anything changed,
 * which is only of interest to a caller that wants to log it.
 */
/*
 * Is anything being watched at all?  The supervisor looks round more often
 * while a driver is under a budget and goes back to its lazy quarter-second
 * when none is: the tick can only mark an overrun, so how fast the punishment
 * lands is how fast somebody in task context notices.
 */
bool ag_budget_watching(void);

bool ag_budget_service(void);

/* For `ps`-like reporting: how much of its budget each watched task used in
 * the window that just closed.  Returns false when `index` is past the end. */
bool ag_budget_report(uint32_t index, const char **name, uint32_t *used_ms,
                      uint32_t *budget_ms, bool *demoted);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_BUDGET_H */
