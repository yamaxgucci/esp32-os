/*
 * ArgonOS - the portable instruction set: what CC writes with `-bc` and what
 * AGVM.AXE runs.  The third rung of the cascade in docs/plans/cluster.md 3b.
 *
 * WHY A THIRD MACHINE AT ALL.  A cluster that spans instruction sets passes
 * messages and shares devices without noticing the line; code stops at it dead.
 * Rung 1 answers that by identity - the far node runs ITS OWN build of the same
 * program - and rung 2 by compiling from source where there is memory to do it.
 * Both need the program to exist for the far machine.  This rung is the answer
 * when neither holds: ONE image that every node can run, at five to fifty times
 * the cost.  For coordination - policy, supervision, glue - that costs nothing,
 * because such a program sleeps between events.  For audio it is unacceptable,
 * and saying which is which is part of the design rather than a caveat.
 *
 * THE SHAPE IS THE COMPILER'S SEAM, deliberately.  cc_compile.c already talks
 * to its back ends in operations over sixteen numbered registers - 8 the value
 * in hand, 9 the other operand, 7 the frame, 10..15 the arguments going out,
 * and a call's answer in 10 - so this instruction set is that seam written
 * down.  A back end that had to invent a different machine would need a second
 * set of decisions about the same things.
 *
 * REGISTERS HOLD REAL ADDRESSES.  A program compiled here calls the ordinary
 * ABI with ordinary pointers: `&local` is an address in the interpreter's own
 * frame store, a string is an address in the image's data part, and a handle
 * from ag_open is whatever the kernel said.  There is no sandbox, and there is
 * no MMU underneath to build one with - see [T-12] in docs/06-ideas.md.  What
 * this rung buys is portability, not safety.
 *
 * AND ONE THING IT CANNOT DO.  A code address here is an offset into bytecode
 * with the top bit set, which is not something the kernel can jump to - so a
 * bytecode program cannot be a `.SYS` driver and cannot register cluster entry
 * points.  Both are callbacks FROM native code.  That is a real limit and the
 * reason the plan gives this rung to coordination rather than to everything.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef AG_BC_H
#define AG_BC_H

#include <stdint.h>

/*
 * `arch` in the .AXE header.  It sits beside AG_ARCH_XTENSA and AG_ARCH_RISCV32
 * on purpose: a bytecode program is an image like any other - same header, same
 * name and version, same way of saying what it needs - and everything that
 * reads a header keeps working.  What it is NOT is executable by a processor,
 * so ag_axe_native_arch() never answers this and a loader that does not know
 * about the interpreter refuses it in the ordinary words.
 */
#define AG_ARCH_BC 3

/* A code address is an offset into the bytecode with this bit set.  Every real
 * address on every part this system runs on is below it (0x3C.. data, 0x4x..
 * instruction), so one test tells a call to a function in this image from a
 * call through the ABI table. */
#define AG_BC_CODE_TAG 0x80000000u

/* Sixteen registers, the seam's own numbering.  0..6 are unused by the
 * compiler and are here so that a register fits in four bits. */
#define AG_BC_REGS 16

/*
 * The instructions.  One opcode byte, then operands, little-endian, unaligned -
 * this is decoded by a switch and never by a processor, so packing beats
 * alignment.
 *
 * `rr` in a comment means one byte holding two registers: the destination in
 * the high nibble, the source in the low one.
 */
enum ag_bc_op {
    AG_BC_NOP = 0x00,

    /* rd <- rs                                        op, rr            */
    AG_BC_MOV = 0x01,
    /* rd <- imm32                                     op, r, imm32      */
    AG_BC_LI = 0x02,
    /* rd <- data_base + off32                         op, r, off32      */
    AG_BC_LIDATA = 0x03,
    /* rd <- AG_BC_CODE_TAG | off32                    op, r, off32      */
    AG_BC_LICODE = 0x04,

    /* rd <- rs1 OP rs2                                op, rr, r         */
    AG_BC_ADD = 0x10,
    AG_BC_SUB = 0x11,
    AG_BC_MUL = 0x12,
    AG_BC_DIVS = 0x13,
    AG_BC_REMS = 0x14,
    AG_BC_DIVU = 0x15,
    AG_BC_REMU = 0x16,
    AG_BC_AND = 0x17,
    AG_BC_OR = 0x18,
    AG_BC_XOR = 0x19,
    AG_BC_SHL = 0x1a,
    AG_BC_SHRS = 0x1b,
    AG_BC_SHRU = 0x1c,
    /* rd <- (rs1 < rs2), 0 or 1                       op, rr, r         */
    AG_BC_SLT = 0x1d,
    AG_BC_SLTU = 0x1e,

    /* rd <- rs SHIFT sa                               op, rr, sa        */
    AG_BC_SHLI = 0x20,
    AG_BC_SHRSI = 0x21,
    AG_BC_SHRUI = 0x22,

    /* rd <- [rs + off16]                              op, rr, off16     */
    AG_BC_LW = 0x30,
    AG_BC_LBU = 0x31,
    /* [rs2 + off16] <- rs1                            op, rr, off16     */
    AG_BC_SW = 0x32,
    AG_BC_SB = 0x33,

    /*
     * Call what is in the register: a bytecode offset if it carries the tag,
     * otherwise a real function address from the ABI table.  Arguments are in
     * 10..15 either way and the answer comes back in 10, which is the one
     * convention the whole seam rests on.
     */
    AG_BC_CALL = 0x40,
    /* Open a frame of imm32 bytes and point register 7 at it.  op, imm32 */
    AG_BC_ENTER = 0x41,
    /* Close it and return to the caller, answer already in 10.  op       */
    AG_BC_RET = 0x42,

    /* pc <- target32                                  op, target32      */
    AG_BC_JMP = 0x50,
    /* if (rs1 CC rs2) pc <- target32                  op, rr, target32  */
    AG_BC_BEQ = 0x51,
    AG_BC_BNE = 0x52,
    AG_BC_BLT = 0x53,
    AG_BC_BGE = 0x54,
    AG_BC_BLTU = 0x55,
    AG_BC_BGEU = 0x56
};

#endif /* AG_BC_H */
