#!/usr/bin/env python3
"""Rewrite FatFs subtype selection to trust a consistent BPB.

See components/fatfs/CMakeLists.txt.
"""
from __future__ import annotations

import pathlib
import sys

MARKER = "/* Determine the FAT sub type */"

OLD = """\t\t/* Determine the FAT sub type */
\t\tsysect = nrsv + fasize + fs->n_rootdir / (SS(fs) / SZDIRE);\t/* RSV + FAT + FF_DIR */
\t\tif (tsect < sysect) return FR_NO_FILESYSTEM;\t/* (Invalid volume size) */
\t\tnclst = (tsect - sysect) / fs->csize;\t\t\t/* Number of clusters */
\t\tif (nclst == 0) return FR_NO_FILESYSTEM;\t\t/* (Invalid volume size) */
\t\tfmt = 0;
\t\tif (nclst <= MAX_FAT32) fmt = FS_FAT32;
\t\tif (nclst <= MAX_FAT16) fmt = FS_FAT16;
\t\tif (nclst <= MAX_FAT12) fmt = FS_FAT12;
\t\tif (fmt == 0) return FR_NO_FILESYSTEM;
"""

NEW = """\t\t/* Determine the FAT sub type (ArgonOS: BPB first, then nclst) */
\t\tsysect = nrsv + fasize + fs->n_rootdir / (SS(fs) / SZDIRE);\t/* RSV + FAT + FF_DIR */
\t\tif (tsect < sysect) return FR_NO_FILESYSTEM;\t/* (Invalid volume size) */
\t\tnclst = (tsect - sysect) / fs->csize;\t\t\t/* Number of clusters */
\t\tif (nclst == 0) return FR_NO_FILESYSTEM;\t\t/* (Invalid volume size) */
\t\t/* Prefer BPB layout over the cluster-count heuristic.  A volume with
\t\t * FATSz16!=0, a fixed root, and BS_FilSysType "FAT16   " is FAT16 even
\t\t * when nclst is in the FAT12 numeric band (small host-sync images). */
\t\tfmt = 0;
\t\tif (ld_word(fs->win + BPB_FATSz16) == 0) {
\t\t\tif (nclst <= MAX_FAT32) fmt = FS_FAT32;
\t\t} else if (!memcmp(fs->win + BS_FilSysType, "FAT16   ", 8) && nclst <= MAX_FAT16) {
\t\t\tfmt = FS_FAT16;
\t\t} else if (!memcmp(fs->win + BS_FilSysType, "FAT12   ", 8) && nclst <= MAX_FAT12) {
\t\t\tfmt = FS_FAT12;
\t\t} else {
\t\t\tif (nclst <= MAX_FAT32) fmt = FS_FAT32;
\t\t\tif (nclst <= MAX_FAT16) fmt = FS_FAT16;
\t\t\tif (nclst <= MAX_FAT12) fmt = FS_FAT12;
\t\t}
\t\tif (fmt == 0) return FR_NO_FILESYSTEM;
"""

ALREADY = "Determine the FAT sub type (ArgonOS: BPB first, then nclst)"


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: patch_fatfs_bpb.py <ff.c.in> <ff.c.out>")
    raw = pathlib.Path(sys.argv[1]).read_bytes()
    # FatFs sources are ASCII; keep newline style of the input.
    nl = b"\r\n" if b"\r\n" in raw else b"\n"
    text = raw.decode("utf-8")
    norm = text.replace("\r\n", "\n")
    if ALREADY in norm:
        out = norm
    elif OLD not in norm:
        idx = norm.find(MARKER)
        hint = norm[idx : idx + 400] if idx >= 0 else "(marker missing)"
        raise SystemExit(
            "patch_fatfs_bpb: expected stock FatFs block not found.\n" + hint
        )
    else:
        out = norm.replace(OLD, NEW, 1)
    data = out.replace("\n", nl.decode("ascii")).encode("utf-8")
    pathlib.Path(sys.argv[2]).write_bytes(data)


if __name__ == "__main__":
    main()
