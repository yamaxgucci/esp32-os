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
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_FAULT_H
#define ARGON_PORT_FAULT_H

#include <stdbool.h>
#include <stdint.h>

#include <argon/abi.h>

/*
 * Called from the exception context.  Returns true when the fault belongs to an
 * application that can be unwound, false to let it take its normal course.
 */
typedef bool (*ag_port_fault_note_fn)(uint32_t cause, uint32_t pc,
                                      uint32_t vaddr, uint32_t sp);

/* Where the faulting task resumes, in ordinary task context.  Never returns. */
typedef void (*ag_port_fault_recover_fn)(void);

ag_err_t ag_port_fault_init(ag_port_fault_note_fn note,
                            ag_port_fault_recover_fn recover);

/* "write to an invalid address", "illegal instruction" - for the record. */
const char *ag_port_fault_cause_name(uint32_t cause);

#endif /* ARGON_PORT_FAULT_H */
