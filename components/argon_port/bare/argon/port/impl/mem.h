/*
 * ArgonOS port: bare metal - memory.
 *
 * The first thing to write, because nothing compiles without it.
 *
 * What is needed here is an allocator with capability classes.  The two that
 * matter are AG_MEM_FAST (internal SRAM: executable, reachable from an ISR,
 * scarce) and AG_MEM_SLOW (external RAM: plentiful, unusable from an ISR).  A
 * machine with only one kind of memory answers every request from it and the
 * system runs - smaller, and with the loader unable to place large data
 * anywhere clever, but it runs.
 *
 * The second half is a heap over a caller-supplied block, and that is not
 * optional: a process allocates from its own arena so that ending it returns
 * everything in one operation.  Any small allocator will do (there are several
 * public-domain ones of a few hundred lines) as long as it can be handed a
 * block rather than owning the world.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_PORT_IMPL_MEM_H
#define ARGON_PORT_IMPL_MEM_H

#error "bare: no allocator yet.  See argon/port/mem.h for the contract."

/*
 * The shape of it, for when the #error above comes out:
 *
 *   #define AG_MEM_FAST 0x01u
 *   #define AG_MEM_SLOW 0x02u
 *   #define AG_MEM_BYTE 0x04u
 *   #define AG_MEM_DMA  0x08u
 *   #define AG_MEM_EXEC 0x10u
 *
 *   typedef struct ag_bare_heap *ag_port_heap_t;
 *
 * and one function per line in argon/port/mem.h.  They may be ordinary
 * functions in a .c file here rather than static inline: the inline rule exists
 * so that a port which is already a one-to-one match costs nothing, and a port
 * that has to implement the allocator itself is not that.
 */

#endif /* ARGON_PORT_IMPL_MEM_H */
