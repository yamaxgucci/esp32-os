/*
 * ArgonOS - placing a .AXE image in memory.
 *
 * The part of loading that has no dependency on the chip: validating the
 * header, zeroing bss, adding the load bias to the absolute words, and working
 * out where the entry point and the API pointer ended up.  Everything about
 * where the memory came from is somebody else's problem, which is what makes
 * this testable on a host against a reference build.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_AXELOAD_H
#define ARGON_AXELOAD_H

#include <argon/abi.h>
#include <argon/axe.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uintptr_t base;  /* where the image actually is                        */
    void     *entry; /* ag_main, as a function pointer                     */

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
 * Finishes an image already copied to `image`, which must be at least
 * header->image_size bytes.  The stored part is expected to be in place; bss is
 * zeroed here, so the caller need not clear the allocation first.
 */
ag_err_t ag_axe_apply(void *image, size_t image_capacity,
                      const ag_axe_header_t *header, const uint32_t *relocs,
                      uint32_t reloc_count, ag_axe_binding_t *out);

/* Which architecture this build of the kernel can run. */
ag_axe_arch_t ag_axe_native_arch(void);

/* "xtensa", "riscv32" - for messages a person reads. */
const char *ag_axe_arch_name(ag_axe_arch_t arch);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_AXELOAD_H */
