/*
 * ArgonOS - the .AXE application image format.
 *
 * An application is linked on the host at nominal addresses and loaded on the
 * device wherever there is room.  Moving a part moves every PC-relative
 * reference inside it - the instruction and its target shift together - so the
 * only thing the loader has to fix is the absolute 32-bit words, which are the
 * literal pool entries and initialised pointers holding addresses.
 *
 * Measured, not assumed: linking the same object at two bases 0x10000 apart
 * produced images differing in exactly six bytes, one in each of the six
 * literal words that hold an address.  Every instruction was identical.
 *
 * That is why there is no ELF parser and no instruction decoder in the kernel.
 * The build tool does the hard part on a machine where complexity is cheap, and
 * leaves the loader with a copy, a pass over a list of offsets, and a jump.
 *
 * An image has two parts, because the memory they need is not the same memory.
 * Code has to be executable; data and bss have to be writable, and on this
 * family most memory is one or the other.  Keeping them apart lets the code sit
 * in the small executable arena while the data - usually the part that grows -
 * lives in extended memory, megabytes of it.
 *
 * Constants belong to the data part, not to the code: a font or a bitmap is read
 * and never executed, so there is no reason for it to take up the arena.  The
 * exception is .hot_rodata, which an application asks for by name when the read
 * latency matters more than the space.
 *
 * The two parts are relocated by two independent biases, so a word holding an
 * address must say which part that address is in.  The offsets in the table are
 * word-aligned, which leaves the low two bits free to say exactly that.
 *
 * On RISC-V there is no literal pool: an address is built by a pair of
 * instructions carrying it in their immediate fields, so relocating one means
 * re-encoding the instruction.  That is the second table below
 * (ag_axe_ireloc_t), and it is what allows the parts to be far apart - which is
 * what executing the code from flash while the data lives in RAM requires.
 *
 * Layout of a .AXE file:
 *
 *   +-------------------------+ 0
 *   | ag_axe_header_t         |
 *   +-------------------------+ code.offset
 *   | code part               |
 *   |  (literals, text,       | code.file_size bytes
 *   |   hot_rodata, header)   |
 *   +-------------------------+ data.offset
 *   | data part               |
 *   |  (rodata and data -     | data.file_size bytes
 *   |   not bss)              |
 *   +-------------------------+ reloc_offset
 *   | uint32 relocations      | reloc_count entries
 *   +-------------------------+ ireloc_offset
 *   | ag_axe_ireloc_t pairs   | ireloc_count entries (RISC-V only)
 *   +-------------------------+
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_AXE_H
#define ARGON_AXE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AG_AXE_MAGIC_STR "AXE1"

typedef enum {
    AG_ARCH_NONE = 0,
    AG_ARCH_XTENSA = 1,  /* ESP32, S2, S3          */
    AG_ARCH_RISCV32 = 2, /* C3, C6, H2, P4         */
} ag_axe_arch_t;

/*
 * One part of an image: where it was linked, how much memory it needs, and how
 * much of that the file carries.  Anything the file does not carry is bss and
 * is zeroed by the loader, so `size` may exceed `file_size` - for the data part
 * it usually does, and for the code part it never does.
 *
 * A part with size 0 is absent, which is normal for the data part of an
 * application that has no writable statics at all.
 */
typedef struct {
    uint32_t base;      /* address this part was linked at                 */
    uint32_t size;      /* bytes to allocate, bss included                 */
    uint32_t file_size; /* bytes of it stored in the file                  */
    uint32_t offset;    /* where those bytes start in the file             */
} ag_axe_part_t;

/*
 * Relocation entries.  The offset is word-aligned, so the low two bits carry
 * which part the word lives in and which part the address it holds points into.
 * A v1 single-part image comes out as all-zero bits, which reads correctly as
 * "in the code part, pointing at the code part".
 */
#define AG_AXE_R_OFFSET(e) ((e) & ~3u)
#define AG_AXE_R_IN_DATA 0x1u /* the word itself is stored in the data part */
#define AG_AXE_R_TO_DATA 0x2u /* the address it holds is in the data part   */

/*
 * Relocations that live inside an instruction, not in a word of its own.
 *
 * Xtensa needs none of these: an address it cannot reach with a PC-relative
 * instruction is fetched with l32r from a literal pool, so the address is a
 * plain 32-bit word and the table above describes it.  RISC-V has no literal
 * pool - an address is built by a pair of instructions that carry it in their
 * immediate fields (`lui` with the top twenty bits, then `addi`/`lw`/`sw`
 * with the low twelve) - so relocating one means decoding and re-encoding the
 * instruction.
 *
 * That is what lets a RISC-V image be split into two parts that move
 * independently, which is what flash XIP requires: code in flash, data in RAM,
 * nowhere near each other.  Without it the parts have to stay adjacent (see
 * AG_AXE_CONTIGUOUS) because the code reaches its data by PC-relative
 * arithmetic, and the code then cannot be executed from flash at all.
 *
 * Each entry is a pair of words: where the instruction is, and what address it
 * should end up holding - as an offset into whichever part that address points
 * into.  The offset rather than the address, because the loader knows where it
 * put the parts and the file cannot.  Storing the target rather than a bias to
 * add makes applying an entry idempotent, which the streamed XIP path needs:
 * an instruction that straddles a page boundary is patched from both sides.
 */
typedef struct {
    uint32_t site;   /* offset of the instruction, plus the bits below      */
    uint32_t target; /* offset within the part the address points into      */
} ag_axe_ireloc_t;

#define AG_AXE_I_OFFSET(e) ((e) & 0x0FFFFFFFu)
#define AG_AXE_I_TO_DATA 0x10000000u /* the address is in the data part     */
#define AG_AXE_I_KIND(e) ((e) & 0xE0000000u)
#define AG_AXE_I_HI20 0x20000000u   /* lui: bits 31..12 hold imm[31:12]     */
#define AG_AXE_I_LO12_I 0x40000000u /* addi/lw: bits 31..20 hold imm[11:0]  */
#define AG_AXE_I_LO12_S 0x60000000u /* sw: imm split across 31..25 and 11..7 */

/*
 * An image built for one architecture will not run on the other.  The shell
 * says so by name rather than crashing, which is the whole reason this field
 * exists.
 */
typedef struct {
    char     magic[4]; /* "AXE1"                                          */
    uint16_t abi_major;
    uint16_t abi_minor;
    uint16_t arch;        /* ag_axe_arch_t                                */
    uint16_t header_size; /* so a newer tool can add fields               */
    uint32_t flags;       /* ag_axe_flags                                 */

    ag_axe_part_t code; /* executable: literals, text, hot constants      */
    ag_axe_part_t data; /* writable: constants, data and bss             */

    /*
     * Addresses as linked.  The loader subtracts the base of whichever part
     * they fall in and adds where that part really went; nothing has to be
     * looked up by name, so the kernel needs no symbol table.
     */
    uint32_t entry;    /* ag_main, in the code part                       */
    uint32_t api_slot; /* the application's pointer to the syscall table  */

    uint32_t reloc_offset; /* file offset of the relocation table         */
    uint32_t reloc_count;

    uint32_t stack_size; /* 0 means the kernel default                    */
    uint32_t heap_size;  /* 0 means the kernel default                    */

    char name[32];
    char version[16];
    char author[32];

    /*
     * Signature / future resources.  Layout when used for HMAC:
     *   reserved[0] = algo (0 = none; 1 = HMAC-SHA256 truncated to 16 bytes)
     *   reserved[1] = key_id (0 = built-in development key)
     *   reserved[2..5] = 16-byte tag
     * All zeros means unsigned (accepted).  See argon/axesig.h.
     */
    uint32_t reserved[6];

    /*
     * Instruction relocations (see ag_axe_ireloc_t).  Added after `reserved`,
     * which is why `header_size` exists: an older loader stops before these
     * fields and reads an image that has none of them exactly as it always
     * did, and a newer loader reading an older image sees a header too short
     * to contain them and treats the count as zero.
     */
    uint32_t ireloc_offset;
    uint32_t ireloc_count;
} ag_axe_header_t;

/*
 * The part of the header every image has and every loader needs.
 *
 * A loader must check header_size against THIS, never against
 * sizeof(ag_axe_header_t): the struct grows, images already built do not, and
 * the whole purpose of header_size is that an older image keeps loading.
 * Anything past this point is optional and must be asked for through an
 * accessor that checks header_size for itself.
 */
#define AG_AXE_HEADER_MIN offsetof(ag_axe_header_t, ireloc_offset)

#ifdef __cplusplus
}
#endif

#endif /* ARGON_AXE_H */
