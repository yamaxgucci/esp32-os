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
 *   +-------------------------+
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_AXE_H
#define ARGON_AXE_H

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

    uint32_t reserved[6]; /* future: signature, resources                 */
} ag_axe_header_t;

#ifdef __cplusplus
}
#endif

#endif /* ARGON_AXE_H */
