/*
 * ArgonOS - mapping the .DAT index out of flash.  See dat_index.h.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include "dat_index.h"

#include <argon/argon.h>

#include <stdint.h>
#include <string.h>

/* The header apps/fallout/tools/mkdatidx.py writes.  Little-endian, and every
 * offset counts from the start of the file, so it does not matter where the
 * mapping lands. */
typedef struct {
    char     magic[4];
    uint32_t version;
    uint32_t entries;
    int32_t  dataOffset;
    uint32_t entryTableOffset;
    uint32_t pathBlobOffset;
    uint32_t pathBlobLength;
    uint32_t datSize;
} dat_index_header_t;

#define DAT_INDEX_VERSION 1u
#define DAT_INDEX_ENTRY_SIZE 20u

/* "master.dat" → "master.IDX", in place, on a caller's buffer. */
static bool idx_path_of(const char *datPath, char *out, size_t cap)
{
    const size_t n = strlen(datPath);
    size_t       dot = n;

    if (n + 5 > cap) {
        return false;
    }
    while (dot > 0 && datPath[dot - 1] != '.') {
        if (datPath[dot - 1] == '\\' || datPath[dot - 1] == '/') {
            dot = 0;
            break;
        }
        dot--;
    }
    if (dot == 0) {
        /* No extension: append rather than replace. */
        memcpy(out, datPath, n);
        memcpy(out + n, ".IDX", 5);
        return true;
    }
    memcpy(out, datPath, dot);
    memcpy(out + dot, "IDX", 4);
    return true;
}

int falloutDatIndexMap(const char *datPath, unsigned int datSize,
                       FalloutDatIndex *out)
{
    char        path[128];
    const void *base = NULL;
    uint64_t    len = 0;

    if (datPath == NULL || out == NULL) {
        return 0;
    }
    if (!idx_path_of(datPath, path, sizeof(path))) {
        return 0;
    }
    /* ABI 0.32 and later.  argon.h asks for this check by name, and without it
     * an older kernel is a null call rather than a missing feature. */
    if (!AG_HAS(ag_api()->fs, map)) {
        return 0;
    }

    if (ag_map(path, &base, &len) != AG_OK || base == NULL) {
        return 0;
    }

    /*
     * Everything below is a reason to hand the mapping back and let the engine
     * do it the slow way.  None of them is fatal and none of them should be
     * silent: an index that is quietly ignored looks exactly like one that is
     * working, right up to the point where the memory it was supposed to save
     * is missing and nobody knows why.
     */
    {
        const dat_index_header_t *h = (const dat_index_header_t *)base;
        const char               *why = NULL;

        if (len < sizeof(*h)) {
            why = "too short";
        } else if (memcmp(h->magic, "AGDX", 4) != 0) {
            why = "not an index";
        } else if (h->version != DAT_INDEX_VERSION) {
            why = "made by another version of mkdatidx.py";
        } else if (h->datSize != datSize) {
            why = "built from a different archive";
        } else if ((uint64_t)h->entryTableOffset +
                       (uint64_t)h->entries * DAT_INDEX_ENTRY_SIZE >
                   len) {
            why = "entry table runs past the end";
        } else if ((uint64_t)h->pathBlobOffset + h->pathBlobLength > len) {
            why = "paths run past the end";
        } else if (h->entries == 0u || h->entries > (1u << 20)) {
            why = "entry count is not believable";
        }

        if (why != NULL) {
            ag_printf("fallout: %s ignored - %s\n", path, why);
            (void)ag_unmap(base);
            return 0;
        }

        out->base = base;
        out->entries = (const uint8_t *)base + h->entryTableOffset;
        out->pathBase = (const char *)base;
        out->count = (int)h->entries;
        out->dataOffset = h->dataOffset;

        ag_printf("fallout: %s mapped, %u entries, %u KB in flash\n", path,
                  (unsigned)h->entries, (unsigned)(len / 1024u));
        return 1;
    }
}

void falloutDatIndexUnmap(const FalloutDatIndex *idx)
{
    if (idx != NULL && idx->base != NULL) {
        (void)ag_unmap(idx->base);
    }
}
