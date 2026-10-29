/*
 * ArgonOS - placing a .AXE image in memory.
 *
 * The part of loading that has no dependency on the chip: validating the
 * header, zeroing bss, adding each part's load bias to the absolute words, and
 * working out where the entry point and the API pointer ended up.  Everything
 * about where the memory came from is somebody else's problem, which is what
 * makes this testable on a host against a reference build.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARGON_AXELOAD_H
#define ARGON_AXELOAD_H

#include <argon/abi.h>
#include <argon/axe.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where the caller put the two parts.  The stored bytes of each are expected to
 * be in place already; the capacities are here so that an allocation too small
 * for what the header asks is caught before anything is written to it.
 *
 * `data` may be NULL when the image has no data part.  For a contiguous image
 * it points into the same allocation as `code`, immediately after it.
 */
typedef struct {
    void  *code; /* final address: biases, entry, execution          */
    size_t code_capacity;
    void  *data;
    size_t data_capacity;
    /*
     * When non-NULL, relocation patches and api_slot resolution for the code
     * part write through this buffer instead of `code`.  Used for flash XIP:
     * biases target the eventual mmap address while bytes are still in RAM.
     */
    void *code_writable;

    /*
     * The cold part, when the image has one: the code that runs once, which
     * the caller has put somewhere slower and larger - flash - so that the
     * loop in `code` can have the arena.
     *
     * NULL, with a zero capacity, for every image built without --split-hot,
     * which is most of them.  `cold_writable` plays the same role for it that
     * `code_writable` does above: for flash the bytes are relocated in RAM
     * while the biases already point at where the mapping will be.
     */
    void  *cold;
    size_t cold_capacity;
    void  *cold_writable;
} ag_axe_place_t;

typedef struct {
    uintptr_t code_base; /* where the code part actually is                */
    uintptr_t data_base; /* where the data part actually is, 0 if absent    */
    uintptr_t cold_base; /* where the cold part is, 0 if the image has none */
    void     *entry;     /* ag_main, as a function pointer                  */

    /*
     * The application's pointer to the syscall table, as a 32-bit word rather
     * than a void** - an image is a 32-bit address space whatever the machine
     * inspecting it happens to be, and treating the slot as a host pointer
     * reads eight bytes where the image has four.
     */
    uint32_t *api_slot;

    uint32_t relocated; /* words adjusted, for the record                  */
} ag_axe_binding_t;

/*
 * Writes the syscall table address into the image's slot.  Separate from
 * ag_axe_apply because binding an API into an image is the kernel's business,
 * not the format's.
 */
void ag_axe_bind_api(const ag_axe_binding_t *binding, const void *api);

/*
 * Checks the header against what this kernel can run.  `file_bytes` is the size
 * of the whole file, so that the offsets inside can be shown to be inside it -
 * a malformed image must be rejected, not trusted and then read past.
 */
ag_err_t ag_axe_validate(const ag_axe_header_t *header, size_t file_bytes,
                         ag_axe_arch_t arch, uint16_t abi_major,
                         uint16_t abi_minor);

/*
 * Finishes an image whose parts are already copied into `place`.  bss is zeroed
 * here, so the caller need not clear the allocations first.
 */
ag_err_t ag_axe_apply(const ag_axe_header_t *header,
                      const ag_axe_place_t *place, const uint32_t *relocs,
                      uint32_t reloc_count, const ag_axe_ireloc_t *irelocs,
                      uint32_t ireloc_count, ag_axe_binding_t *out);

/*
 * The same, for an image with a cold part.
 *
 * A separate entry point rather than two more arguments on the one above, so
 * that a caller written before the cold part existed cannot be handed a split
 * image and quietly leave half its relocations unapplied: ag_axe_apply refuses
 * a split header outright.  An unsplit image may be passed to either.
 */
ag_err_t ag_axe_apply_split(const ag_axe_header_t *header,
                            const ag_axe_place_t *place,
                            const uint32_t *relocs, uint32_t reloc_count,
                            const ag_axe_ireloc_t *irelocs,
                            uint32_t ireloc_count,
                            const ag_axe_creloc_t *crelocs,
                            uint32_t creloc_count, ag_axe_binding_t *out);

/*
 * The instruction relocation table, as this image really has it.
 *
 * Two words were added to the header after `reserved`, so an image built
 * before them has neither, and asking these two rather than reading the fields
 * is what keeps such an image loading unchanged: a short header read into a
 * full struct leaves those fields holding whatever was in the struct.
 */
uint32_t ag_axe_ireloc_count(const ag_axe_header_t *header);
uint32_t ag_axe_ireloc_offset(const ag_axe_header_t *header);

/*
 * The split relocation table, for the same reason and read the same way: an
 * image built before the cold part existed has a header too short to hold
 * these fields, and asking rather than reading is what keeps it loading
 * unchanged.  Both are zero for an unsplit image, whatever its header length.
 */
uint32_t ag_axe_creloc_count(const ag_axe_header_t *header);
uint32_t ag_axe_creloc_offset(const ag_axe_header_t *header);

/* True when the image has a cold part at all. */
bool ag_axe_is_split(const ag_axe_header_t *header);

/*
 * One instruction relocation, applied to a window of the code part.
 *
 * `code_bytes` points at the code part's byte `code_off`, and `code_addr` /
 * `data_addr` are where the parts will finally sit.  Entries outside the
 * window are ignored, which is what lets the streamed XIP path hand over one
 * page at a time; entries that point outside the image return false and the
 * image is refused.
 */
bool ag_axe_ireloc_apply(const ag_axe_header_t *header, uint8_t *code_bytes,
                         uint32_t code_stored, uint32_t code_addr,
                         uint32_t data_addr, uint32_t code_off,
                         ag_axe_ireloc_t entry);

/*
 * Turns an address as linked into the address it ended up at, by finding which
 * part it falls in.  NULL when it belongs to neither, which means the header
 * and the image disagree.
 */
void *ag_axe_resolve(const ag_axe_header_t *header, const ag_axe_place_t *place,
                     uint32_t linked_addr);

/* Which architecture this build of the kernel can run. */
ag_axe_arch_t ag_axe_native_arch(void);

/* "xtensa", "riscv32" - for messages a person reads. */
const char *ag_axe_arch_name(ag_axe_arch_t arch);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_AXELOAD_H */
