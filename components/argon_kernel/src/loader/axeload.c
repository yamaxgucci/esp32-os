/*
 * ArgonOS - placing a .AXE image in memory.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/axeload.h>

#include <string.h>

/* Sanity limits: an application asking for more than this is a broken file. */
#define AG_AXE_MAX_IMAGE (8u * 1024u * 1024u)
#define AG_AXE_MAX_RELOCS (64u * 1024u)

ag_axe_arch_t ag_axe_native_arch(void)
{
#if defined(__XTENSA__)
    return AG_ARCH_XTENSA;
#elif defined(__riscv)
    return AG_ARCH_RISCV32;
#else
    return AG_ARCH_NONE;
#endif
}

const char *ag_axe_arch_name(ag_axe_arch_t arch)
{
    switch (arch) {
    case AG_ARCH_XTENSA:  return "xtensa";
    case AG_ARCH_RISCV32: return "riscv32";
    default:              return "unknown";
    }
}

ag_err_t ag_axe_validate(const ag_axe_header_t *header, size_t file_bytes,
                         ag_axe_arch_t arch, uint16_t abi_major,
                         uint16_t abi_minor)
{
    if (header == NULL) {
        return -AG_EINVAL;
    }
    if (file_bytes < sizeof(ag_axe_header_t)) {
        return -AG_EFORMAT;
    }
    if (memcmp(header->magic, AG_AXE_MAGIC_STR, 4) != 0) {
        return -AG_EFORMAT;
    }
    if (header->header_size < sizeof(ag_axe_header_t)) {
        return -AG_EFORMAT;
    }

    /*
     * Architecture before ABI: an image built for the other instruction set is
     * a different kind of wrong, and saying which is more useful than saying
     * the version does not match.
     */
    if ((ag_axe_arch_t)header->arch != arch) {
        return -AG_ENOTSUP;
    }
    if (header->abi_major != abi_major || header->abi_minor > abi_minor) {
        return -AG_EABI;
    }

    if (header->image_size == 0 || header->image_size > AG_AXE_MAX_IMAGE) {
        return -AG_EFORMAT;
    }
    if (header->file_size > header->image_size) {
        return -AG_EFORMAT;
    }
    if (header->reloc_count > AG_AXE_MAX_RELOCS) {
        return -AG_EFORMAT;
    }

    /* Every offset the header names has to be inside the file. */
    if ((uint64_t)header->image_offset + header->file_size > file_bytes) {
        return -AG_EFORMAT;
    }
    if ((uint64_t)header->reloc_offset +
            (uint64_t)header->reloc_count * sizeof(uint32_t) > file_bytes) {
        return -AG_EFORMAT;
    }

    /* And the addresses it names have to be inside the image. */
    const uint32_t base = header->link_base;
    const uint32_t end = base + header->image_size;
    if (header->entry < base || header->entry >= end) {
        return -AG_EFORMAT;
    }
    if (header->api_slot < base || header->api_slot + 4 > end) {
        return -AG_EFORMAT;
    }

    return AG_OK;
}

ag_err_t ag_axe_apply(void *image, size_t image_capacity,
                      const ag_axe_header_t *header, const uint32_t *relocs,
                      uint32_t reloc_count, ag_axe_binding_t *out)
{
    if (image == NULL || header == NULL || out == NULL) {
        return -AG_EINVAL;
    }
    if (image_capacity < header->image_size) {
        return -AG_ENOMEM;
    }
    if (reloc_count != header->reloc_count) {
        return -AG_EINVAL;
    }
    if (reloc_count > 0 && relocs == NULL) {
        return -AG_EINVAL;
    }

    uint8_t *bytes = (uint8_t *)image;

    /* bss is whatever the file did not carry. */
    if (header->image_size > header->file_size) {
        memset(bytes + header->file_size, 0,
               header->image_size - header->file_size);
    }

    /*
     * The bias is the whole trick.  Every PC-relative instruction in the image
     * is already correct, because it and its target moved together; only words
     * holding an absolute address need it added.
     */
    const uintptr_t base = (uintptr_t)image;
    const uint32_t  bias = (uint32_t)(base - (uintptr_t)header->link_base);

    for (uint32_t i = 0; i < reloc_count; i++) {
        const uint32_t at = relocs[i];

        /*
         * A relocation outside the stored part means the file disagrees with
         * itself.  Refusing beats writing past the allocation.
         */
        if (at + 4 > header->file_size || (at & 3u) != 0) {
            return -AG_EFORMAT;
        }

        uint32_t word;
        memcpy(&word, bytes + at, sizeof(word));
        word += bias;
        memcpy(bytes + at, &word, sizeof(word));
    }

    out->base = base;
    out->entry = bytes + (header->entry - header->link_base);
    out->api_slot = (uint32_t *)(void *)(bytes +
                                         (header->api_slot - header->link_base));
    out->relocated = reloc_count;
    return AG_OK;
}

void ag_axe_bind_api(const ag_axe_binding_t *binding, const void *api)
{
    if (binding == NULL || binding->api_slot == NULL) {
        return;
    }

    /*
     * Truncating to 32 bits is exact on the target, which is a 32-bit machine.
     * On a host inspecting an image it would not be, and nothing there runs the
     * application, so there is nothing to get wrong.
     */
    const uint32_t value = (uint32_t)(uintptr_t)api;
    memcpy(binding->api_slot, &value, sizeof(value));
}
