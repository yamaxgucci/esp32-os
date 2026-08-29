#!/usr/bin/env python3
"""Build a FAT16 or FAT32 disk image from a host directory (no external tools).

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
    path: str = ""
    size: int = 0
    children: List["Node"] = field(default_factory=list)
    cluster: int = 0

    def read(self) -> bytes:
        """The file's contents, at the moment they are needed.

        Not held on the node: a card with a game on it is most of a gigabyte,
        and the image is already that size in memory.  Reading each file when
        its clusters are written keeps one copy rather than two.
        """
        if self.is_dir or not self.path:
            return b""
        with open(self.path, "rb") as f:
            return f.read()


# The cluster count decides the format, and nothing else does - the name in the
# boot sector is a comment as far as a driver is concerned.  Under 4085 a
# volume is FAT12, up to 65524 it is FAT16, above that FAT32.  Both the tool
# and the reader work this out the same way, so the geometry has to land
# squarely inside the intended band rather than near its edge.
MIN_FAT16_CLUSTERS = 4085
MAX_FAT16_CLUSTERS = 65524
MIN_FAT32_CLUSTERS = 65525
MAX_FAT32_CLUSTERS = 0x0FFFFFF4

# The largest FAT16 image worth building.  Above this the clusters would have
# to be 32 KB or more to stay under 65525 of them, and every small file would
# waste most of one.
MAX_FAT16_MB = 512


class FatImage:
    """A FAT16 or FAT32 volume, built whole in memory.

    FAT32 exists here for one reason: a game on a card is more than 512 MiB,
    and FAT16 cannot address that without absurd cluster sizes.  The two differ
    in three places and are otherwise the same file system - the FAT is 32 bits
    wide instead of 16, the root directory is an ordinary cluster chain instead
    of a fixed area behind the FATs, and there is an FSInfo sector and a backup
    boot sector that a reader may consult and must therefore be right.
    """

    def __init__(self, size_mb: int, fat_bits: int = 0):
        if size_mb < 4:
            raise SystemExit("mkfatimg: size must be at least 4 MiB")
        if fat_bits == 0:
            fat_bits = 16 if size_mb <= MAX_FAT16_MB else 32
        if fat_bits not in (16, 32):
            raise SystemExit("mkfatimg: --fat must be 16 or 32")
        if fat_bits == 16 and size_mb > MAX_FAT16_MB:
            raise SystemExit(
                f"mkfatimg: {size_mb} MiB needs FAT32 (FAT16 tops out at "
                f"{MAX_FAT16_MB} MiB); drop --fat 16"
            )

        self.fat_bits = fat_bits
        self.size = size_mb * 1024 * 1024
        self.num_fats = 2
        self.root_cluster = 0

        if fat_bits == 16:
            self.reserved = 1
            self.root_ents = 512
            self.root_secs = (self.root_ents * 32 + SECTOR - 1) // SECTOR
            lo, hi, per_sector = MIN_FAT16_CLUSTERS, MAX_FAT16_CLUSTERS, 256
        else:
            # 32 reserved sectors is what everything else writes, and it is
            # what leaves room for the FSInfo sector at 1 and the backup boot
            # sector at 6.  The root has no fixed area at all.
            self.reserved = 32
            self.root_ents = 0
            self.root_secs = 0
            lo, hi, per_sector = MIN_FAT32_CLUSTERS, MAX_FAT32_CLUSTERS, 128

        # Which cluster sizes to try, in the order worth trying them.
        #
        # FAT16 wants the smallest: the count has to reach 4085, and on a small
        # image only a small cluster gets there.
        #
        # FAT32 wants 4 KB first, and smaller only if the volume is too small
        # to reach 65525 clusters that way.  The lower bound is easy to clear
        # here, so taking the smallest would give half-kilobyte clusters and a
        # FAT of eight megabytes per copy on a gigabyte card - all of which has
        # to be read to follow one chain.
        if fat_bits == 16:
            order = (1, 2, 4, 8, 16, 32, 64)
        else:
            order = (8, 4, 2, 1, 16, 32, 64, 128)

        for spc in order:
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
                need = (self.clusters + 2 + per_sector - 1) // per_sector
                if need <= self.fat_secs:
                    break
                self.fat_secs = need
            if lo <= self.clusters <= hi:
                break
        else:
            raise SystemExit(
                f"mkfatimg: cannot build FAT{fat_bits} for {size_mb} MiB "
                f"(got {self.clusters} clusters); try a different size"
            )

        self.eoc = 0xFFFF if fat_bits == 16 else 0x0FFFFFFF
        self.eoc_min = 0xFFF8 if fat_bits == 16 else 0x0FFFFFF8

        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0xFFF8 if fat_bits == 16 else 0x0FFFFFF8
        self.fat[1] = self.eoc
        self.next_cluster = 2
        self.root_area = b""
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
        # The 16-bit FAT size field must be zero on FAT32; the real one is the
        # 32-bit field at 36.  A reader that finds both non-zero believes the
        # wrong one.
        struct.pack_into("<H", b, 22, 0 if self.fat_bits == 32 else self.fat_secs)
        struct.pack_into("<H", b, 24, 32)
        struct.pack_into("<H", b, 26, 16)

        if self.fat_bits == 16:
            b[38] = 0x29
            struct.pack_into("<I", b, 39, 0x41524F4E)
            b[43:54] = b"ARGONSHARE "
            b[54:62] = b"FAT16   "
        else:
            struct.pack_into("<I", b, 36, self.fat_secs)
            struct.pack_into("<H", b, 40, 0)  # both FATs live, mirrored
            struct.pack_into("<H", b, 42, 0)  # version 0.0
            struct.pack_into("<I", b, 44, self.root_cluster or 2)
            struct.pack_into("<H", b, 48, 1)  # FSInfo sector
            struct.pack_into("<H", b, 50, 6)  # backup boot sector
            b[66] = 0x29
            struct.pack_into("<I", b, 67, 0x41524F4E)
            b[71:82] = b"ARGONSHARE "
            b[82:90] = b"FAT32   "

        b[510] = 0x55
        b[511] = 0xAA
        self.image[0:SECTOR] = b

        if self.fat_bits == 32:
            # FSInfo.  The free count is advisory and may be "unknown", but a
            # wrong one is worse than none: a driver that trusts it can hand
            # out a cluster that is already in use.
            fs = bytearray(SECTOR)
            struct.pack_into("<I", fs, 0, 0x41615252)
            struct.pack_into("<I", fs, 484, 0x61417272)
            struct.pack_into("<I", fs, 488, 0xFFFFFFFF)
            struct.pack_into("<I", fs, 492, 0xFFFFFFFF)
            struct.pack_into("<I", fs, 508, 0xAA550000)
            self.image[SECTOR : 2 * SECTOR] = fs
            # The backup, which is what a reader falls back to when sector 0
            # will not parse.
            self.image[6 * SECTOR : 7 * SECTOR] = b

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
            self.fat[c] = self.eoc if i == need - 1 else c + 1
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
            if nxt >= self.eoc_min:
                break
            c = nxt

    def place_root(self, root_bytes: bytes) -> None:
        """Put the root directory where this format keeps it.

        On FAT16 that is the fixed area behind the FATs, sized at format time
        and unable to grow - which is why a card with too many files in its
        top level is refused rather than truncated.

        On FAT32 it is an ordinary chain, allocated here like any other
        directory, and the boot sector is rewritten to say where.  It does not
        have to be cluster 2: BPB_RootClus exists precisely so it can be
        anywhere, which lets it be allocated last, once its size is known.
        """
        if self.fat_bits == 16:
            if len(root_bytes) > self.root_secs * SECTOR:
                raise SystemExit(
                    f"mkfatimg: root directory full ({len(root_bytes)} > "
                    f"{self.root_secs * SECTOR} bytes); put files in a "
                    "subdirectory, or use FAT32"
                )
            self.root_area = bytes(root_bytes)
            return

        self.root_cluster = self.alloc_clusters(len(root_bytes))
        self.write_file_data(self.root_cluster, root_bytes)
        self._write_boot()

    def flush_fat(self) -> None:
        fat_bytes = bytearray(self.fat_secs * SECTOR)
        if self.fat_bits == 16:
            for i, v in enumerate(self.fat):
                struct.pack_into("<H", fat_bytes, i * 2, v & 0xFFFF)
        else:
            # The top four bits of a FAT32 entry belong to whoever formatted
            # the volume and must be preserved by anyone editing it; written
            # from scratch they are zero.
            for i, v in enumerate(self.fat):
                struct.pack_into("<I", fat_bytes, i * 4, v & 0x0FFFFFFF)
        fat_off = self.reserved * SECTOR
        for n in range(self.num_fats):
            o = fat_off + n * len(fat_bytes)
            self.image[o : o + len(fat_bytes)] = fat_bytes

        if self.fat_bits == 16:
            root_off = fat_off + self.num_fats * len(fat_bytes)
            root_area = bytearray(self.root_secs * SECTOR)
            root_area[: len(self.root_area)] = self.root_area
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
            node.children.append(
                Node(
                    name=name,
                    is_dir=False,
                    mtime=datetime.fromtimestamp(stf.st_mtime),
                    path=full,
                    size=stf.st_size,
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
            child.cluster = img.alloc_clusters(child.size)
            img.write_file_data(child.cluster, child.read())
        for e in lfn:
            records.append(e)
        records.append(
            _dir_entry(
                name83,
                0x20,
                child.cluster if allocate else 0,
                child.size,
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


def pack(host_dir: str, out_path: str, size_mb: int, fat_bits: int = 0) -> None:
    host_dir = os.path.abspath(host_dir)
    if not os.path.isdir(host_dir):
        raise SystemExit(f"mkfatimg: not a directory: {host_dir}")

    root = scan_dir(host_dir)
    img = FatImage(size_mb, fat_bits)
    used: Set[bytes] = set()
    root_records = bytearray()
    items = []
    for child in root.children:
        name83 = _unique_83(child.name, used)
        items.append((child, name83, _lfn_entries(child.name, name83)))

    for child, name83, lfn in items:
        if child.is_dir:
            continue
        child.cluster = img.alloc_clusters(child.size)
        img.write_file_data(child.cluster, child.read())
        for e in lfn:
            root_records += e
        root_records += _dir_entry(name83, 0x20, child.cluster, child.size, child.mtime)

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

    img.place_root(bytes(root_records))
    img.flush_fat()
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
    clus = img.sec_per_clus * SECTOR
    clus_txt = f"{clus // 1024} KB" if clus >= 1024 else f"{clus} B"
    used = (img.next_cluster - 2) * clus
    print(
        f"mkfatimg: wrote {out_path} ({size_mb} MiB FAT{img.fat_bits}, "
        f"{clus_txt} clusters, {used / (1024 * 1024):.1f} MiB used, "
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
    ap.add_argument(
        "--fat",
        type=int,
        choices=(16, 32),
        default=0,
        help="FAT width; by default 16 up to %d MiB and 32 above it. FAT32 "
             "also lifts the 512-entry limit on the root directory"
             % MAX_FAT16_MB,
    )
    args = ap.parse_args()
    pack(args.directory, args.output, args.size_mb, args.fat)


if __name__ == "__main__":
    main()
