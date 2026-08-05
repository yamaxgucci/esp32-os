/*
 * ArgonOS - application image loading tests.
 *
 * The important test here is not that the loader runs without crashing, it is
 * that its output is byte-for-byte what the linker would have produced had it
 * targeted that address in the first place.  Two fixtures are built from the
 * same source at different bases; loading one at the other's base has to
 * reproduce the other exactly.  If it does, the relocation is right, and if it
 * does not, the difference says where.
 *
 * Regenerate the fixtures with host-tests/fixtures/regenerate.ps1.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/axeload.h>

#include <stdlib.h>

#include "test.h"

#define FIXTURE_NOMINAL "sample-nominal.axe"
#define FIXTURE_RELINKED "sample-relinked.axe"

typedef struct {
    uint8_t *data;
    size_t   size;
} blob_t;

/*
 * The fixture directory is compiled in rather than guessed from the working
 * directory, which differs between running the executable and running it
 * through ctest.
 */
#ifndef AG_FIXTURE_DIR
#define AG_FIXTURE_DIR "."
#endif

static blob_t load_file(const char *path)
{
    blob_t out = {NULL, 0};

    char full[512];
    snprintf(full, sizeof(full), "%s/%s", AG_FIXTURE_DIR, path);

    FILE *f = fopen(full, "rb");
    if (f == NULL) {
        printf("     (cannot open %s)\n", full);
        return out;
    }

    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0) {
        out.data = (uint8_t *)malloc((size_t)size);
        if (out.data != NULL && fread(out.data, 1, (size_t)size, f) ==
                                    (size_t)size) {
            out.size = (size_t)size;
        }
    }
    fclose(f);
    return out;
}

static const ag_axe_header_t *header_of(const blob_t *b)
{
    return (const ag_axe_header_t *)b->data;
}

static const uint32_t *relocs_of(const blob_t *b)
{
    return (const uint32_t *)(b->data + header_of(b)->reloc_offset);
}

static const uint8_t *stored_of(const blob_t *b)
{
    return b->data + header_of(b)->image_offset;
}

/* ---------------------------------------------------------------------- */

static void test_fixtures_are_present(void)
{
    blob_t a = load_file(FIXTURE_NOMINAL);
    blob_t b = load_file(FIXTURE_RELINKED);

    AG_CHECK(a.size > 0);
    AG_CHECK(b.size > 0);
    if (a.size == 0 || b.size == 0) {
        printf("     (fixtures missing; run host-tests/fixtures/regenerate.ps1)\n");
        free(a.data);
        free(b.data);
        return;
    }

    /* Same source, same size, different link addresses. */
    AG_CHECK_INT(header_of(&a)->image_size, header_of(&b)->image_size);
    AG_CHECK_INT(header_of(&a)->file_size, header_of(&b)->file_size);
    AG_CHECK_INT(header_of(&a)->reloc_count, header_of(&b)->reloc_count);
    AG_CHECK(header_of(&a)->link_base != header_of(&b)->link_base);
    AG_CHECK(header_of(&a)->reloc_count > 0);

    free(a.data);
    free(b.data);
}

static void test_validate(void)
{
    blob_t a = load_file(FIXTURE_NOMINAL);
    if (a.size == 0) {
        return;
    }
    const ag_axe_header_t *h = header_of(&a);

    AG_CHECK_INT(ag_axe_validate(h, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 AG_OK);

    /* An image for the other instruction set is rejected as unsupported, not
     * as a version mismatch: which one it is matters to whoever reads it. */
    AG_CHECK_INT(ag_axe_validate(h, a.size, AG_ARCH_RISCV32, h->abi_major,
                                 h->abi_minor),
                 -AG_ENOTSUP);

    /* A newer minor version than we implement cannot be run. */
    AG_CHECK_INT(ag_axe_validate(h, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 (uint16_t)(h->abi_minor - 1)),
                 -AG_EABI);
    AG_CHECK_INT(ag_axe_validate(h, a.size, AG_ARCH_XTENSA,
                                 (uint16_t)(h->abi_major + 1), h->abi_minor),
                 -AG_EABI);

    /* Truncation is caught by the offsets no longer fitting. */
    AG_CHECK_INT(ag_axe_validate(h, sizeof(ag_axe_header_t), AG_ARCH_XTENSA,
                                 h->abi_major, h->abi_minor),
                 -AG_EFORMAT);
    AG_CHECK_INT(ag_axe_validate(h, 4, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);
    AG_CHECK_INT(ag_axe_validate(NULL, a.size, AG_ARCH_XTENSA, 0, 1),
                 -AG_EINVAL);

    /* A wrong magic is not an ArgonOS image at all. */
    ag_axe_header_t bad = *h;
    bad.magic[0] = 'X';
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    /* An entry point outside the image would jump into nothing. */
    bad = *h;
    bad.entry = bad.link_base + bad.image_size + 4;
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    bad = *h;
    bad.file_size = bad.image_size + 1;
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    free(a.data);
}

/*
 * The one that matters: relocating the nominal image to the other fixture's
 * base must reproduce that fixture byte for byte.
 */
static void test_relocation_matches_a_direct_link(void)
{
    blob_t a = load_file(FIXTURE_NOMINAL);
    blob_t b = load_file(FIXTURE_RELINKED);
    if (a.size == 0 || b.size == 0) {
        free(a.data);
        free(b.data);
        return;
    }

    const ag_axe_header_t *ha = header_of(&a);
    const ag_axe_header_t *hb = header_of(&b);

    /*
     * The loader relocates relative to where the image really is, so the test
     * pretends the buffer lives at the reference base by adjusting link_base by
     * the same amount.  What is being checked is the arithmetic on the stored
     * words, and that is unaffected by where the buffer actually is.
     */
    uint8_t *buf = (uint8_t *)malloc(ha->image_size);
    AG_CHECK(buf != NULL);
    if (buf == NULL) {
        free(a.data);
        free(b.data);
        return;
    }
    memcpy(buf, stored_of(&a), ha->file_size);

    ag_axe_header_t shifted = *ha;
    /* Make the bias come out as (reference base - nominal base). */
    shifted.link_base =
        (uint32_t)((uintptr_t)buf - (hb->link_base - ha->link_base));

    ag_axe_binding_t binding;
    AG_CHECK_INT(ag_axe_apply(buf, ha->image_size, &shifted, relocs_of(&a),
                              ha->reloc_count, &binding),
                 AG_OK);
    AG_CHECK_INT(binding.relocated, ha->reloc_count);

    /* Byte for byte identical to what the linker produced for that base. */
    const uint8_t *want = stored_of(&b);
    int            differences = 0;
    for (uint32_t i = 0; i < hb->file_size; i++) {
        if (buf[i] != want[i]) {
            if (differences < 4) {
                printf("     first difference at image offset 0x%x: "
                       "loaded %02x, linker %02x\n",
                       (unsigned)i, buf[i], want[i]);
            }
            differences++;
        }
    }
    AG_CHECK_INT(differences, 0);

    /* And bss really is zero, without the caller having cleared anything. */
    int nonzero = 0;
    for (uint32_t i = ha->file_size; i < ha->image_size; i++) {
        if (buf[i] != 0) {
            nonzero++;
        }
    }
    AG_CHECK_INT(nonzero, 0);

    free(buf);
    free(a.data);
    free(b.data);
}

static void test_entry_and_api_slot(void)
{
    blob_t a = load_file(FIXTURE_NOMINAL);
    if (a.size == 0) {
        return;
    }
    const ag_axe_header_t *h = header_of(&a);

    uint8_t *buf = (uint8_t *)malloc(h->image_size);
    AG_CHECK(buf != NULL);
    if (buf == NULL) {
        free(a.data);
        return;
    }
    memcpy(buf, stored_of(&a), h->file_size);

    ag_axe_binding_t binding;
    AG_CHECK_INT(ag_axe_apply(buf, h->image_size, h, relocs_of(&a),
                              h->reloc_count, &binding),
                 AG_OK);

    /* Both land inside the buffer, at the offsets the header described. */
    AG_CHECK_INT((uint8_t *)binding.entry - buf, h->entry - h->link_base);
    AG_CHECK_INT((uint8_t *)binding.api_slot - buf, h->api_slot - h->link_base);
    AG_CHECK((uint8_t *)binding.entry >= buf);
    AG_CHECK((uint8_t *)binding.entry < buf + h->image_size);

    /*
     * The slot starts empty and is where the kernel writes the API table.  It
     * is read as a 32-bit word, not as a host pointer: on a 64-bit machine that
     * would read four bytes past the end of the image.
     */
    uint32_t slot = 0xdeadbeef;
    memcpy(&slot, binding.api_slot, sizeof(slot));
    AG_CHECK_INT(slot, 0);

    /* And binding writes exactly the low 32 bits of the table address. */
    const void *fake_api = (const void *)(uintptr_t)0x3fc12340u;
    ag_axe_bind_api(&binding, fake_api);
    memcpy(&slot, binding.api_slot, sizeof(slot));
    AG_CHECK_INT(slot, 0x3fc12340u);

    free(buf);
    free(a.data);
}

static void test_apply_rejects_bad_input(void)
{
    blob_t a = load_file(FIXTURE_NOMINAL);
    if (a.size == 0) {
        return;
    }
    const ag_axe_header_t *h = header_of(&a);

    uint8_t         *buf = (uint8_t *)malloc(h->image_size);
    ag_axe_binding_t binding;
    AG_CHECK(buf != NULL);
    if (buf == NULL) {
        free(a.data);
        return;
    }

    AG_CHECK_INT(ag_axe_apply(NULL, h->image_size, h, relocs_of(&a),
                              h->reloc_count, &binding),
                 -AG_EINVAL);
    /* Too small an allocation is caught before anything is written. */
    AG_CHECK_INT(ag_axe_apply(buf, h->image_size - 1, h, relocs_of(&a),
                              h->reloc_count, &binding),
                 -AG_ENOMEM);
    /* A count that disagrees with the header means the file is inconsistent. */
    AG_CHECK_INT(ag_axe_apply(buf, h->image_size, h, relocs_of(&a),
                              h->reloc_count + 1, &binding),
                 -AG_EINVAL);

    /* A relocation pointing outside the stored bytes must not be applied. */
    uint32_t evil[8];
    const uint32_t count = (h->reloc_count < 8) ? h->reloc_count : 8;
    memcpy(evil, relocs_of(&a), count * sizeof(uint32_t));
    evil[0] = h->file_size;  /* one word past the end */
    AG_CHECK_INT(ag_axe_apply(buf, h->image_size, h, evil, count, &binding),
                 -AG_EFORMAT);

    /* And an unaligned one is not a word at all. */
    memcpy(evil, relocs_of(&a), count * sizeof(uint32_t));
    evil[0] = 1;
    AG_CHECK_INT(ag_axe_apply(buf, h->image_size, h, evil, count, &binding),
                 -AG_EFORMAT);

    free(buf);
    free(a.data);
}

static void test_arch_names(void)
{
    AG_CHECK_STR(ag_axe_arch_name(AG_ARCH_XTENSA), "xtensa");
    AG_CHECK_STR(ag_axe_arch_name(AG_ARCH_RISCV32), "riscv32");
    AG_CHECK_STR(ag_axe_arch_name(AG_ARCH_NONE), "unknown");
}

void run_axeload_tests(void)
{
    test_fixtures_are_present();
    test_validate();
    test_relocation_matches_a_direct_link();
    test_entry_and_api_slot();
    test_apply_rejects_bad_input();
    test_arch_names();
}
