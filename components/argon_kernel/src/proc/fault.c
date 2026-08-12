/*
 * ArgonOS - catching a fault and blaming the right process.
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
#include "proc/proc_internal.h"

#include <xtensa/corebits.h>

#include "esp_ipc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xtensa_api.h"

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

const char *ag_fault_cause_name(uint32_t cause)
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
    ag_proc_fault_exit();

    /*
     * Unreachable: fault_exit does not return.  If it somehow did, going back to
     * the instruction that faulted would loop forever, so stop here instead and
     * let the watchdog have it.
     */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

static void fault_handler(XtExcFrame *frame)
{
    const uint32_t cause = (uint32_t)frame->exccause;

    /*
     * Nothing here locks, logs or allocates: this is an exception context, and
     * the process layer's note_fault is written to the same rule.  It answers
     * whether this fault belongs to a recoverable application.
     */
    if (ag_proc_note_fault(cause, (uint32_t)frame->pc, (uint32_t)frame->excvaddr,
                           (uint32_t)frame->a1)) {
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

ag_err_t ag_fault_init(void)
{
    if (s_installed) {
        return AG_OK;
    }

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
