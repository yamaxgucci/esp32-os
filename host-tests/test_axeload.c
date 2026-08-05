/*
 * ArgonOS - application image loading tests.
 *
 * The important test here is not that the loader runs without crashing, it is
 * that its output is byte-for-byte what the linker would have produced had it
 * targeted those addresses in the first place.  Fixtures are built from the same
 * source at different bases; loading one at the other's bases has to reproduce
 * the other exactly.  If it does, the relocation is right, and if it does not,
 * the difference says where.
 *
 * The two parts of an image move independently, so the split fixtures are
 * relinked by *different* distances - a loader that used one bias for both would
 * pass a test where the distances matched, and fails this one.
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
#define FIXTURE_CONTIG "sample-contig.axe"
#define FIXTURE_CONTIG_RELINKED "sample-contig-relinked.axe"

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

static const uint8_t *code_of(const blob_t *b)
{
    return b->data + header_of(b)->code.offset;
}

static const uint8_t *data_of(const blob_t *b)
{
    return b->data + header_of(b)->data.offset;
}

/* Counts the bytes that differ, printing the first few - those say where. */
static int compare(const char *what, const uint8_t *got, const uint8_t *want,
                   uint32_t bytes)
{
    int differences = 0;
    for (uint32_t i = 0; i < bytes; i++) {
        if (got[i] != want[i]) {
            if (differences < 4) {
                printf("     %s differs at offset 0x%x: loaded %02x, "
                       "linker %02x\n",
                       what, (unsigned)i, got[i], want[i]);
            }
            differences++;
        }
    }
    return differences;
}

/*
 * A header describing the same image as if it had been linked at bases chosen so
 * that each part's load bias comes out as the distance between the two fixtures.
 * Everything the header states in linked addresses moves with it, so what the
 * test hands the loader is coherent rather than a header with one field poked.
 */
static ag_axe_header_t relinked_as(const ag_axe_header_t *nominal,
                                   const ag_axe_header_t *reference,
                                   const uint8_t *code, const uint8_t *data)
{
    ag_axe_header_t h = *nominal;

    h.code.base = (uint32_t)(uintptr_t)code -
                  (reference->code.base - nominal->code.base);
    if (nominal->data.size > 0) {
        h.data.base = (uint32_t)(uintptr_t)data -
                      (reference->data.base - nominal->data.base);
    }

    const bool entry_in_code =
        (uint32_t)(nominal->entry - nominal->code.base) < nominal->code.size;
    h.entry = entry_in_code
                  ? h.code.base + (nominal->entry - nominal->code.base)
                  : h.data.base + (nominal->entry - nominal->data.base);

    const bool slot_in_code =
        (uint32_t)(nominal->api_slot - nominal->code.base) < nominal->code.size;
    h.api_slot = slot_in_code
                     ? h.code.base + (nominal->api_slot - nominal->code.base)
                     : h.data.base + (nominal->api_slot - nominal->data.base);
    return h;
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

    const ag_axe_header_t *ha = header_of(&a);
    const ag_axe_header_t *hb = header_of(&b);

    /* Same source, same sizes, different link addresses. */
    AG_CHECK_INT(ha->code.size, hb->code.size);
    AG_CHECK_INT(ha->data.size, hb->data.size);
    AG_CHECK_INT(ha->data.file_size, hb->data.file_size);
    AG_CHECK_INT(ha->reloc_count, hb->reloc_count);
    AG_CHECK(ha->reloc_count > 0);

    /* Both parts moved, and by different distances - that is the point. */
    AG_CHECK(ha->code.base != hb->code.base);
    AG_CHECK(ha->data.base != hb->data.base);
    AG_CHECK((hb->code.base - ha->code.base) !=
             (hb->data.base - ha->data.base));

    /* The sample has writable statics, so it has a data part with bss. */
    AG_CHECK(ha->data.size > 0);
    AG_CHECK(ha->data.size > ha->data.file_size);

    /* And the code part is executed as it arrives: no bss in it. */
    AG_CHECK_INT(ha->code.size, ha->code.file_size);

    /*
     * The fixture has to exercise both directions of the split, otherwise the
     * relocation classification is untested: words in the code part pointing at
     * data (literal pool entries) and words in the data part pointing at code
     * or data (initialised pointers).
     */
    int in_code = 0, in_data = 0, to_code = 0, to_data = 0;
    for (uint32_t i = 0; i < ha->reloc_count; i++) {
        const uint32_t e = relocs_of(&a)[i];
        if (e & AG_AXE_R_IN_DATA) {
            in_data++;
        } else {
            in_code++;
        }
        if (e & AG_AXE_R_TO_DATA) {
            to_data++;
        } else {
            to_code++;
        }
    }
    AG_CHECK(in_code > 0);
    AG_CHECK(in_data > 0);
    AG_CHECK(to_code > 0);
    AG_CHECK(to_data > 0);
    printf("     (%d words in code, %d in data; %d point at code, %d at data)\n",
           in_code, in_data, to_code, to_data);

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

    /* An entry point outside the code part would jump into nothing. */
    bad = *h;
    bad.entry = bad.code.base + bad.code.size;
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    /* Nor may it point into the data part: data is not executable. */
    bad = *h;
    bad.entry = bad.data.base;
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    /* An API slot in neither part could not be written to. */
    bad = *h;
    bad.api_slot = bad.data.base + bad.data.size;
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    /* A part claiming more stored bytes than it has memory for. */
    bad = *h;
    bad.data.file_size = bad.data.size + 1;
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    /* The code part is executed as it arrives, so it may not claim bss. */
    bad = *h;
    bad.code.file_size = bad.code.size - 4;
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    /* No code at all is not an application. */
    bad = *h;
    bad.code.size = 0;
    bad.code.file_size = 0;
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    /*
     * Overlapping parts make an address ambiguous, and the loader decides which
     * bias a word needs by which part its target falls in.  Two answers is no
     * answer, so the file is refused rather than half-relocated.
     */
    bad = *h;
    bad.data.base = bad.code.base + 4;
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    /* Word alignment is what makes the low bits of a relocation free. */
    bad = *h;
    bad.data.base += 2;
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    /*
     * A split image that claims to be contiguous is a contradiction: the flag
     * says the code reaches its data by distance, so that distance is fixed.
     */
    bad = *h;
    bad.flags |= AG_AXE_CONTIGUOUS;
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    /*
     * The name is printed, logged and used as a task name, so a run of bytes
     * with no terminator is refused rather than read past.
     */
    bad = *h;
    memset(bad.name, 'A', sizeof(bad.name));
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    bad = *h;
    memset(bad.version, 'B', sizeof(bad.version));
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    /* Offsets have to be inside the file, whatever the header claims. */
    bad = *h;
    bad.data.offset = (uint32_t)a.size;
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    bad = *h;
    bad.reloc_count = (uint32_t)a.size;
    AG_CHECK_INT(ag_axe_validate(&bad, a.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    free(a.data);
}

static void test_contiguous_validates(void)
{
    blob_t c = load_file(FIXTURE_CONTIG);
    if (c.size == 0) {
        return;
    }
    const ag_axe_header_t *h = header_of(&c);

    AG_CHECK((h->flags & AG_AXE_CONTIGUOUS) != 0);
    AG_CHECK_INT(h->data.base, h->code.base + h->code.size);
    AG_CHECK_INT(ag_axe_validate(h, c.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 AG_OK);

    /* Adjacent is the contract, and a gap breaks it. */
    ag_axe_header_t bad = *h;
    bad.data.base += 16;
    AG_CHECK_INT(ag_axe_validate(&bad, c.size, AG_ARCH_XTENSA, h->abi_major,
                                 h->abi_minor),
                 -AG_EFORMAT);

    free(c.data);
}

/*
 * The one that matters: relocating the nominal image to the other fixture's
 * bases must reproduce that fixture byte for byte, in both parts.
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

    uint8_t *code = (uint8_t *)malloc(ha->code.size);
    uint8_t *data = (uint8_t *)malloc(ha->data.size);
    AG_CHECK(code != NULL && data != NULL);
    if (code == NULL || data == NULL) {
        free(code);
        free(data);
        free(a.data);
        free(b.data);
        return;
    }

    memcpy(code, code_of(&a), ha->code.file_size);
    memcpy(data, data_of(&a), ha->data.file_size);
    /* Deliberately dirty, to show that bss is zeroed rather than assumed. */
    memset(data + ha->data.file_size, 0x5a, ha->data.size - ha->data.file_size);

    const ag_axe_header_t shifted = relinked_as(ha, hb, code, data);
    const ag_axe_place_t  place = {code, ha->code.size, data, ha->data.size};

    ag_axe_binding_t binding;
    AG_CHECK_INT(ag_axe_apply(&shifted, &place, relocs_of(&a), ha->reloc_count,
                              &binding),
                 AG_OK);
    AG_CHECK_INT(binding.relocated, ha->reloc_count);

    /* Byte for byte identical to what the linker produced for those bases. */
    AG_CHECK_INT(compare("code", code, code_of(&b), hb->code.file_size), 0);
    AG_CHECK_INT(compare("data", data, data_of(&b), hb->data.file_size), 0);

    /* And bss really is zero, without the caller having cleared anything. */
    int nonzero = 0;
    for (uint32_t i = ha->data.file_size; i < ha->data.size; i++) {
        nonzero += (data[i] != 0);
    }
    AG_CHECK_INT(nonzero, 0);

    free(code);
    free(data);
    free(a.data);
    free(b.data);
}

/* The same proof for a contiguous image, whose parts share one allocation. */
static void test_contiguous_relocation_matches_a_direct_link(void)
{
    blob_t a = load_file(FIXTURE_CONTIG);
    blob_t b = load_file(FIXTURE_CONTIG_RELINKED);
    if (a.size == 0 || b.size == 0) {
        free(a.data);
        free(b.data);
        return;
    }

    const ag_axe_header_t *ha = header_of(&a);
    const ag_axe_header_t *hb = header_of(&b);

    const size_t total = (size_t)ha->code.size + ha->data.size;
    uint8_t     *buf = (uint8_t *)malloc(total);
    AG_CHECK(buf != NULL);
    if (buf == NULL) {
        free(a.data);
        free(b.data);
        return;
    }

    memcpy(buf, code_of(&a), ha->code.file_size);
    uint8_t *data = buf + ha->code.size;
    memcpy(data, data_of(&a), ha->data.file_size);

    const ag_axe_header_t shifted = relinked_as(ha, hb, buf, data);
    const ag_axe_place_t  place = {buf, ha->code.size, data, ha->data.size};

    ag_axe_binding_t binding;
    AG_CHECK_INT(ag_axe_apply(&shifted, &place, relocs_of(&a), ha->reloc_count,
                              &binding),
                 AG_OK);

    AG_CHECK_INT(compare("code", buf, code_of(&b), hb->code.file_size), 0);
    AG_CHECK_INT(compare("data", data, data_of(&b), hb->data.file_size), 0);

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

    uint8_t *code = (uint8_t *)malloc(h->code.size);
    uint8_t *data = (uint8_t *)malloc(h->data.size);
    AG_CHECK(code != NULL && data != NULL);
    if (code == NULL || data == NULL) {
        free(code);
        free(data);
        free(a.data);
        return;
    }
    memcpy(code, code_of(&a), h->code.file_size);
    memcpy(data, data_of(&a), h->data.file_size);

    const ag_axe_place_t place = {code, h->code.size, data, h->data.size};

    ag_axe_binding_t binding;
    AG_CHECK_INT(ag_axe_apply(h, &place, relocs_of(&a), h->reloc_count,
                              &binding),
                 AG_OK);

    /* The entry point is in the code part, at the offset the header describes. */
    AG_CHECK_INT((uint8_t *)binding.entry - code, h->entry - h->code.base);
    AG_CHECK((uint8_t *)binding.entry >= code);
    AG_CHECK((uint8_t *)binding.entry < code + h->code.size);

    /*
     * The API slot is a variable, so it is in the data part - in its bss, which
     * is why it reads as zero before the kernel writes the table in.
     */
    AG_CHECK_INT((uint8_t *)binding.api_slot - data, h->api_slot - h->data.base);
    AG_CHECK_INT(binding.code_base, (uintptr_t)code);
    AG_CHECK_INT(binding.data_base, (uintptr_t)data);

    /*
     * Read as a 32-bit word, not as a host pointer: on a 64-bit machine that
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

    /* Resolving an address in neither part says so rather than guessing. */
    AG_CHECK(ag_axe_resolve(h, &place, h->code.base - 4) == NULL);
    AG_CHECK(ag_axe_resolve(h, &place, h->data.base + h->data.size) == NULL);
    AG_CHECK(ag_axe_resolve(h, &place, h->code.base) == code);
    AG_CHECK(ag_axe_resolve(h, &place, h->data.base) == data);

    free(code);
    free(data);
    free(a.data);
}

static void test_apply_rejects_bad_input(void)
{
    blob_t a = load_file(FIXTURE_NOMINAL);
    if (a.size == 0) {
        return;
    }
    const ag_axe_header_t *h = header_of(&a);

    uint8_t *code = (uint8_t *)malloc(h->code.size);
    uint8_t *data = (uint8_t *)malloc(h->data.size);
    AG_CHECK(code != NULL && data != NULL);
    if (code == NULL || data == NULL) {
        free(code);
        free(data);
        free(a.data);
        return;
    }

    ag_axe_binding_t binding;
    const ag_axe_place_t good = {code, h->code.size, data, h->data.size};

    AG_CHECK_INT(ag_axe_apply(NULL, &good, relocs_of(&a), h->reloc_count,
                              &binding),
                 -AG_EINVAL);
    AG_CHECK_INT(ag_axe_apply(h, NULL, relocs_of(&a), h->reloc_count, &binding),
                 -AG_EINVAL);

    /* Too small an allocation is caught before anything is written. */
    ag_axe_place_t small = good;
    small.code_capacity = h->code.size - 1;
    AG_CHECK_INT(ag_axe_apply(h, &small, relocs_of(&a), h->reloc_count,
                              &binding),
                 -AG_ENOMEM);

    small = good;
    small.data_capacity = h->data.size - 1;
    AG_CHECK_INT(ag_axe_apply(h, &small, relocs_of(&a), h->reloc_count,
                              &binding),
                 -AG_ENOMEM);

    /* An image with a data part needs somewhere to put it. */
    ag_axe_place_t nodata = good;
    nodata.data = NULL;
    AG_CHECK_INT(ag_axe_apply(h, &nodata, relocs_of(&a), h->reloc_count,
                              &binding),
                 -AG_EINVAL);

    /* A count that disagrees with the header means the file is inconsistent. */
    AG_CHECK_INT(ag_axe_apply(h, &good, relocs_of(&a), h->reloc_count + 1,
                              &binding),
                 -AG_EINVAL);

    /*
     * A whole table, so that the count still matches the header: a short one
     * would be refused for that reason instead of the one being tested.
     */
    const uint32_t count = h->reloc_count;
    uint32_t      *evil = (uint32_t *)malloc(count * sizeof(uint32_t));
    AG_CHECK(evil != NULL);
    if (evil == NULL) {
        free(code);
        free(data);
        free(a.data);
        return;
    }

    /* A relocation pointing outside the stored bytes must not be applied. */
    memcpy(evil, relocs_of(&a), count * sizeof(uint32_t));
    evil[0] = h->code.size; /* one word past the end of the code part */
    AG_CHECK_INT(ag_axe_apply(h, &good, evil, count, &binding), -AG_EFORMAT);

    memcpy(evil, relocs_of(&a), count * sizeof(uint32_t));
    evil[0] = h->data.file_size | AG_AXE_R_IN_DATA;
    AG_CHECK_INT(ag_axe_apply(h, &good, evil, count, &binding), -AG_EFORMAT);

    /* An offset so large it would wrap the bounds check is still refused. */
    memcpy(evil, relocs_of(&a), count * sizeof(uint32_t));
    evil[0] = 0xfffffffcu;
    AG_CHECK_INT(ag_axe_apply(h, &good, evil, count, &binding), -AG_EFORMAT);

    /*
     * And a relocation about a data part in an image that has none: the header
     * and the table contradict each other, so neither is trusted.
     */
    ag_axe_header_t codeonly = *h;
    codeonly.data.size = 0;
    codeonly.data.file_size = 0;
    codeonly.api_slot = codeonly.code.base;
    const ag_axe_place_t place_codeonly = {code, h->code.size, NULL, 0};
    memcpy(evil, relocs_of(&a), count * sizeof(uint32_t));
    evil[0] = AG_AXE_R_TO_DATA;
    AG_CHECK_INT(ag_axe_apply(&codeonly, &place_codeonly, evil, count, &binding),
                 -AG_EFORMAT);

    free(evil);
    free(code);
    free(data);
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
    test_contiguous_validates();
    test_relocation_matches_a_direct_link();
    test_contiguous_relocation_matches_a_direct_link();
    test_entry_and_api_slot();
    test_apply_rejects_bad_input();
    test_arch_names();
}
