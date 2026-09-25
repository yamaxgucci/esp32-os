#!/usr/bin/env python3
"""Extract a file from a FAT16 or FAT32 disk image made by mkfatimg / argon sync.

  python tools/fatget.py build/sdcard.img a:/shot.ppm -o build/shot.ppm
"""
from __future__ import annotations

import argparse
import os
import struct
import sys


SECTOR = 512


def _read_boot(img: bytes):
    """Geometry, the FAT, and the root directory, for FAT16 or FAT32.

    Which one it is is decided the way every reader decides it: by the number
    of clusters.  The name in the boot sector is a comment.

    The two differ where it matters here.  FAT32 puts its FAT size in the
    32-bit field at offset 36 and leaves the 16-bit one at 22 zero; its FAT
    entries are 32 bits wide with the top four reserved; and its root
    directory is an ordinary cluster chain starting wherever BPB_RootClus says,
    rather than a fixed area behind the FATs.
    """
    b = img[:SECTOR]
    if b[510] != 0x55 or b[511] != 0xAA:
        raise SystemExit("fatget: bad boot signature")
    spc = b[13]
    reserved = struct.unpack_from("<H", b, 14)[0]
    fats = b[16]
    root_ents = struct.unpack_from("<H", b, 17)[0]
    fat_secs = struct.unpack_from("<H", b, 22)[0]
    tot = struct.unpack_from("<H", b, 19)[0] or struct.unpack_from("<I", b, 32)[0]
    if fat_secs == 0:
        fat_secs = struct.unpack_from("<I", b, 36)[0]

    root_secs = (root_ents * 32 + SECTOR - 1) // SECTOR
    data_start = (reserved + fats * fat_secs + root_secs) * SECTOR
    clusters = (tot - reserved - fats * fat_secs - root_secs) // spc
    fat32 = clusters >= 65525

    fat_off = reserved * SECTOR
    if fat32:
        fat = [
            struct.unpack_from("<I", img, fat_off + n * 4)[0] & 0x0FFFFFFF
            for n in range((fat_secs * SECTOR) // 4)
        ]
        eoc = 0x0FFFFFF8
        root_cluster = struct.unpack_from("<I", b, 44)[0]
        root = _read_chain(img, fat, data_start, spc, eoc, root_cluster, None)
    else:
        fat = [
            struct.unpack_from("<H", img, fat_off + n * 2)[0]
            for n in range((fat_secs * SECTOR) // 2)
        ]
        eoc = 0xFFF8
        root = img[reserved * SECTOR + fats * fat_secs * SECTOR : data_start]
    return spc, data_start, fat, root, eoc


def _lfn_name(parts: list) -> str:
    if not parts:
        return ""
    seqs = sorted(parts, key=lambda x: x[0] & 0x1F)
    units = []
    for _, chunk in seqs:
        units.extend(chunk)
    chars = []
    for u in units:
        if u == 0 or u == 0xFFFF:
            break
        chars.append(u)
    return bytes(struct.pack("<" + "H" * len(chars), *chars)).decode(
        "utf-16le", errors="replace"
    )


def _short_name(name83: bytes) -> str:
    base = name83[:8].decode("ascii", "replace").rstrip()
    ext = name83[8:11].decode("ascii", "replace").rstrip()
    return f"{base}.{ext}" if ext else base


def _list_root(root: bytes):
    i = 0
    lfn_parts = []
    while i < len(root):
        e = root[i : i + 32]
        if e[0] == 0:
            break
        if e[0] == 0xE5:
            lfn_parts = []
            i += 32
            continue
        if e[11] == 0x0F:
            chunk = [
                struct.unpack_from("<H", e, off)[0]
                for off in (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
            ]
            lfn_parts.append((e[0], chunk))
            i += 32
            continue
        longname = _lfn_name(lfn_parts)
        short = _short_name(e[0:11])
        # The first cluster is split across the entry: low half at 26, high
        # half at 20.  On FAT16 the high half is always zero, which is why
        # reading only the low one worked until a volume had more than 65535
        # clusters in it.
        cl = (struct.unpack_from("<H", e, 20)[0] << 16) | \
            struct.unpack_from("<H", e, 26)[0]
        sz = struct.unpack_from("<I", e, 28)[0]
        attr = e[11]
        yield longname or short, short, cl, sz, attr
        lfn_parts = []
        i += 32


def _read_chain(img: bytes, fat, data_start: int, spc: int, eoc: int, cl: int,
                sz) -> bytes:
    """Follow one chain.  sz is None for a directory, whose length is its
    chain rather than a number in an entry."""
    data = bytearray()
    c = cl
    seen = set()
    while c < eoc and (sz is None or len(data) < sz):
        if c in seen or c < 2:
            raise SystemExit("fatget: bad cluster chain")
        seen.add(c)
        off = data_start + (c - 2) * spc * SECTOR
        data += img[off : off + spc * SECTOR]
        c = fat[c]
    return bytes(data) if sz is None else bytes(data[:sz])


def extract(image: str, guest: str, out: str) -> None:
    img = open(image, "rb").read()
    spc, data_start, fat, root, eoc = _read_boot(img)

    want = guest.replace("\\", "/").split("/")[-1]
    if ":" in want:
        want = want.split(":", 1)[-1]
    want = want.lstrip("/").lower()

    for longname, short, cl, sz, attr in _list_root(root):
        if attr & 0x10:
            continue
        names = {longname.lower(), short.lower()}
        if want not in names and want.replace("/", "") not in names:
            continue
        data = _read_chain(img, fat, data_start, spc, eoc, cl, sz)
        os.makedirs(os.path.dirname(os.path.abspath(out)) or ".", exist_ok=True)
        with open(out, "wb") as f:
            f.write(data)
        print(f"fatget: {guest} -> {out} ({len(data)} bytes)")
        return
    raise SystemExit(f"fatget: {guest!r} not found in root of {image}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", help="FAT16 or FAT32 image (build/sdcard.img)")
    ap.add_argument("guest", help="guest path, e.g. a:/shot.ppm or shot.ppm")
    ap.add_argument("-o", "--output", required=True, help="host output file")
    args = ap.parse_args()
    extract(args.image, args.guest, args.output)


if __name__ == "__main__":
    main()
