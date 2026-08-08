#!/usr/bin/env python3
"""Build a FAT16 disk image from a host directory (no external tools).

Used by `argon sync` so QEMU can see Windows files on A: via build/sdcard.img.

  python tools/mkfatimg.py -o build/sdcard.img -s 64 path/to/folder
"""
from __future__ import annotations

import argparse
import os
import struct
from dataclasses import dataclass, field
from datetime import datetime
from typing import List, Set, Tuple


SECTOR = 512


def _fat_time(dt: datetime) -> Tuple[int, int]:
    year = max(1980, min(2107, dt.year))
    dos_date = ((year - 1980) << 9) | (dt.month << 5) | dt.day
    dos_time = (dt.hour << 11) | (dt.minute << 5) | (dt.second // 2)
    return dos_date, dos_time


def _checksum_83(name83: bytes) -> int:
    s = 0
    for b in name83:
        s = ((s & 1) << 7) + (s >> 1) + b
        s &= 0xFF
    return s


def _clean_part(s: str, n: int) -> bytes:
    out = []
    for ch in s.upper():
        o = ord(ch)
        if 32 < o < 127 and ch not in ' "/\\[]:;|=,*?':
            out.append(o)
        else:
            out.append(ord("_"))
    return (bytes(out) + b" " * n)[:n]


def _unique_83(filename: str, used: Set[bytes]) -> bytes:
    base, ext = os.path.splitext(filename)
    ext = ext[1:] if ext.startswith(".") else ""
    simple = _clean_part(base, 8) + _clean_part(ext, 3)
    if (
        simple not in used
        and len(base) <= 8
        and len(ext) <= 3
        and base.upper() == _clean_part(base, 8).decode("ascii").strip()
        and ext.upper() == _clean_part(ext, 3).decode("ascii").strip()
    ):
        used.add(simple)
        return simple

    stem = "".join(c if c.isalnum() else "_" for c in base.upper()) or "FILE"
    e = "".join(c if c.isalnum() else "_" for c in ext.upper())[:3]
    for n in range(1, 1000):
        num = str(n)
        left = f"{stem[: max(1, 8 - 1 - len(num))]}~{num}"
        cand = left.encode("ascii")[:8].ljust(8) + e.encode("ascii")[:3].ljust(3)
        if cand not in used:
            used.add(cand)
            return cand
    raise SystemExit(f"mkfatimg: cannot invent 8.3 name for {filename!r}")


def _lfn_entries(long_name: str, name83: bytes) -> List[bytes]:
    chk = _checksum_83(name83)
    units = list(long_name.encode("utf-16le"))
    u16 = [units[i] | (units[i + 1] << 8) for i in range(0, len(units), 2)]
    u16.append(0x0000)
    while len(u16) % 13 != 0:
        u16.append(0xFFFF)
    chunks = [u16[i : i + 13] for i in range(0, len(u16), 13)]
    total = len(chunks)
    entries: List[bytes] = []
    for seq, chunk in enumerate(reversed(chunks), start=1):
        ordinal = total - seq + 1
        if seq == 1:
            ordinal |= 0x40
        buf = bytearray(32)
        buf[0] = ordinal
        for i, c in enumerate(chunk[0:5]):
            struct.pack_into("<H", buf, 1 + i * 2, c)
        buf[11] = 0x0F
        buf[13] = chk
        for i, c in enumerate(chunk[5:11]):
            struct.pack_into("<H", buf, 14 + i * 2, c)
        for i, c in enumerate(chunk[11:13]):
            struct.pack_into("<H", buf, 28 + i * 2, c)
        entries.append(bytes(buf))
    return entries


def _dir_entry(name83: bytes, attr: int, cluster: int, size: int, mtime: datetime) -> bytes:
    d, t = _fat_time(mtime)
    buf = bytearray(32)
    buf[0:11] = name83
    buf[11] = attr
    struct.pack_into("<H", buf, 14, t)
    struct.pack_into("<H", buf, 16, d)
    struct.pack_into("<H", buf, 18, d)
    struct.pack_into("<H", buf, 20, (cluster >> 16) & 0xFFFF)
    struct.pack_into("<H", buf, 22, t)
    struct.pack_into("<H", buf, 24, d)
    struct.pack_into("<H", buf, 26, cluster & 0xFFFF)
    struct.pack_into("<I", buf, 28, size)
    return bytes(buf)


@dataclass
class Node:
    name: str
    is_dir: bool
    mtime: datetime
    data: bytes = b""
    children: List["Node"] = field(default_factory=list)
    cluster: int = 0


# FatFs / MS rule: cluster_count < 4085 → FAT12, else FAT16 until 65525.
# We only emit 16-bit FAT entries, so geometry MUST stay in the FAT16 band.
MIN_FAT16_CLUSTERS = 4085
MAX_FAT16_CLUSTERS = 65524


class FatImage:
    def __init__(self, size_mb: int):
        if size_mb < 4 or size_mb > 512:
            raise SystemExit("mkfatimg: size must be 4..512 MiB for FAT16")
        self.size = size_mb * 1024 * 1024
        self.reserved = 1
        self.num_fats = 2
        self.root_ents = 512
        self.root_secs = (self.root_ents * 32 + SECTOR - 1) // SECTOR

        # Prefer smaller clusters on small images so n_clusters stays ≥ 4085.
        for spc in (1, 2, 4, 8, 16, 32, 64):
            self.sec_per_clus = spc
            self.fat_secs = 1
            while True:
                data_secs = (
                    self.size // SECTOR
                    - self.reserved
                    - self.num_fats * self.fat_secs
                    - self.root_secs
                )
                if data_secs <= 0:
                    self.clusters = 0
                    break
                self.clusters = data_secs // self.sec_per_clus
                need = (self.clusters + 2 + 255) // 256
                if need <= self.fat_secs:
                    break
                self.fat_secs = need
            if MIN_FAT16_CLUSTERS <= self.clusters <= MAX_FAT16_CLUSTERS:
                break
        else:
            raise SystemExit(
                f"mkfatimg: cannot build FAT16 for {size_mb} MiB "
                f"(got {self.clusters} clusters); try -s 16 or larger"
            )

        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0xFFF8
        self.fat[1] = 0xFFFF
        self.next_cluster = 2
        self.image = bytearray(self.size)
        self._write_boot()

    def _write_boot(self) -> None:
        b = bytearray(SECTOR)
        b[0:3] = b"\xeb\x3c\x90"
        b[3:11] = b"ARGONOS "
        struct.pack_into("<H", b, 11, SECTOR)
        b[13] = self.sec_per_clus
        struct.pack_into("<H", b, 14, self.reserved)
        b[16] = self.num_fats
        struct.pack_into("<H", b, 17, self.root_ents)
        tot = self.size // SECTOR
        if tot < 0x10000:
            struct.pack_into("<H", b, 19, tot)
        else:
            struct.pack_into("<I", b, 32, tot)
        b[21] = 0xF8
        struct.pack_into("<H", b, 22, self.fat_secs)
        struct.pack_into("<H", b, 24, 32)
        struct.pack_into("<H", b, 26, 16)
        b[38] = 0x29
        struct.pack_into("<I", b, 39, 0x41524F4E)
        b[43:54] = b"ARGONSHARE "
        b[54:62] = b"FAT16   "
        b[510] = 0x55
        b[511] = 0xAA
        self.image[0:SECTOR] = b

    def alloc_clusters(self, nbytes: int) -> int:
        if nbytes <= 0:
            nbytes = 1
        need = (nbytes + self.sec_per_clus * SECTOR - 1) // (self.sec_per_clus * SECTOR)
        if self.next_cluster + need > self.clusters + 2:
            raise SystemExit(
                f"mkfatimg: image full (need {need} clusters, "
                f"{self.clusters + 2 - self.next_cluster} left)"
            )
        first = self.next_cluster
        for i in range(need):
            c = first + i
            self.fat[c] = 0xFFFF if i == need - 1 else c + 1
        self.next_cluster += need
        return first

    def cluster_offset(self, cluster: int) -> int:
        data_start = (
            self.reserved + self.num_fats * self.fat_secs + self.root_secs
        ) * SECTOR
        return data_start + (cluster - 2) * self.sec_per_clus * SECTOR

    def write_file_data(self, first: int, data: bytes) -> None:
        if first == 0:
            return
        c = first
        off = 0
        chunk = self.sec_per_clus * SECTOR
        while True:
            o = self.cluster_offset(c)
            piece = data[off : off + chunk]
            self.image[o : o + len(piece)] = piece
            off += len(piece)
            nxt = self.fat[c]
            if nxt >= 0xFFF8:
                break
            c = nxt

    def flush_fat_and_root(self, root_bytes: bytes) -> None:
        fat_bytes = bytearray(self.fat_secs * SECTOR)
        for i, v in enumerate(self.fat):
            struct.pack_into("<H", fat_bytes, i * 2, v & 0xFFFF)
        fat_off = self.reserved * SECTOR
        for n in range(self.num_fats):
            o = fat_off + n * len(fat_bytes)
            self.image[o : o + len(fat_bytes)] = fat_bytes
        root_off = fat_off + self.num_fats * len(fat_bytes)
        root_area = bytearray(self.root_secs * SECTOR)
        root_area[: len(root_bytes)] = root_bytes
        self.image[root_off : root_off + len(root_area)] = root_area


def scan_dir(path: str) -> Node:
    st = os.stat(path)
    node = Node(
        name=os.path.basename(path.rstrip("\\/")) or path,
        is_dir=True,
        mtime=datetime.fromtimestamp(st.st_mtime),
    )
    try:
        names = sorted(os.listdir(path), key=lambda s: s.lower())
    except OSError as e:
        raise SystemExit(f"mkfatimg: cannot read {path}: {e}") from e
    for name in names:
        full = os.path.join(path, name)
        if os.path.isdir(full):
            child = scan_dir(full)
            child.name = name
            node.children.append(child)
        elif os.path.isfile(full):
            stf = os.stat(full)
            with open(full, "rb") as f:
                data = f.read()
            node.children.append(
                Node(
                    name=name,
                    is_dir=False,
                    mtime=datetime.fromtimestamp(stf.st_mtime),
                    data=data,
                )
            )
    return node


def build_subdir(
    img: FatImage,
    node: Node,
    parent_cluster: int,
    self_cluster: int,
    allocate: bool,
) -> bytes:
    used: Set[bytes] = set()
    dot = b"." + b" " * 10
    dotdot = b".." + b" " * 9
    records: List[bytes] = [
        _dir_entry(dot, 0x10, self_cluster, 0, node.mtime),
        _dir_entry(dotdot, 0x10, parent_cluster, 0, node.mtime),
    ]
    used.add(dot)
    used.add(dotdot)

    items = []
    for child in node.children:
        name83 = _unique_83(child.name, used)
        items.append((child, name83, _lfn_entries(child.name, name83)))

    for child, name83, lfn in items:
        if child.is_dir:
            continue
        if allocate:
            child.cluster = img.alloc_clusters(len(child.data))
            img.write_file_data(child.cluster, child.data)
        for e in lfn:
            records.append(e)
        records.append(
            _dir_entry(
                name83,
                0x20,
                child.cluster if allocate else 0,
                len(child.data),
                child.mtime,
            )
        )

    for child, name83, lfn in items:
        if not child.is_dir:
            continue
        if allocate:
            probe = build_subdir(img, child, self_cluster, 2, allocate=False)
            child.cluster = img.alloc_clusters(len(probe))
            body = build_subdir(img, child, self_cluster, child.cluster, allocate=True)
            img.write_file_data(child.cluster, body)
        for e in lfn:
            records.append(e)
        records.append(
            _dir_entry(name83, 0x10, child.cluster if allocate else 0, 0, child.mtime)
        )

    return b"".join(records)


def pack(host_dir: str, out_path: str, size_mb: int) -> None:
    host_dir = os.path.abspath(host_dir)
    if not os.path.isdir(host_dir):
        raise SystemExit(f"mkfatimg: not a directory: {host_dir}")

    root = scan_dir(host_dir)
    img = FatImage(size_mb)
    used: Set[bytes] = set()
    root_records = bytearray()
    items = []
    for child in root.children:
        name83 = _unique_83(child.name, used)
        items.append((child, name83, _lfn_entries(child.name, name83)))

    for child, name83, lfn in items:
        if child.is_dir:
            continue
        child.cluster = img.alloc_clusters(len(child.data))
        img.write_file_data(child.cluster, child.data)
        for e in lfn:
            root_records += e
        root_records += _dir_entry(name83, 0x20, child.cluster, len(child.data), child.mtime)

    for child, name83, lfn in items:
        if not child.is_dir:
            continue
        probe = build_subdir(img, child, 0, 2, allocate=False)
        child.cluster = img.alloc_clusters(len(probe))
        body = build_subdir(img, child, 0, child.cluster, allocate=True)
        img.write_file_data(child.cluster, body)
        for e in lfn:
            root_records += e
        root_records += _dir_entry(name83, 0x10, child.cluster, 0, child.mtime)

    max_root = img.root_secs * SECTOR
    if len(root_records) > max_root:
        raise SystemExit(
            f"mkfatimg: root directory full ({len(root_records)} > {max_root} bytes); "
            "put files in a subdirectory"
        )

    img.flush_fat_and_root(bytes(root_records))
    parent = os.path.dirname(os.path.abspath(out_path))
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(img.image)

    n_files = 0
    n_dirs = 0

    def count(n: Node) -> None:
        nonlocal n_files, n_dirs
        for c in n.children:
            if c.is_dir:
                n_dirs += 1
                count(c)
            else:
                n_files += 1

    count(root)
    print(
        f"mkfatimg: wrote {out_path} ({size_mb} MiB FAT16, "
        f"{n_files} file(s), {n_dirs} dir(s) from {host_dir})"
    )


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("directory", help="host folder to pack")
    ap.add_argument("-o", "--output", default="build/sdcard.img")
    ap.add_argument(
        "-s",
        "--size-mb",
        type=int,
        default=64,
        help="image size in MiB (default 64; keep ≥16 so FatFs sees FAT16)",
    )
    args = ap.parse_args()
    pack(args.directory, args.output, args.size_mb)


if __name__ == "__main__":
    main()
