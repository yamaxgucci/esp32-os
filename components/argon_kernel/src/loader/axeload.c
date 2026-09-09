/*
 * ArgonOS - placing a .AXE image in memory.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <argon/axeload.h>

#include <stddef.h>
#include <string.h>

/* Sanity limits: an application asking for more than this is a broken file. */
#define AG_AXE_MAX_PART (8u * 1024u * 1024u)
#define AG_AXE_MAX_RELOCS (64u * 1024u)

/*
 * Instruction relocations are two words each and there are more of them - a
 * RISC-V image names one for every reference to a global, where xtensa names
 * one per literal - so the same ceiling in entries is twice the bytes.  The
 * desktop shell, the largest in the tree, has sixteen hundred.
 */
#define AG_AXE_MAX_IRELOCS (64u * 1024u)

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
    if (file_bytes < AG_AXE_HEADER_MIN) {
        return -AG_EFORMAT;
    }
    if (memcmp(header->magic, AG_AXE_MAGIC_STR, 4) != 0) {
        return -AG_EFORMAT;
    }
    /*
     * Big enough to hold what a loader must have, not as big as this loader
     * happens to be.
     *
     * The difference is the whole point of header_size, and getting it wrong is
     * not subtle: comparing against sizeof(ag_axe_header_t) means the day a
     * field is added, every image ever built becomes -AG_EFORMAT - which is
     * what happened, and what it looked like was a board whose panel driver
     * stopped loading with no other change than a new kernel.  Fields past the
     * minimum are optional and are asked for through accessors that check for
     * themselves.
     */
    if (header->header_size < AG_AXE_HEADER_MIN) {
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

    /*
     * The instruction relocations, if this image is new enough to have any.
     *
     * A header that stops before the fields cannot have them, and an image
     * built by an older tool must keep loading exactly as it did - which is
     * what header_size is for.  Read through the header rather than off the
     * struct so a short header is never read past its end.
     */
    if (ag_axe_ireloc_count(header) > AG_AXE_MAX_IRELOCS) {
        return -AG_EFORMAT;
    }
    if ((uint64_t)ag_axe_ireloc_offset(header) +
            (uint64_t)ag_axe_ireloc_count(header) * sizeof(ag_axe_ireloc_t) >
        file_bytes) {
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

/*
 * How much of the header this image actually has.
 *
 * Two words were added after `reserved`, so an image built before them has a
 * shorter header and no table.  Asked through these two rather than by reading
 * the fields directly, because reading a field an image does not have is
 * reading whatever follows it in memory - which for a header read into a
 * struct is the struct's own tail, and would be believed.
 */
uint32_t ag_axe_ireloc_count(const ag_axe_header_t *header)
{
    if (header == NULL ||
        header->header_size < offsetof(ag_axe_header_t, ireloc_count) +
                                  sizeof(uint32_t)) {
        return 0;
    }
    return header->ireloc_count;
}

uint32_t ag_axe_ireloc_offset(const ag_axe_header_t *header)
{
    if (ag_axe_ireloc_count(header) == 0) {
        return 0;
    }
    return header->ireloc_offset;
}

/*
 * Put an address into an instruction, leaving the rest of it alone.
 *
 * RISC-V builds an address out of two instructions: `lui` carries the top
 * twenty bits and an `addi`/`lw`/`sw` the low twelve, sign-extended.  The
 * rounding in the high half is what makes the pair agree: the low half is
 * signed, so for half of all addresses it subtracts, and the high half has to
 * have been rounded up by one to match.  Getting that wrong is wrong by
 * exactly 4096, on exactly those addresses whose low twelve bits are 0x800 or
 * more - which is a bug that works most of the time.
 *
 * Written from the target address alone, so applying it twice is the same as
 * applying it once.  The streamed XIP path relies on that.
 *
 * Byte-wise, because an instruction is only two-byte aligned once compressed
 * instructions are in play.  That is safe here and would not be on xtensa,
 * whose instruction memory refuses unaligned access - but an xtensa image has
 * no entries of this kind at all, because there an address lives in a literal
 * pool as a plain word.
 */
static void ireloc_patch(uint8_t *at, uint32_t kind, uint32_t addr)
{
    uint32_t word;
    memcpy(&word, at, sizeof(word));

    switch (kind) {
    case AG_AXE_I_HI20:
        word = ((addr + 0x800u) & 0xFFFFF000u) | (word & 0xFFFu);
        break;
    case AG_AXE_I_LO12_I:
        word = ((addr & 0xFFFu) << 20) | (word & 0x000FFFFFu);
        break;
    case AG_AXE_I_LO12_S:
        word = ((((addr >> 5) & 0x7Fu) << 25) | (word & 0x01FFF07Fu) |
                ((addr & 0x1Fu) << 7));
        break;
    default:
        return; /* a kind this loader does not know; validation refused it */
    }
    memcpy(at, &word, sizeof(word));
}

/*
 * One entry, checked against the image it claims to describe.
 *
 * Returns false when the entry points outside the part it says it is in, which
 * is a file disagreeing with itself; the caller refuses the image rather than
 * writing past an allocation.
 */
bool ag_axe_ireloc_apply(const ag_axe_header_t *header, uint8_t *code_bytes,
                         uint32_t code_stored, uint32_t code_addr,
                         uint32_t data_addr, uint32_t code_off,
                         ag_axe_ireloc_t entry)
{
    const uint32_t at = AG_AXE_I_OFFSET(entry.site);
    const uint32_t kind = AG_AXE_I_KIND(entry.site);
    const bool     to_data = (entry.site & AG_AXE_I_TO_DATA) != 0;

    if (code_stored < 4u || at > code_stored - 4u) {
        return false;
    }
    if (to_data) {
        if (entry.target > header->data.size) {
            return false;
        }
    } else if (entry.target > header->code.size) {
        return false;
    }
    if (at < code_off) {
        return true; /* not in the window the caller is holding */
    }

    const uint32_t base = to_data ? data_addr : code_addr;
    ireloc_patch(code_bytes + (at - code_off), kind, base + entry.target);
    return true;
}

ag_err_t ag_axe_apply(const ag_axe_header_t *header,
                      const ag_axe_place_t *place, const uint32_t *relocs,
                      uint32_t reloc_count, const ag_axe_ireloc_t *irelocs,
                      uint32_t ireloc_count, ag_axe_binding_t *out)
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

    uint8_t *const code_final = (uint8_t *)place->code;
    uint8_t *const code_write =
        (place->code_writable != NULL) ? (uint8_t *)place->code_writable
                                       : code_final;
    uint8_t *data = NULL;
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
    if (ireloc_count != ag_axe_ireloc_count(header)) {
        return -AG_EINVAL;
    }
    if (ireloc_count > 0 && irelocs == NULL) {
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
     *
     * Biases use the final code address (`code`), even when patches are written
     * into `code_writable` for a later flash program (R-1 XIP).
     */
    const uint32_t code_bias =
        (uint32_t)((uintptr_t)code_final - (uintptr_t)header->code.base);
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

        uint8_t *const part = in_data ? data : code_write;
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

        /*
         * A word, addressed as a word.  AG_AXE_R_OFFSET masks off the two flag
         * bits, so `at` is a multiple of four, and both parts start 16-aligned;
         * the cast is therefore sound - and it has to be made, because when the
         * code part is the arena it is instruction memory.  On the original
         * ESP32 that memory answers only to aligned 32-bit accesses, and a
         * four-byte memcpy through a uint8_t * is compiled as four byte
         * accesses on a machine with no unaligned loads.  It would fault here,
         * inside relocation, on the first application the board ever loaded.
         */
        uint32_t *const slot = (uint32_t *)(void *)(part + at);
        uint32_t        word;

        memcpy(&word, slot, sizeof(word));
        word += to_data ? data_bias : code_bias;
        memcpy(slot, &word, sizeof(word));
    }

    /*
     * And the addresses that live inside instructions.  Same biases, different
     * arithmetic: there is no word to add to, so each one is re-encoded from
     * the address it should end up holding.  Patched into the writable view for
     * the same reason the words are, while the address written is the final
     * one.
     */
    for (uint32_t i = 0; i < ireloc_count; i++) {
        if ((irelocs[i].site & AG_AXE_I_TO_DATA) != 0 && data == NULL) {
            return -AG_EFORMAT;
        }
        if (!ag_axe_ireloc_apply(header, code_write, header->code.file_size,
                                 (uint32_t)(uintptr_t)code_final,
                                 (uint32_t)(uintptr_t)data, 0u, irelocs[i])) {
            return -AG_EFORMAT;
        }
    }

    out->code_base = (uintptr_t)code_final;
    out->data_base = (uintptr_t)data;
    out->entry = (void *)(code_final + (header->entry - header->code.base));
    if (part_covers(&header->code, header->api_slot, 4)) {
        out->api_slot =
            (uint32_t *)(code_write + (header->api_slot - header->code.base));
    } else {
        out->api_slot =
            (uint32_t *)ag_axe_resolve(header, place, header->api_slot);
    }
    if (out->entry == NULL || out->api_slot == NULL) {
        return -AG_EFORMAT;
    }
    out->relocated = reloc_count + ireloc_count;
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
