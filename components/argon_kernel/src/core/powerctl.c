/*
 * ArgonOS - doing what power.c decided, including the part that hurts.
 *
 * The clock through argon/port/power.h, the screen through whichever driver
 * owns the panel, the idle timer off the supervisor's tick, and - on a
 * transition a person asked for - ending the processes that did not say they
 * could live with it.  One line in the journal either way, so that a board
 * found running slowly, or short of an application, can be asked why.
 *
 * Nothing here decides what the machine should be: the announcing, the waiting
 * and the answers live next door in power.c, where they can be tested without a
 * machine under them.  What is here is everything that needs one.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/power.h>

#include <stdio.h>

#include <argon/cfg.h>
#include <argon/console.h>
#include <argon/device.h>
#include <argon/display.h>
#include <argon/log.h>
#include <argon/textpanel.h>

#include <argon/port/power.h>
#include <argon/port/sync.h>
#include <argon/port/task.h>
#include <argon/port/time.h>

#include "core/sysconfig.h"

/* How often the wait loop looks again.  Ten milliseconds is well inside a
 * frame, so an application polling once a frame is never the reason the
 * command feels slow. */
#define AG_POWERCTL_STEP_MS 10

/*
 * Defaults for the idle timer, used when SYSTEM.CFG says nothing.  Off, and
 * that is deliberate: a machine that puts its own screen out after a minute is
 * a pleasant laptop and an alarming instrument, and which of the two this is
 * belongs to whoever installed it.  `power auto on` and [power] in SYSTEM.CFG
 * both turn it on.
 */
#define AG_POWER_AUTO_SCREEN_S 60
#define AG_POWER_AUTO_ECO_S 300

/*
 * How long after the last process needed the full clock before the machine
 * cruises again.  Five seconds is hysteresis, not politeness: a `.bat` that
 * starts one program after another would otherwise flap between two clock
 * settings, announcing each one, and the announcing is the expensive part.
 */
#define AG_POWER_CRUISE_S 5

static bool            s_backlight_ok;
static ag_power_auto_t s_auto;

/*
 * Two tasks can arrive here, and on this part they can be two cores: the shell,
 * when a person types the command, and the supervisor, on the idle tick.  The
 * table in power.c is written for one transition at a time and says so; this is
 * where that is made true.
 *
 * Recursive, because the tick decides under the lock and then calls apply(),
 * which takes it again.  A person's transition waits for the tick's - a tick
 * holds it for at most the automatic grace period - while the tick does not
 * wait at all: if somebody is at the console changing modes, a timer has
 * nothing useful to add.
 */
static ag_port_mutex_t s_lock;

/*
 * Did the idle timer put the machine where it is?
 *
 * The rule this implements: the timer may lower the machine only from the
 * resting state, and may raise only what it lowered itself.  Without it, `power
 * eco` typed by a person would be undone by the next keypress - and a command
 * that a timer can quietly reverse is not a command.
 */
static bool s_auto_owns;

/*
 * When something last needed the full clock.  Read against AG_POWER_CRUISE_S;
 * zero at boot means the machine starts cruising once that hold-off has passed
 * since it came up, which is also the right answer - nothing has asked for
 * anything yet.
 */
static uint32_t s_last_demand_ms;

static uint32_t now_ms(void) { return (uint32_t)(ag_port_us() / 1000); }

/* NULL means the mutex could not be made: carry on unserialised rather than
 * refuse to change modes at all (see ag_powerctl_init). */
static bool lock_take(ag_port_ticks_t ticks)
{
    return (s_lock == NULL) || ag_port_mutex_take_recursive(s_lock, ticks);
}

static void lock_give(void)
{
    if (s_lock != NULL) {
        (void)ag_port_mutex_give_recursive(s_lock);
    }
}

static const char *mode_name(ag_power_mode_t m)
{
    switch (m) {
    case AG_POWER_FULL:   return "full";
    case AG_POWER_CRUISE: return "cruise";
    case AG_POWER_ECO:    return "eco";
    case AG_POWER_DOZE:   return "doze";
    default:              return "?";
    }
}

/* ---------------------------------------------------------------------- */

uint32_t ag_powerctl_cpu_mhz(void) { return ag_port_power_cpu_mhz(); }

uint32_t ag_powerctl_cpu_steps(uint16_t *out, uint32_t max)
{
    return ag_port_power_cpu_steps(out, max);
}

bool ag_powerctl_can_scale(void)
{
    return (ag_port_power_caps() & AG_PORT_PWR_CPU_BAND) != 0u;
}

bool ag_powerctl_backlight_works(void) { return s_backlight_ok; }

static uint32_t cpu_max_mhz(void)
{
    uint16_t       steps[AG_PORT_PWR_STEPS_MAX];
    const uint32_t n = ag_port_power_cpu_steps(steps, AG_PORT_PWR_STEPS_MAX);
    /*
     * out[0] is the maximum by contract.  A port that answered nothing at all
     * would leave the system with no idea how fast it is; the live reading is
     * the honest fallback, and it is what this machine is running at right now
     * by definition.
     */
    return (n > 0u) ? (uint32_t)steps[0] : ag_port_power_cpu_mhz();
}

/*
 * Eighty megahertz where the part has it: a third of full speed, still a PLL
 * setting, so the peripheral bus keeps a frequency every driver in the tree has
 * been run at.  Dropping to the crystal is a bigger saving and a bigger risk,
 * so it is available by name and is not the default.
 */
uint32_t ag_powerctl_eco_default(void)
{
    uint16_t       steps[AG_PORT_PWR_STEPS_MAX];
    const uint32_t n = ag_port_power_cpu_steps(steps, AG_PORT_PWR_STEPS_MAX);
    if (n == 0u) {
        return 0u;
    }
    for (uint32_t i = 0; i < n && i < AG_PORT_PWR_STEPS_MAX; i++) {
        if (steps[i] == 80u) {
            return 80u;
        }
    }
    const uint32_t last =
        (n < AG_PORT_PWR_STEPS_MAX) ? n : AG_PORT_PWR_STEPS_MAX;
    return (uint32_t)steps[last - 1];
}

uint32_t ag_powerctl_cruise_default(void)
{
    uint16_t       steps[AG_PORT_PWR_STEPS_MAX];
    const uint32_t n = ag_port_power_cpu_steps(steps, AG_PORT_PWR_STEPS_MAX);
    for (uint32_t i = 0; i < n && i < AG_PORT_PWR_STEPS_MAX; i++) {
        if (steps[i] == 160u) {
            return 160u;
        }
    }
    /* No middle step: cruising would mean either full speed or a third of it,
     * and a third of it is a decision, not a default. */
    return 0u;
}

ag_err_t ag_powerctl_init(void)
{
    if (s_lock == NULL) {
        /*
         * Without it the two callers race, which on this table means one
         * transition's answers counted against the other's deadline.  Not fatal
         * - the system runs, changes modes, and only a simultaneous command and
         * tick can confuse it - so it is a line in the journal and not a failed
         * boot stage.
         */
        s_lock = ag_port_mutex_new_recursive();
        if (s_lock == NULL) {
            ag_log(AG_LOG_WARN, "power",
                   "no mutex; a command and the idle timer at the same moment "
                   "can disagree");
        }
    }

    ag_power_init(cpu_max_mhz());
    s_backlight_ok = false;
    s_auto_owns = false;

    const ag_cfg_t *cfg = ag_sysconfig();
    s_auto.on = ag_cfg_get_bool(cfg, "power.auto", false);
    s_auto.screen_off_s = (uint32_t)ag_cfg_get_int(cfg, "power.screen_off_s",
                                                  AG_POWER_AUTO_SCREEN_S);
    s_auto.eco_s =
        (uint32_t)ag_cfg_get_int(cfg, "power.eco_s", AG_POWER_AUTO_ECO_S);
    s_auto.eco_mhz = (uint32_t)ag_cfg_get_int(cfg, "power.eco_mhz",
                                              (int32_t)ag_powerctl_eco_default());

    s_auto.cruise_mhz = (uint32_t)ag_cfg_get_int(
        cfg, "power.cruise_mhz", (int32_t)ag_powerctl_cruise_default());
    s_auto.cruise_s =
        (uint32_t)ag_cfg_get_int(cfg, "power.cruise_s", AG_POWER_CRUISE_S);

    /*
     * `silence` is the default, and it is the whole point of this rung: a
     * process that has said nothing is assumed to manage at 160 MHz, because
     * two thirds of the speed and an unchanged peripheral bus break only work
     * that was already at the edge.  `declared` inverts it for an installation
     * that runs images it did not build.
     */
    const char *when = ag_cfg_get(cfg, "power.cruise_when", "silence");
    s_auto.cruise_strict = (when != NULL) && (when[0] == 'd' || when[0] == 'D');

    s_last_demand_ms = 0u;
    return AG_OK;
}

void ag_powerctl_auto_get(ag_power_auto_t *out)
{
    if (out != NULL) {
        *out = s_auto;
    }
}

ag_err_t ag_powerctl_auto_set(const ag_power_auto_t *in)
{
    if (in == NULL) {
        return -AG_EINVAL;
    }
    if (!lock_take(AG_PORT_FOREVER)) {
        return -AG_EBUSY;
    }
    s_auto = *in;
    if (s_auto.eco_mhz == 0u) {
        s_auto.eco_mhz = ag_powerctl_eco_default();
    }
    lock_give();
    return AG_OK;
}

/* ---------------------------------------------------------------------- */

/*
 * The panel is asked rather than told about, for the same reason the console
 * asks it to draw a row: the driver is loadable, so the pin the backlight is on
 * belongs to code that may not be there.  Every display in the registry gets
 * the request; the ones that answer -AG_ENOTSUP have no light to switch.
 */
static void backlight_broadcast(uint8_t percent)
{
    bool taken = false;

    for (uint32_t i = 0;; i++) {
        ag_devinfo_t info;
        if (ag_dev_info(i, AG_DEV_DISPLAY, &info) != AG_OK) {
            break;
        }
        ag_device_t *dev = ag_dev_find(info.name);
        if (dev == NULL) {
            continue;
        }
        uint8_t        arg = percent;
        const ag_err_t err =
            ag_dev_ioctl(dev, AG_IOC_DISPLAY_BACKLIGHT, &arg, sizeof(arg));
        if (err == AG_OK) {
            taken = true;
        }
    }

    s_backlight_ok = taken;
}

static void apply_screen(bool on)
{
    /*
     * Order matters on the way down and on the way up, and it is not the same
     * order.  Going dark: stop sending pixels first, then put the light out, so
     * nothing is half drawn on a panel that is about to go black.  Coming back:
     * light first, then repaint, because a panel lit with last week's contents
     * on it for one tick is what a person reads as a glitch.
     */
    if (!on) {
        ag_display_power(false);
        ag_textpanel_enable(false);
        backlight_broadcast(0);
    } else {
        backlight_broadcast(100);
        ag_display_power(true);
        ag_textpanel_enable(true);
    }
}

static ag_err_t apply_clock(const ag_power_target_t *t)
{
    if (!ag_powerctl_can_scale()) {
        return -AG_ENOTSUP;
    }
    return ag_port_power_cpu_band(t->cpu_min_mhz, t->cpu_max_mhz);
}

/* Put the table back where it was, when the machine refused what it said. */
static void table_restore(const ag_power_target_t *prev, ag_power_cause_t cause)
{
    if (ag_power_begin(prev, cause, now_ms(), 0u) == AG_OK) {
        (void)ag_power_commit(NULL);
    }
}

static bool same_target(const ag_power_target_t *a, const ag_power_target_t *b)
{
    return a->mode == b->mode && a->screen_on == b->screen_on &&
           a->cpu_min_mhz == b->cpu_min_mhz && a->cpu_max_mhz == b->cpu_max_mhz;
}

/*
 * Everything that did not say it could live with the new mode, ended.
 *
 * Only on a transition a person asked for, and only when the mode itself went
 * down.  Two deliberate asymmetries:
 *
 * - The screen going out never costs a process its life.  An application that
 *   draws has somewhere to put the decision to stop - it stops drawing -
 *   whereas one whose arithmetic does not fit in a slower processor has nowhere
 *   to put it at all.
 *
 * - The mode is what counts, not the megahertz.  On a build that cannot move
 *   its clock the mode still means "this machine is being asked to save power",
 *   and an application that cannot answer that is no more fit there than
 *   anywhere else.
 *
 * The pids are collected before anything is killed: ag_proc_info walks used
 * slots, and ending one while walking would renumber the rest.
 */
static uint32_t sweep(ag_power_mode_t mode, bool refusers_only,
                     ag_power_ended_t *out, uint32_t max)
{
    ag_power_ended_t victims[AG_PROC_MAX];
    uint32_t         n = 0;

    for (uint32_t i = 0; i < AG_PROC_MAX; i++) {
        ag_procinfo_t info;
        if (ag_proc_info(i, &info) != AG_OK) {
            break;
        }
        if (info.pid == AG_PID_KERNEL) {
            continue;
        }
        /*
         * On the mild rung only an explicit refusal counts, because that rung
         * is entered on silence in the first place: ending a process for not
         * answering a question the system had already decided to answer on its
         * behalf would be incoherent.  Lower down, silence is not consent and
         * anything unproven goes.
         */
        const bool doomed = refusers_only ? ag_power_refused(info.pid)
                                          : !ag_power_fit(info.pid);
        if (!doomed) {
            continue;
        }
        /*
         * A process that has not started yet is not asked, and found out by
         * being killed mid-load in the emulator - which is also the proof that
         * this matters: whether it had run a single instruction by the time the
         * command arrived depended on how fast the image came off the disk.
         *
         * Nothing is lost by sparing it.  It comes up into the mode that is
         * already set, sees it in its first ag_power_status(), and decides
         * there - which is the same decision, taken with better information.
         * A zombie is skipped for the plainer reason that it is already gone.
         */
        if (info.state == AG_PS_LOADING || info.state == AG_PS_ZOMBIE) {
            continue;
        }
        victims[n].pid = info.pid;
        /* The precision is not decoration: a process name is twice the width of
         * the row it is copied into, and the compiler will not take snprintf's
         * word for it. */
        snprintf(victims[n].name, sizeof(victims[n].name), "%.*s",
                 (int)sizeof(victims[n].name) - 1, info.name);
        snprintf(victims[n].why, sizeof(victims[n].why), "%.*s",
                 (int)sizeof(victims[n].why) - 1, ag_power_why(info.pid));
        n++;
    }

    for (uint32_t i = 0; i < n; i++) {
        char reason[80];

        if (victims[i].why[0] != '\0') {
            snprintf(reason, sizeof(reason), "not fit for %s: %.*s",
                     mode_name(mode), (int)sizeof(victims[i].why) - 1,
                     victims[i].why);
        } else {
            snprintf(reason, sizeof(reason),
                     "did not answer that it is fit for %s", mode_name(mode));
        }
        (void)ag_proc_kill(victims[i].pid, reason);
        if (out != NULL && i < max) {
            out[i] = victims[i];
        }
    }
    return n;
}

/*
 * `by` is what the journal calls whoever started this.  The cause says what the
 * transition is allowed to do; this says who asked, and they are not the same
 * question - a raise triggered by an application declaring that it needs the
 * clock is advisory like the timer's, and reading "full by idle timer" when no
 * timer was involved sends the next person looking in the wrong place.
 */
static ag_err_t apply_locked(const ag_power_target_t *want,
                            ag_power_cause_t cause, const char *by,
                            ag_power_ended_t *ended, uint32_t max,
                            uint32_t *n_ended)
{

    ag_power_target_t prev;
    ag_power_current(&prev);

    ag_power_target_t asked = *want;
    bool              clock_moves = (asked.cpu_min_mhz != prev.cpu_min_mhz) ||
                       (asked.cpu_max_mhz != prev.cpu_max_mhz);

    /*
     * A machine whose clock is fixed still has modes.  Two of the three things
     * a mode does - the screen, and telling the applications - work here
     * exactly as they do anywhere, so the mode is entered and only the clock
     * stays put.
     *
     * What must not happen is the table claiming a frequency the chip is not
     * running at: `power` would then print eighty megahertz on a machine doing
     * two hundred and forty, which is the one thing this command exists to be
     * able to trust.  So the band asked for is replaced with the band there is,
     * before anybody is told about it - the applications are asked about what
     * will actually happen, not about what was typed.
     */
    if (clock_moves && !ag_powerctl_can_scale()) {
        asked.cpu_min_mhz = prev.cpu_min_mhz;
        asked.cpu_max_mhz = prev.cpu_max_mhz;
        clock_moves = false;
    }

    if (same_target(&asked, &prev)) {
        return AG_OK; /* nothing to announce, nothing to do */
    }

    const uint32_t grace =
        (cause == AG_POWER_USER) ? AG_POWER_GRACE_MS : AG_POWER_AUTO_GRACE_MS;

    ag_err_t err = ag_power_begin(&asked, cause, now_ms(), grace);
    if (err != AG_OK) {
        return err;
    }

    while (!ag_power_settled(now_ms())) {
        ag_port_task_delay(ag_port_ms_to_ticks(AG_POWERCTL_STEP_MS));
    }

    ag_power_target_t applied;
    err = ag_power_commit(&applied);
    if (err != AG_OK) {
        return err;
    }

    if (clock_moves) {
        const ag_err_t clk = apply_clock(&applied);
        if (clk != AG_OK) {
            /*
             * The machine said no after the table said yes.  Anything but
             * putting the table back would leave `power` printing a frequency
             * the chip is not running at, which is the one thing this command
             * exists to be able to trust.  Nothing is ended in this case: the
             * mode nobody is in cannot be a reason to end anything.
             */
            table_restore(&prev, cause);
            ag_log(AG_LOG_ERROR, "power",
                   "%u-%u MHz refused by the machine (%d); staying at %u-%u",
                   (unsigned)applied.cpu_min_mhz, (unsigned)applied.cpu_max_mhz,
                   (int)clk, (unsigned)prev.cpu_min_mhz,
                   (unsigned)prev.cpu_max_mhz);
            return clk;
        }
    }

    if (applied.screen_on != prev.screen_on) {
        apply_screen(applied.screen_on);
    }

    uint32_t gone = 0;
    if (cause == AG_POWER_USER && applied.mode > prev.mode) {
        gone = sweep(applied.mode, applied.mode == AG_POWER_CRUISE, ended, max);
        if (n_ended != NULL) {
            *n_ended = gone;
        }
    }

    uint32_t parked = 0, silent = 0, declared = 0;
    for (uint32_t i = 0;; i++) {
        ag_power_watcher_t w;
        if (!ag_power_watcher_at(i, now_ms(), &w)) {
            break;
        }
        if (w.fitness != AG_POWER_FIT_ASK) {
            declared++;
        } else if (w.answer == AG_POWER_PARKED) {
            parked++;
        } else if (w.answer == AG_POWER_ANSWER_NONE && w.listening) {
            silent++;
        }
    }

    ag_log(AG_LOG_INFO, "power",
           "%s by %s: cpu %u-%u MHz, screen %s (%u parked, %u silent, "
           "%u declared, %u ended)",
           mode_name(applied.mode), by,
           (unsigned)applied.cpu_min_mhz, (unsigned)applied.cpu_max_mhz,
           applied.screen_on ? "on" : "off", (unsigned)parked,
           (unsigned)silent, (unsigned)declared, (unsigned)gone);

    s_auto_owns = (cause == AG_POWER_AUTO) && (applied.mode > AG_POWER_FULL ||
                                               !applied.screen_on);
    return AG_OK;
}

ag_err_t ag_powerctl_apply(const ag_power_target_t *want,
                           ag_power_cause_t cause, ag_power_ended_t *ended,
                           uint32_t max, uint32_t *n_ended)
{
    if (n_ended != NULL) {
        *n_ended = 0;
    }
    if (want == NULL) {
        return -AG_EINVAL;
    }
    if (!lock_take(AG_PORT_FOREVER)) {
        return -AG_EBUSY;
    }
    const ag_err_t err = apply_locked(
        want, cause, (cause == AG_POWER_USER) ? "command" : "idle timer", ended,
        max, n_ended);
    lock_give();
    return err;
}

/* ---------------------------------------------------------------------- */

/*
 * Is anything running that needs the full clock?
 *
 * Two readings of the same question, and which one is in force is an
 * installation's decision (`[power] cruise_when`).
 *
 * `silence` - the default - asks only whether somebody has said so.  Nothing
 * said means nothing needed: on this rung that is a safe assumption, because
 * two thirds of the clock with an unmoved peripheral bus breaks only work that
 * was already at the edge, and the handful of programs in this tree that are
 * (the audio paths, the emulators, the benchmarks) say so in their first line.
 *
 * `declared` is for a machine running images somebody else built: there, only a
 * process that declared AG_POWER_FIT_ANY lets the clock down, and an unknown
 * image keeps the machine at full speed until it says otherwise.
 */
static bool demands_full(void)
{
    if (!s_auto.cruise_strict) {
        return ag_power_full_only_held();
    }

    for (uint32_t i = 0; i < AG_PROC_MAX; i++) {
        ag_procinfo_t info;
        if (ag_proc_info(i, &info) != AG_OK) {
            break;
        }
        if (info.pid == AG_PID_KERNEL || info.state == AG_PS_ZOMBIE) {
            continue;
        }
        if (ag_power_fitness_of(info.pid) != AG_POWER_FIT_ANY) {
            return true;
        }
    }
    return false;
}

void ag_powerctl_declared(ag_power_fitness_t fitness)
{
    if (fitness != AG_POWER_FIT_FULL_ONLY) {
        return;
    }
    /*
     * Not waiting for the lock: a transition already in flight will read the
     * declaration when it decides, and the tick after it will see the hold-off.
     * Blocking an application inside a declaration to wait for the shell is a
     * worse trade than a quarter of a second at the wrong clock.
     */
    if (!lock_take(0)) {
        return;
    }
    s_last_demand_ms = now_ms();

    ag_power_target_t now;
    ag_power_current(&now);
    if (s_auto_owns && now.mode == AG_POWER_CRUISE) {
        ag_power_target_t want = now;
        want.mode = AG_POWER_FULL;
        want.cpu_min_mhz = cpu_max_mhz();
        want.cpu_max_mhz = want.cpu_min_mhz;
        (void)apply_locked(&want, AG_POWER_AUTO, "a declaration", NULL, 0u,
                           NULL);
    }
    lock_give();
}

void ag_powerctl_tick(void)
{
    /*
     * Nought ticks: a transition in flight means somebody else is deciding
     * this, and the next tick is a quarter of a second away.
     */
    if (!lock_take(0)) {
        return;
    }

    ag_power_target_t now;
    ag_power_current(&now);

    const bool resting = (now.mode == AG_POWER_FULL) && now.screen_on;
    if (!resting && !s_auto_owns) {
        /*
         * A person put the machine here.  The timer does not undo that: a
         * command a keypress can reverse is not a command, and `power eco`
         * typed deliberately has to survive somebody touching the keyboard.
         */
        lock_give();
        return;
    }

    const uint32_t ms = now_ms();
    const uint32_t idle_ms = ag_console_idle_ms();
    const uint32_t max_mhz = cpu_max_mhz();

    /*
     * The first rung, and the only one that is not about time: cruise while
     * nothing needs the speed.  The hold-off is measured from the last moment
     * something did, so a program that ends does not put the machine back to
     * 160 MHz before the next one has started.
     */
    if (demands_full()) {
        s_last_demand_ms = ms;
    }
    const bool cruise = (s_auto.cruise_mhz > 0u) &&
                        ((uint32_t)(ms - s_last_demand_ms) >=
                         s_auto.cruise_s * 1000u);

    /* The other two rungs are the idle timer's, and it can be switched off
     * without taking cruising with it. */
    const bool screen_off = s_auto.on && (s_auto.screen_off_s > 0u) &&
                            (idle_ms >= s_auto.screen_off_s * 1000u);
    /*
     * eco is the rung where a third of the clock is at stake, so it keeps the
     * stricter rule whatever cruising is set to: nothing is lowered that far
     * while any process has said it needs the machine.
     */
    const bool eco = s_auto.on && (s_auto.eco_s > 0u) &&
                     (idle_ms >= s_auto.eco_s * 1000u) &&
                     !ag_power_full_only_held();

    ag_power_target_t want = now;
    want.screen_on = !screen_off;
    if (eco) {
        want.cpu_min_mhz = s_auto.eco_mhz;
        want.mode = screen_off ? AG_POWER_DOZE : AG_POWER_ECO;
    } else if (cruise) {
        want.cpu_min_mhz = s_auto.cruise_mhz;
        want.mode = AG_POWER_CRUISE;
    } else {
        want.cpu_min_mhz = max_mhz;
        /* A dark screen at full speed is not a mode of its own: `doze` would be
         * a lie in the one place a person looks to find out. */
        want.mode = AG_POWER_FULL;
    }
    want.cpu_max_mhz = want.cpu_min_mhz;

    if (same_target(&want, &now)) {
        lock_give();
        return;
    }

    (void)apply_locked(&want, AG_POWER_AUTO, "idle timer", NULL, 0u, NULL);
    lock_give();
}
