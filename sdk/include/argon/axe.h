/*
 * ArgonOS - the .AXE application image format.
 *
 * An application is linked on the host into one contiguous image at a nominal
 * base address, and loaded on the device wherever there is room.  Moving the
 * image moves every PC-relative reference in it - the instruction and its
 * target shift together - so the only thing the loader has to fix is the
 * absolute 32-bit words, which are the literal pool entries holding addresses.
 *
 * Measured, not assumed: linking the same object at two bases 0x10000 apart
 * produced images differing in exactly six bytes, one in each of the six
 * literal words that hold an address.  Every instruction was identical.
 *
 * That is why there is no ELF parser and no instruction decoder in the kernel.
 * The build tool does the hard part on a machine where complexity is cheap, and
 * leaves the loader with a copy, a pass over a list of offsets, and a jump.
 *
 * Layout of a .AXE file:
 *
 *   +-------------------------+ 0
 *   | ag_axe_header_t         |
 *   +-------------------------+ image_offset
 *   | image (text, rodata,    |
 *   |  data - not bss)        | file_size bytes
 *   +-------------------------+ reloc_offset
 *   | uint32 image offsets    | reloc_count entries
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

    uint32_t link_base;    /* address the image was linked at             */
    uint32_t image_size;   /* bytes to allocate, bss included             */
    uint32_t file_size;    /* bytes of image stored in the file           */
    uint32_t image_offset; /* where those bytes start in the file         */

    /*
     * Addresses as linked.  The loader subtracts link_base and adds the real
     * load address; nothing has to be looked up by name, so the kernel needs
     * no symbol table.
     */
    uint32_t entry;    /* ag_main                                         */
    uint32_t api_slot; /* the application's pointer to the syscall table  */

    uint32_t reloc_offset; /* file offset of the table of image offsets   */
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
