/*
 * ArgonOS port: ESP-IDF - catching a fault and blaming the right process.
 *
 * Two halves, and the file is split down the middle by the instruction set
 * because argon/port/fault.h says this is the one place where that shows: an
 * Xtensa half that works, and a RISC-V half that says so.
 *
 * Without an MMU a wild pointer can reach anything, and that is a deliberate
 * trade [Т-12].  What is not acceptable is the consequence: an application that
 * dereferences nothing taking the whole system down with it.  Most faults are
 * not corruption at all - a null pointer, a bad cast, a jump through an
 * uninitialised function pointer - and the machine is perfectly healthy
 * afterwards.  Those should cost one process, not a reboot.
 *
 * How it works.  Xtensa dispatches general exceptions through a table of
 * per-cause handlers, and the documented contract of that table is exactly what
 * is needed: "if the handler returns, the thread context will be restored, and
 * any values in the exception frame modified by the handler will be restored as
 * part of the context".  So the handler changes the return PC to point at a
 * recovery routine and returns.  The faulting task resumes - in ordinary task
 * context, on its own stack - inside code that reports what happened and ends
 * the process.
 *
 * What the handler must not do: take a lock, log, or allocate.  It runs in an
 * exception context where any of those would be a second, worse failure.  It
 * writes down what it saw and gets out; everything that needs a lock happens in
 * the recovery routine, which is a normal task again.
 *
 * When it steps aside.  If the faulting task is not an application, or it is
 * inside the kernel holding a lock, or it has already faulted once, the fault is
 * left to take its normal course - the panic handler, with its full report and a
 * reboot.  A process unwound out of a held kernel lock would leave that lock
 * held forever, which is a hung system: exactly what this is trying to avoid.
 *
 * One case is out of reach by construction: a fault taken while the flash cache
 * is disabled - during a flash write, say - cannot even fetch this handler, and
 * goes straight to the panic path.  That is the correct outcome anyway, and it is
 * why the panic handler is the one that lives in IRAM and this one does not.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/port/fault.h>

/* Wanted by both halves: the watchpoint at the end of this file is portable. */
#include "esp_cpu.h"
#include "esp_ipc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(__XTENSA__)

#include <xtensa/corebits.h>

#include "esp_rom_sys.h"
#include "xtensa_api.h"

#include <argon/port/task.h>

/*
 * The causes worth taking over: the ones that mean the program is wrong.
 *
 * Deliberately absent: SYSCALL (1), LEVEL1_INTERRUPT (4), ALLOCA (5) and the
 * coprocessor-disabled causes (32..39).  Those are working machinery - window
 * handling, interrupts, lazy FPU save - and taking them over would break the
 * system rather than protect it.
 */
static const int k_causes[] = {
    EXCCAUSE_ILLEGAL,               /* 0  */
    EXCCAUSE_INSTR_ERROR,           /* 2  */
    EXCCAUSE_LOAD_STORE_ERROR,      /* 3  */
    EXCCAUSE_DIVIDE_BY_ZERO,        /* 6  */
    EXCCAUSE_PC_ERROR,              /* 7  */
    EXCCAUSE_PRIVILEGED,            /* 8  */
    EXCCAUSE_UNALIGNED,             /* 9  */
    EXCCAUSE_INSTR_DATA_ERROR,      /* 12 */
    EXCCAUSE_LOAD_STORE_DATA_ERROR, /* 13 */
    EXCCAUSE_INSTR_ADDR_ERROR,      /* 14 */
    EXCCAUSE_LOAD_STORE_ADDR_ERROR, /* 15 */
    EXCCAUSE_INSTR_RING,            /* 18 */
    EXCCAUSE_INSTR_PROHIBITED,      /* 20 */
    EXCCAUSE_LOAD_STORE_RING,       /* 26 */
    EXCCAUSE_LOAD_PROHIBITED,       /* 28 */
    EXCCAUSE_STORE_PROHIBITED,      /* 29 */
};

#define AG_FAULT_CAUSE_MAX 40

static xt_exc_handler s_chain[AG_FAULT_CAUSE_MAX];
static bool           s_chain_saved;
static bool           s_installed;

static ag_port_fault_note_fn    s_note;
static ag_port_fault_recover_fn s_recover;

const char *ag_port_fault_cause_name(uint32_t cause)
{
    switch (cause) {
    case EXCCAUSE_ILLEGAL:               return "illegal instruction";
    case EXCCAUSE_INSTR_ERROR:           return "instruction fetch error";
    case EXCCAUSE_LOAD_STORE_ERROR:      return "load or store error";
    case EXCCAUSE_DIVIDE_BY_ZERO:        return "divide by zero";
    case EXCCAUSE_PC_ERROR:              return "jump to an illegal address";
    case EXCCAUSE_PRIVILEGED:            return "privileged instruction";
    case EXCCAUSE_UNALIGNED:             return "unaligned load or store";
    case EXCCAUSE_INSTR_DATA_ERROR:      return "bus error fetching code";
    case EXCCAUSE_LOAD_STORE_DATA_ERROR: return "bus error on data";
    case EXCCAUSE_INSTR_ADDR_ERROR:      return "bad address fetching code";
    case EXCCAUSE_LOAD_STORE_ADDR_ERROR: return "bad address on data";
    case EXCCAUSE_INSTR_RING:            return "code fetch not permitted";
    case EXCCAUSE_INSTR_PROHIBITED:      return "execute from a non-code address";
    case EXCCAUSE_LOAD_STORE_RING:       return "data access not permitted";
    case EXCCAUSE_LOAD_PROHIBITED:       return "read from an invalid address";
    case EXCCAUSE_STORE_PROHIBITED:      return "write to an invalid address";
    default:                             return "fault";
    }
}

/*
 * Where a faulted application lands.  A separate function, and not static, so
 * that its address is a plain constant to put in the frame and so the compiler
 * cannot decide to inline it into somewhere it does not belong.
 */
void ag_fault_trampoline(void);

void ag_fault_trampoline(void)
{
    s_recover();

    /*
     * Unreachable: recover does not return.  If it somehow did, going back to
     * the instruction that faulted would loop forever, so stop here instead and
     * let the watchdog have it.
     */
    for (;;) {
        ag_port_task_delay(AG_PORT_FOREVER);
    }
}

static void fault_handler(XtExcFrame *frame)
{
    const uint32_t cause = (uint32_t)frame->exccause;

    /*
     * Nothing here locks, logs or allocates: this is an exception context, and
     * the note callback is written to the same rule.  It answers whether this
     * fault belongs to a recoverable application.
     */
    if (s_note(cause, (uint32_t)frame->pc, (uint32_t)frame->excvaddr,
               (uint32_t)frame->a1)) {
        /*
         * Say it here, from the exception itself, with esp_rom_printf - the one
         * output that is safe in this context (it busy-writes UART0 through ROM,
         * no lock, no heap, the same call the panic handler uses).  The tidy
         * report comes later from a task context, but only if recovery gets
         * there: an application that faulted by corrupting its own stack can
         * take the unwind down with it, and then this line is the only record
         * of where it died.
         */
        esp_rom_printf("\n[fault] app pc=0x%08x addr=0x%08x cause=%u (%s)\n",
                       (uint32_t)frame->pc, (uint32_t)frame->excvaddr,
                       (unsigned)cause, ag_port_fault_cause_name(cause));
        frame->pc = (long)&ag_fault_trampoline;
        return;
    }

    /* Not ours to recover: the normal path, with its report and its reboot. */
    if (cause < AG_FAULT_CAUSE_MAX && s_chain[cause] != NULL) {
        s_chain[cause](frame);
    }
}

/* Runs once per core: the exception table belongs to the core it is set on. */
static void install_here(void *arg)
{
    (void)arg;

    for (size_t i = 0; i < sizeof(k_causes) / sizeof(k_causes[0]); i++) {
        const int         cause = k_causes[i];
        xt_exc_handler    previous = xt_set_exception_handler(cause,
                                                             fault_handler);
        if (!s_chain_saved && cause < AG_FAULT_CAUSE_MAX) {
            s_chain[cause] = previous;
        }
    }
}

ag_err_t ag_port_fault_init(ag_port_fault_note_fn note,
                            ag_port_fault_recover_fn recover)
{
    if (note == NULL || recover == NULL) {
        return -AG_EINVAL;
    }
    if (s_installed) {
        return AG_OK;
    }

    /* Set before any handler can run, which is the moment install_here returns. */
    s_note = note;
    s_recover = recover;

    install_here(NULL);
    s_chain_saved = true; /* the other core's table holds the same handlers */

#if portNUM_PROCESSORS > 1
    /*
     * The application runs on the other core, which has an exception table of
     * its own; the handler has to be installed there by something running there.
     */
    const BaseType_t other = (xPortGetCoreID() == 0) ? 1 : 0;
    if (esp_ipc_call_blocking((uint32_t)other, install_here, NULL) != ESP_OK) {
        return -AG_EIO;
    }
#endif

    s_installed = true;
    return AG_OK;
}

#else /* !__XTENSA__ - the RISC-V parts (C3, C6, H2, P4) */

/*
 * Not yet, and not because nobody got round to it.
 *
 * The Xtensa half above rests on one property of that architecture: a table of
 * per-cause handlers whose documented contract is that returning from a handler
 * restores the thread context *including whatever the handler changed in the
 * frame*.  That is what makes the trick legal - the handler moves the return PC
 * and returns, and the faulting task wakes up in the recovery routine on its own
 * stack.
 *
 * RISC-V on ESP-IDF has no such table.  Every exception lands in one vector
 * which calls panic_from_exception(), and the panic path does not return.  A
 * port would have to get in ahead of it - overriding esp_panic_handler, or the
 * vector itself - and then reconstruct the same guarantee by hand out of an
 * RvExcFrame: change mepc, restore the registers, mret.  That is a real piece
 * of work with a real way to be subtly wrong, and it is not the thing standing
 * between this system and a C6 board.
 *
 * So it answers -AG_ENOTSUP, which the contract allows in so many words: the
 * system runs, the supervisor says at boot that a faulting application takes
 * the machine with it, and it does - the panic handler prints its report and
 * reboots, which is what every other firmware on this chip does anyway.
 */
const char *ag_port_fault_cause_name(uint32_t cause)
{
    /* mcause exception codes, for the record the panic handler prints. */
    switch (cause) {
    case 0u:  return "instruction address misaligned";
    case 1u:  return "instruction access fault";
    case 2u:  return "illegal instruction";
    case 3u:  return "breakpoint";
    case 4u:  return "load address misaligned";
    case 5u:  return "load access fault";
    case 6u:  return "store address misaligned";
    case 7u:  return "store access fault";
    case 8u:  return "environment call";
    case 11u: return "environment call from machine mode";
    default:  return "fault";
    }
}

ag_err_t ag_port_fault_init(ag_port_fault_note_fn note,
                            ag_port_fault_recover_fn recover)
{
    (void)note;
    (void)recover;
    return -AG_ENOTSUP;
}

#endif /* __XTENSA__ */

/* ------------------------------------------------------------------------ */
/* Watching one address                                                       */
/* ------------------------------------------------------------------------ */

/*
 * The same on both architectures - esp_cpu_set_watchpoint is the portable
 * call - so it is written once, below the two halves above.
 *
 * Watchpoint 0: IDF takes number 1 for the end-of-stack watch when
 * CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK is on, and that one is worth more
 * than this one.
 */
#define AG_WATCH_SLOT 0

struct watch_req {
    const void *addr;
    size_t      bytes;
    bool        set;
    bool        ok;
};

static void watch_here(void *arg)
{
    struct watch_req *r = (struct watch_req *)arg;

    if (!r->set) {
        esp_cpu_clear_watchpoint(AG_WATCH_SLOT);
        r->ok = true;
        return;
    }
    r->ok = (esp_cpu_set_watchpoint(AG_WATCH_SLOT, r->addr, r->bytes,
                                    ESP_CPU_WATCHPOINT_STORE) == ESP_OK);
}

static bool watch_on_core(int core, struct watch_req *r)
{
#if portNUM_PROCESSORS > 1
    if (core >= 0 && core != (int)xPortGetCoreID()) {
        if (esp_ipc_call_blocking((uint32_t)core, watch_here, r) != ESP_OK) {
            return false;
        }
        return r->ok;
    }
#else
    (void)core;
#endif
    watch_here(r);
    return r->ok;
}

bool ag_port_watch_write(int core, const void *addr, size_t bytes)
{
    /* The hardware's own rules: a power of two up to 64, aligned to itself. */
    if (addr == NULL || bytes == 0u || bytes > 64u ||
        (bytes & (bytes - 1u)) != 0u ||
        ((uintptr_t)addr & (uintptr_t)(bytes - 1u)) != 0u) {
        return false;
    }
    struct watch_req r = {.addr = addr, .bytes = bytes, .set = true,
                          .ok = false};
    return watch_on_core(core, &r);
}

void ag_port_watch_clear(int core)
{
    struct watch_req r = {.addr = NULL, .bytes = 0, .set = false, .ok = false};
    (void)watch_on_core(core, &r);
}

/* ------------------------------------------------------------------------ */
/* Speaking past the kernel                                                  */
/* ------------------------------------------------------------------------ */

#include <stdarg.h>
#include <stdio.h>

#include "esp_rom_sys.h"
#include "esp_timer.h"

void ag_port_raw_print(const char *fmt, ...)
{
    char    buf[160];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    esp_rom_printf("%s", buf);
}

static esp_timer_handle_t s_lockwatch;
static void (*s_lockwatch_fn)(void);

static void lockwatch_tick(void *arg)
{
    (void)arg;
    if (s_lockwatch_fn != NULL) {
        s_lockwatch_fn();
    }
}

bool ag_port_lockwatch_start(void (*fn)(void), uint32_t period_ms)
{
    if (fn == NULL || period_ms == 0u) {
        return false;
    }
    s_lockwatch_fn = fn;
    if (s_lockwatch == NULL) {
        const esp_timer_create_args_t args = {
            .callback = lockwatch_tick,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "lockwatch",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &s_lockwatch) != ESP_OK) {
            return false;
        }
    } else {
        (void)esp_timer_stop(s_lockwatch);
    }
    return esp_timer_start_periodic(s_lockwatch,
                                    (uint64_t)period_ms * 1000ull) == ESP_OK;
}
