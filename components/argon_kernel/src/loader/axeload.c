/*
 * ArgonOS - placing a .AXE image in memory.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/axeload.h>

#include <string.h>

/* Sanity limits: an application asking for more than this is a broken file. */
#define AG_AXE_MAX_PART (8u * 1024u * 1024u)
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

/*
 * True when [base, base+size) covers `bytes` at addr.  Written entirely as
 * unsigned differences so that it is exact in a 32-bit address space however the
 * numbers sit relative to each other: an address below the base subtracts to a
 * huge value and falls out, with no need to compare addresses directly.
 */
static bool part_covers(const ag_axe_part_t *part, uint32_t addr, uint32_t bytes)
{
    if (part->size < bytes) {
        return false;
    }
    return (uint32_t)(addr - part->base) <= (part->size - bytes);
}

/* Everything one part has to satisfy on its own, before the parts are compared. */
static ag_err_t check_part(const ag_axe_part_t *part, size_t file_bytes)
{
    if (part->size == 0) {
        /* Absent, so it must claim nothing at all. */
        return (part->file_size == 0) ? AG_OK : -AG_EFORMAT;
    }
    if (part->size > AG_AXE_MAX_PART) {
        return -AG_EFORMAT;
    }
    if (part->file_size > part->size) {
        return -AG_EFORMAT;
    }
    /* Parts hold words, and the loader relocates words. */
    if ((part->base & 3u) != 0 || (part->size & 3u) != 0) {
        return -AG_EFORMAT;
    }
    /* A part running off the end of the 32-bit address space is nonsense. */
    if (part->base > UINT32_MAX - part->size) {
        return -AG_EFORMAT;
    }
    if ((uint64_t)part->offset + part->file_size > file_bytes) {
        return -AG_EFORMAT;
    }
    return AG_OK;
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

    /*
     * The strings come from a file and end up printed, logged and handed to
     * FreeRTOS as a task name.  A run of bytes with no terminator would be read
     * past the end of the header, so the last byte of each has to be one.
     */
    if (header->name[sizeof(header->name) - 1] != '\0' ||
        header->version[sizeof(header->version) - 1] != '\0' ||
        header->author[sizeof(header->author) - 1] != '\0') {
        return -AG_EFORMAT;
    }

    /* There is no such thing as an application without code. */
    if (header->code.size == 0) {
        return -AG_EFORMAT;
    }
    /* The code part is executed as it arrives; it has no bss to zero. */
    if (header->code.file_size != header->code.size) {
        return -AG_EFORMAT;
    }

    ag_err_t err = check_part(&header->code, file_bytes);
    if (err != AG_OK) {
        return err;
    }
    err = check_part(&header->data, file_bytes);
    if (err != AG_OK) {
        return err;
    }

    if (header->data.size > 0) {
        /*
         * Overlapping parts would make an address ambiguous: the loader decides
         * which bias a word needs by which part its target falls in, and two
         * answers is no answer.  The build tool links them far apart.
         */
        const uint32_t code_end = header->code.base + header->code.size;
        const uint32_t data_end = header->data.base + header->data.size;
        if (header->code.base < data_end && header->data.base < code_end) {
            return -AG_EFORMAT;
        }
        /*
         * A contiguous image is one whose code reaches its data by distance
         * rather than by address, so that distance is part of the contract.
         */
        if ((header->flags & AG_AXE_CONTIGUOUS) != 0 &&
            header->data.base != code_end) {
            return -AG_EFORMAT;
        }
    } else if ((header->flags & AG_AXE_CONTIGUOUS) != 0) {
        /* Nothing to keep adjacent. */
        return -AG_EFORMAT;
    }

    if (header->reloc_count > AG_AXE_MAX_RELOCS) {
        return -AG_EFORMAT;
    }
    if ((uint64_t)header->reloc_offset +
            (uint64_t)header->reloc_count * sizeof(uint32_t) > file_bytes) {
        return -AG_EFORMAT;
    }

    /* Execution starts in the code part; the API slot is a variable, so it can
     * be in either, and in practice it is in the data part's bss. */
    if (!part_covers(&header->code, header->entry, 1)) {
        return -AG_EFORMAT;
    }
    if (!part_covers(&header->code, header->api_slot, 4) &&
        !part_covers(&header->data, header->api_slot, 4)) {
        return -AG_EFORMAT;
    }

    return AG_OK;
}

void *ag_axe_resolve(const ag_axe_header_t *header, const ag_axe_place_t *place,
                     uint32_t linked_addr)
{
    if (header == NULL || place == NULL) {
        return NULL;
    }
    if (place->code != NULL && part_covers(&header->code, linked_addr, 1)) {
        return (uint8_t *)place->code + (linked_addr - header->code.base);
    }
    if (place->data != NULL && part_covers(&header->data, linked_addr, 1)) {
        return (uint8_t *)place->data + (linked_addr - header->data.base);
    }
    return NULL;
}

ag_err_t ag_axe_apply(const ag_axe_header_t *header,
                      const ag_axe_place_t *place, const uint32_t *relocs,
                      uint32_t reloc_count, ag_axe_binding_t *out)
{
    if (header == NULL || place == NULL || out == NULL) {
        return -AG_EINVAL;
    }
    if (place->code == NULL) {
        return -AG_EINVAL;
    }
    if (place->code_capacity < header->code.size) {
        return -AG_ENOMEM;
    }

    uint8_t *const code = (uint8_t *)place->code;
    uint8_t       *data = NULL;
    if (header->data.size > 0) {
        if (place->data == NULL) {
            return -AG_EINVAL;
        }
        if (place->data_capacity < header->data.size) {
            return -AG_ENOMEM;
        }
        data = (uint8_t *)place->data;
    }

    if (reloc_count != header->reloc_count) {
        return -AG_EINVAL;
    }
    if (reloc_count > 0 && relocs == NULL) {
        return -AG_EINVAL;
    }

    /* bss is whatever the file did not carry. */
    if (data != NULL && header->data.size > header->data.file_size) {
        memset(data + header->data.file_size, 0,
               header->data.size - header->data.file_size);
    }

    /*
     * The bias is the whole trick.  Every PC-relative instruction inside a part
     * is already correct, because it and its target moved together; only words
     * holding an absolute address need it added, and which of the two biases
     * depends on which part the address points into.
     */
    const uint32_t code_bias =
        (uint32_t)((uintptr_t)code - (uintptr_t)header->code.base);
    const uint32_t data_bias =
        (data != NULL)
            ? (uint32_t)((uintptr_t)data - (uintptr_t)header->data.base)
            : 0;

    for (uint32_t i = 0; i < reloc_count; i++) {
        const uint32_t entry = relocs[i];
        const uint32_t at = AG_AXE_R_OFFSET(entry);
        const bool     in_data = (entry & AG_AXE_R_IN_DATA) != 0;
        const bool     to_data = (entry & AG_AXE_R_TO_DATA) != 0;

        /* A relocation about a part the image does not have is a contradiction. */
        if ((in_data || to_data) && data == NULL) {
            return -AG_EFORMAT;
        }

        uint8_t *const part = in_data ? data : code;
        const uint32_t stored =
            in_data ? header->data.file_size : header->code.file_size;

        /*
         * A relocation outside the stored bytes means the file disagrees with
         * itself.  Refusing beats writing past the allocation.  Written as a
         * subtraction so a huge offset cannot wrap past the comparison.
         */
        if (stored < 4 || at > stored - 4) {
            return -AG_EFORMAT;
        }

        uint32_t word;
        memcpy(&word, part + at, sizeof(word));
        word += to_data ? data_bias : code_bias;
        memcpy(part + at, &word, sizeof(word));
    }

    out->code_base = (uintptr_t)code;
    out->data_base = (uintptr_t)data;
    out->entry = ag_axe_resolve(header, place, header->entry);
    out->api_slot = (uint32_t *)ag_axe_resolve(header, place, header->api_slot);
    if (out->entry == NULL || out->api_slot == NULL) {
        return -AG_EFORMAT;
    }
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
