/*
 * ArgonOS port contract - catching a fault instead of rebooting.
 *
 * Without an MMU a wild pointer can reach anything, and that is a deliberate
 * trade [Т-12].  What is not acceptable is the consequence: an application that
 * dereferences nothing taking the whole system down with it.  Most faults are
 * not corruption at all - a null pointer, a bad cast, a jump through an
 * uninitialised function pointer - and the machine is perfectly healthy
 * afterwards.  Those should cost one process, not a reboot.
 *
 * This is the most architecture-bound thing the system asks for, and it is the
 * whole of what changes between Xtensa and RISC-V.  It is one file.
 *
 * What a port must supply:
 *
 *   ag_err_t ag_port_fault_init(ag_port_fault_note_fn note,
 *                               ag_port_fault_recover_fn recover)
 *   const char *ag_port_fault_cause_name(uint32_t cause)
 *
 * How it has to work, in the port's own terms:
 *
 * 1. Take over the causes that mean "the program is wrong" - illegal
 *    instruction, bad address, unaligned access, divide by zero - and leave the
 *    ones that are working machinery.  Register-window handling, interrupt
 *    dispatch and lazy FPU save all arrive as exceptions on some architectures,
 *    and taking those over breaks the system rather than protecting it.
 *
 * 2. On such a fault, call `note` with what the frame says.  It answers whether
 *    this fault belongs to an application that can be unwound.
 *
 * 3. If it does: arrange for the faulting task to resume at `recover`, in
 *    ordinary task context and on its own stack, rather than returning to the
 *    instruction that faulted.  `recover` does not return.
 *
 * 4. If it does not: chain to whatever the layer below would have done - the
 *    panic handler, with its full report and its reboot.  A fault that is not
 *    ours must not be swallowed.
 *
 * 5. On a multi-core machine, install on every core.  The application runs on
 *    the other one, and an exception table belongs to the core it is set on.
 *
 * What the handler must not do: take a lock, log, or allocate.  It runs in an
 * exception context where any of those is a second and worse failure.  `note` is
 * written to the same rule; everything that needs a lock happens in `recover`,
 * which is a normal task again.
 *
 * A port that cannot do this at all returns -AG_ENOTSUP.  The system still runs;
 * an application that faults then takes the machine with it, and the supervisor
 * says so at boot.
 *
 * The second half of the contract is the record a fault that is NOT recovered
 * leaves behind.  A fault the port must refuse - the kernel's own, one taken
 * while a kernel lock is held, a second one while the first is being unwound -
 * goes to the panic handler, and the panic handler reboots the machine.  The
 * crash log on disk cannot be used for that: it is written by the supervisor in
 * ordinary task context, and the supervisor never runs again.  So the port keeps
 * the last panic in memory that survives a reboot - the RTC domain on ESP32 -
 * and hands it over once, at the next boot, for the kernel to write into the
 * same crash record as everything else:
 *
 *   bool ag_port_fault_last_panic(ag_port_panic_t *out)
 *
 * It is raw numbers and not a string, because the machine that fills it can
 * neither format nor allocate: no lock, no heap, no flash.  A port that keeps no
 * such record answers false, and the consequence is the one this whole file is
 * about - a panic during an unattended soak leaves nothing to read.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_FAULT_H
#define ARGON_PORT_FAULT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <argon/abi.h>

/*
 * Called from the exception context.  Returns true when the fault belongs to an
 * application that can be unwound, false to let it take its normal course.
 *
 * `ra` is the faulting function's return address, which is where the program
 * came from and is the first thing anybody looks at after the PC itself.  Each
 * architecture names it differently - RA on RISC-V, a0 in an Xtensa window - and
 * the port is where that is translated.
 */
typedef bool (*ag_port_fault_note_fn)(uint32_t cause, uint32_t pc,
                                      uint32_t vaddr, uint32_t sp, uint32_t ra);

/* Where the faulting task resumes, in ordinary task context.  Never returns. */
typedef void (*ag_port_fault_recover_fn)(void);

ag_err_t ag_port_fault_init(ag_port_fault_note_fn note,
                            ag_port_fault_recover_fn recover);

/* "write to an invalid address", "illegal instruction" - for the record. */
const char *ag_port_fault_cause_name(uint32_t cause);

/*
 * A cause that is not a cause: the machine went down for a reason that is not an
 * exception of its own - a watchdog, a deliberate abort, a failed assertion.
 * The record still has the PC and the stack, which is usually enough.
 */
#define AG_PORT_FAULT_CAUSE_PANIC 0xffffffffu

/* How much of the faulting stack travels with the record.  A backtrace needs
 * more than this; a soak needs to know which call was running, and eight words
 * from the faulting SP is that. */
#define AG_PORT_FAULT_STACK_WORDS 8

/*
 * What a panic leaves behind.  Every field is a number the exception frame
 * already held: nothing here was produced by formatting, and that is the point.
 */
typedef struct {
    uint32_t cause;  /* mcause / exccause, or AG_PORT_FAULT_CAUSE_PANIC */
    uint32_t pc;     /* MEPC: where it faulted                          */
    uint32_t ra;     /* RA / a0: where that came from                   */
    uint32_t vaddr;  /* MTVAL / EXCVADDR: the address it could not use  */
    uint32_t sp;     /* the faulting stack pointer                      */
    /*
     * The first four bytes of the running image's ELF SHA256, so that a record
     * read months later can be matched to the ELF it belongs to.  The image SHA
     * changes on every build - the compile time is inside the descriptor - so
     * without this a decoded address is a guess.
     */
    uint32_t image;
    uint32_t words; /* how many of stack[] are real */
    uint32_t stack[AG_PORT_FAULT_STACK_WORDS];
} ag_port_panic_t;

/*
 * Hands over the record the last panic left behind.  False when there is none,
 * which is every boot that follows a clean reset.
 *
 * IT DOES NOT CLEAR IT.  That is deliberate and it is a fix, not a detail.  The
 * kernel reads this at boot and reports it a moment LATER - the supervisor
 * writes it to /sys/crash.log from a safe context, and on a board whose console
 * is not up yet that is a fraction of a second after the read.  Clearing here
 * meant a machine that died again in that fraction destroyed the record of the
 * FIRST panic and left nothing at all: measured on the C3, which panics again
 * shortly after boot, /sys/crash.log stayed empty across a whole loop of panics
 * because every boot consumed the record and then died before writing it.
 * A record that only a panic can leave must survive until somebody has actually
 * read it, so the port keeps it until the kernel says it is reported.
 */
bool ag_port_fault_last_panic(ag_port_panic_t *out);

/*
 * The record handed over by `ag_port_fault_last_panic` has been written down
 * (the supervisor calls this once /sys/crash.log holds it), so the port may
 * forget it.  Until this is called the record survives the next reboot and is
 * reported again - which is what makes an unattended panic visible even when
 * the machine goes down again before it could say anything.
 */
void ag_port_fault_panic_reported(void);

/*
 * Trap the next write to `bytes` at `addr`, on the given core.
 *
 * For watching a guard word: the hardware catches the instruction that writes,
 * which is the one thing a pattern in memory cannot tell you.  There is one
 * such watch in the system at a time (the chip has two per core and the RTOS
 * may hold one for stack overflow), so arming a second replaces the first.
 *
 * `bytes` must be a power of two, 1 to 64, and `addr` aligned to it.  `core`
 * is the core the watched task runs on; a watchpoint set on one core does not
 * exist on the other.
 *
 * A trap here is a debug exception, and a debug exception takes the panic
 * path: the board stops, prints the address of the writing instruction, and
 * reboots.  That is the trade - the machine for the culprit - so this is a
 * deliberate switch, not a defence.  The defence is the guard itself, which
 * the kernel checks on a timer.
 */
bool ag_port_watch_write(int core, const void *addr, size_t bytes);

/* Drop the watch set by ag_port_watch_write, if any. */
void ag_port_watch_clear(int core);

/*
 * Print past everything: no console, no log, no lock - straight to the UART
 * through the ROM.  For the report that has to come out of a machine whose
 * console is behind the very lock being reported on.  Busy-waits on the UART,
 * so it is for a line a second, not for logging.
 */
void ag_port_raw_print(const char *fmt, ...);

/*
 * Call `fn` every `period_ms` from a context that holds none of the kernel's
 * locks and does not wait for any of them, so that it still runs when the
 * kernel does not.  One such timer; a second call replaces the first.
 */
bool ag_port_lockwatch_start(void (*fn)(void), uint32_t period_ms);

#endif /* ARGON_PORT_FAULT_H */
