#!/usr/bin/env python3
"""Write the .DAT index out as a file the guest can map instead of build.

A Fallout .DAT keeps its table of contents at the end, and the engine reads it
into memory at every start: 30240 entries between MASTER.DAT and CRITTER.DAT,
each one a descriptor plus a separately allocated path.  That is about 1.7 MB
of the arena and about ten seconds of small reads, every run, to reconstruct
something that never changes for a given archive.

ArgonOS can map a file into the data address space instead (`ag_map`, see
components/argon_kernel/src/fs/filemap.c): the bytes sit in the appfs flash
partition and the cache that fetches instructions fetches them too, at no cost
in RAM.  So the index is computed here, once, on a machine with memory to
spare, and the guest reads it where it lies.

    python apps/fallout/tools/mkdatidx.py build/fo2_card/master.dat
    python apps/fallout/tools/mkdatidx.py build/fo2_card/critter.dat

The output goes next to the input as .IDX, which is where dbaseOpen looks for
it.  Without one the engine builds the index the old way, so an .IDX is an
optimisation and never a requirement - but it must match its .DAT, and the
header carries the archive's size so that a stale one is refused rather than
believed.

The layout is what the guest wants to see, not what is convenient here.  Every
offset is from the start of this file, so it does not matter where the mapping
lands; the entry table is the guest's DBaseEntry array verbatim, so the binary
search runs over these bytes with no unpacking at all.

    0   char     magic[4]        "AGDX"
    4   uint32   version         1
    8   uint32   entries
    12  int32    dataOffset      where the archive's data section starts
    16  uint32   entry_table_off 32
    20  uint32   path_blob_off
    24  uint32   path_blob_len
    28  uint32   dat_size        the .DAT this was made from, to catch a pair
                                 that has drifted apart
    32  entry[]  { uint32 path_off; uint32 compressed;
                   int32 uncompressed_size; int32 data_size; int32 data_off }
        blob     the paths, NUL-terminated, in entry order

Entry order is the archive's own and is not touched: the engine finds a file by
binary search over this table, which only works because the .DAT was written
sorted.  Sorting it here to be tidy would break every lookup.

Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
"""
from __future__ import annotations

import argparse
import os
import struct
import sys

MAGIC = b"AGDX"
VERSION = 1
HEADER_SIZE = 32
ENTRY_SIZE = 20


def read_dat_entries(path: str):
    """The archive's table of contents, in the order it is stored."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        f.seek(size - 8)
        entries_data_size, dbase_data_size = struct.unpack("<ii", f.read(8))

        # The whole table in one read.  Entry by entry is what makes this slow
        # on the guest; there is no reason to be slow here as well.
        f.seek(size - entries_data_size - 8)
        table = f.read(entries_data_size)

    (count,) = struct.unpack_from("<i", table, 0)
    if count <= 0 or count > 1 << 20:
        raise SystemExit("%s: %d entries is not a .DAT2 archive"
                         % (path, count))

    entries = []
    pos = 4  # past the count
    for _ in range(count):
        (path_len,) = struct.unpack_from("<i", table, pos)
        pos += 4
        name = table[pos:pos + path_len]
        pos += path_len
        compressed, uncompressed_size, data_size, data_off = struct.unpack_from(
            "<Biii", table, pos)
        pos += 13
        entries.append((name, compressed, uncompressed_size, data_size,
                        data_off))

    if pos != len(table):
        raise SystemExit("%s: %d bytes left over after %d entries - the file "
                         "is not the format this reads"
                         % (path, len(table) - pos, count))

    return entries, size - dbase_data_size, size


def write_idx(out_path: str, entries, data_offset: int, dat_size: int) -> None:
    table_off = HEADER_SIZE
    blob_off = table_off + ENTRY_SIZE * len(entries)

    # One blob, built once: identical paths are rare in a .DAT, so there is
    # nothing to gain by pooling them and a plain concatenation keeps the file
    # in the same order as the table.
    blob = bytearray()
    table = bytearray()
    for name, compressed, uncompressed_size, data_size, data_off in entries:
        path_off = blob_off + len(blob)
        blob += name + b"\0"
        table += struct.pack("<IIiii", path_off, compressed,
                             uncompressed_size, data_size, data_off)

    header = struct.pack("<4sIIiIIII", MAGIC, VERSION, len(entries),
                         data_offset, table_off, blob_off, len(blob),
                         dat_size)

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(table)
        f.write(blob)


def verify(out_path: str, entries) -> None:
    """Read the file back and check it says what the archive said.

    Worth the second it takes.  A wrong offset here is not a crash: the guest
    maps the file, believes it, and hands the engine a table that is subtly not
    the archive's - and the failure surfaces a quarter of an hour later as a
    game that cannot find a file which is demonstrably in the .DAT.
    """
    raw = open(out_path, "rb").read()
    (magic, version, count, data_off, table_off, blob_off, blob_len,
     dat_size) = struct.unpack_from("<4sIIiIIII", raw, 0)
    if magic != MAGIC or version != VERSION:
        raise SystemExit("mkdatidx: %s came out wrong" % out_path)
    if count != len(entries):
        raise SystemExit("mkdatidx: %s has %d entries, expected %d"
                         % (out_path, count, len(entries)))

    prev = None
    for i, (name, compressed, usize, dsize, doff) in enumerate(entries):
        (p_off, p_comp, p_usize, p_dsize, p_doff) = struct.unpack_from(
            "<IIiii", raw, table_off + i * ENTRY_SIZE)
        if p_off < blob_off or p_off >= blob_off + blob_len:
            raise SystemExit("mkdatidx: entry %d points outside the blob" % i)
        end = raw.index(bytes(1), p_off)
        got = raw[p_off:end]
        if (got, p_comp, p_usize, p_dsize, p_doff) != (name, compressed,
                                                       usize, dsize, doff):
            raise SystemExit("mkdatidx: entry %d does not round-trip: %r"
                             % (i, got))
        # The engine finds a file by binary search over this table and never
        # sorts it, so an archive whose entries are not in order is one whose
        # files cannot all be found - with or without this file.  Say so.
        low = got.lower()
        if prev is not None and low < prev:
            print("  warning: %s is not sorted at entry %d (%s after %s) - "
                  "the engine's binary search will miss files"
                  % (os.path.basename(out_path), i, got.decode("latin1"),
                     prev.decode("latin1")))
            prev = None
            continue
        prev = low


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dat", nargs="+", help=".DAT files to index")
    ap.add_argument("--no-verify", action="store_true",
                    help="skip reading the result back and checking it")
    ap.add_argument("-o", "--out",
                    help="write here instead of next to the .DAT "
                         "(only with a single input)")
    args = ap.parse_args()

    if args.out is not None and len(args.dat) != 1:
        raise SystemExit("mkdatidx: -o takes one .DAT at a time")

    for dat in args.dat:
        entries, data_offset, dat_size = read_dat_entries(dat)
        out = args.out or (os.path.splitext(dat)[0] + ".IDX")
        write_idx(out, entries, data_offset, dat_size)
        if not args.no_verify:
            verify(out, entries)

        blob = sum(len(e[0]) + 1 for e in entries)
        total = HEADER_SIZE + ENTRY_SIZE * len(entries) + blob
        # What the engine would have spent instead: a descriptor and a
        # separate allocation per path, and the allocator's header on each.
        # 8 bytes is this system's; it is the smaller half of the difference.
        in_arena = len(entries) * (20 + 8) + blob + len(entries) * 8
        print("%s -> %s: %d entries, %d KB (%d KB in the arena otherwise)"
              % (os.path.basename(dat), os.path.basename(out), len(entries),
                 total // 1024, in_arena // 1024))

    return 0


if __name__ == "__main__":
    sys.exit(main())
