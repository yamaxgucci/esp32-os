/*
 * ArgonOS - the .DAT index, read from flash instead of built in memory.
 *
 * A Fallout archive keeps its table of contents at the end and the engine
 * rebuilds it in the arena at every start: 30240 entries for Fallout 2, about
 * 1.7 MB and ten seconds of small reads, to reconstruct something that cannot
 * change while the archive does not.  That is a fifth of the memory an 8 MB
 * board has, spent on a constant.
 *
 * `ag_map` puts a file in the data address space instead - staged into the
 * appfs flash partition, fetched by the same cache that fetches instructions,
 * at no cost in RAM.  So the index is computed on the host by
 * apps/fallout/tools/mkdatidx.py, written next to the .DAT as .IDX, and read
 * here where it lies.
 *
 * This is the whole of the ArgonOS side of it, kept out of the engine so that
 * the patch to a vendored dbaseOpen is a dozen lines and holds across an
 * upstream that knows nothing about any of this.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_FALLOUT_DAT_INDEX_H
#define ARGON_FALLOUT_DAT_INDEX_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What dbaseOpen needs to fill a DBase in, all of it pointing into the mapping.
 *
 * `entries` is the DBaseEntry array itself: the file is written in that exact
 * layout so the engine's binary search runs over these bytes with nothing
 * unpacked and nothing copied.  A path is `pathBase + entry->pathOffset`.
 */
typedef struct {
    const void *base;      /* give this back to falloutDatIndexUnmap */
    const void *entries;   /* const DBaseEntry[count] */
    const char *pathBase;  /* entry->pathOffset counts from here */
    int         count;
    int         dataOffset;
} FalloutDatIndex;

/*
 * Look for the .IDX beside `datPath` and map it.
 *
 * `datSize` is the archive's size in bytes, which the index carries a copy of:
 * an .IDX that does not match its .DAT describes files at offsets that are no
 * longer there, and every read would come back as plausible rubbish.  It is
 * refused instead, and the engine builds the index the old way.
 *
 * Returns 1 on success, 0 if there is no usable index - which is not an error
 * and is never fatal.
 */
int falloutDatIndexMap(const char *datPath, unsigned int datSize,
                       FalloutDatIndex *out);

void falloutDatIndexUnmap(const FalloutDatIndex *idx);

#ifdef __cplusplus
}
#endif

#endif /* ARGON_FALLOUT_DAT_INDEX_H */
