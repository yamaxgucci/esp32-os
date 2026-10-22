#!/usr/bin/env python3
"""Prove an .AXE's relocation table by linking the same source twice.

    python tools\\check_axe_relocs.py --arch riscv32 --gcc <gcc> \\
        --include apps/hello --include sdk/include apps/hello/hello.c

The question this answers is the only one that matters about a relocation
format: does applying the table move an image correctly?  Not "does it look
right" - does the result equal what the linker itself produces for those
addresses.

So the sources are linked at two different pairs of bases, and the first image
is then relocated onto the second's bases using nothing but its own relocation
table.  The two must come out byte for byte identical.  Anything the table
forgets to mention shows up as a differing byte with an offset, and anything it
mentions wrongly shows up the same way.

This is the check that was missing when RISC-V images could not be split at
all: the loader had a relocation kind it had never applied to a real image, and
the only machine that could have told anybody was a board.  It takes a second
here.

Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import mkaxe  # noqa: E402  - the encoder under test, not a copy of it

HEADER_FIELDS = (
    "magic abi_major abi_minor arch header_size flags "
    "code_base code_size code_file_size code_offset "
    "data_base data_size data_file_size data_offset "
    "entry api_slot reloc_offset reloc_count stack_size heap_size "
    "name version author r0 r1 r2 r3 r4 r5"
).split()


def read_axe(path):
    with open(path, "rb") as f:
        blob = f.read()
    values = struct.unpack_from(mkaxe.HEADER_FORMAT_V1, blob, 0)
    h = dict(zip(HEADER_FIELDS, values))
    if h["magic"] != b"AXE1":
        raise SystemExit("%s: not an .AXE" % path)

    # The instruction table only exists in an image that has one, and its two
    # words are only there when the header says so.  Reading them off a shorter
    # header would read the first bytes of the code and believe them, which is
    # the mistake the loader has an accessor to avoid.
    h["ireloc_offset"] = 0
    h["ireloc_count"] = 0
    if h["header_size"] >= struct.calcsize(mkaxe.HEADER_FORMAT):
        h["ireloc_offset"], h["ireloc_count"] = struct.unpack_from(
            "<II", blob, struct.calcsize(mkaxe.HEADER_FORMAT_V1))

    h["code"] = bytearray(blob[h["code_offset"]:
                               h["code_offset"] + h["code_file_size"]])
    h["data"] = bytearray(blob[h["data_offset"]:
                               h["data_offset"] + h["data_file_size"]])
    h["relocs"] = [
        struct.unpack_from("<I", blob, h["reloc_offset"] + i * 4)[0]
        for i in range(h["reloc_count"])
    ]
    h["irelocs"] = [
        struct.unpack_from("<II", blob, h["ireloc_offset"] + i * 8)
        for i in range(h["ireloc_count"])
    ]
    return h


def relocate(img, code_base, data_base):
    """Move `img` onto the given bases, in place, the way the loader does."""
    code_bias = (code_base - img["code_base"]) & 0xFFFFFFFF
    data_bias = (data_base - img["data_base"]) & 0xFFFFFFFF

    for entry in img["relocs"]:
        at = entry & ~3
        part = img["data"] if entry & 1 else img["code"]
        bias = data_bias if entry & 2 else code_bias
        word = struct.unpack_from("<I", part, at)[0]
        struct.pack_into("<I", part, at, (word + bias) & 0xFFFFFFFF)

    for site, target in img["irelocs"]:
        at = site & 0x0FFFFFFF
        kind = site & 0xE0000000
        base = data_base if site & 0x10000000 else code_base
        word = struct.unpack_from("<I", img["code"], at)[0]
        struct.pack_into("<I", img["code"], at,
                         mkaxe.encode_imm(kind, word, base + target))


def build(args, out, code_base, data_base, workdir):
    cmd = [sys.executable, os.path.join(ROOT, "tools", "mkaxe.py"),
           "--arch", args.arch, "--gcc", args.gcc,
           "--code-base", hex(code_base), "--data-base", hex(data_base),
           "--no-stage", "-o", out]
    if args.arch != "xtensa":
        cmd.append("--split")
    for inc in args.include:
        cmd += ["--include", inc]
    for lib in args.libs:
        cmd += ["--libs", lib]
    if args.cflags:
        cmd += ["--cflags", args.cflags]
    cmd += args.sources
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("check_axe_relocs: the build at %#x/%#x failed"
                         % (code_base, data_base))
    return read_axe(out)


def first_difference(a, b, name):
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            return "%s differs at %#x: relocated %#02x, linked %#02x" % (
                name, i, a[i], b[i])
    if len(a) != len(b):
        return "%s is %u bytes relocated and %u bytes linked" % (
            name, len(a), len(b))
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--arch", required=True, choices=("xtensa", "riscv32"))
    ap.add_argument("--gcc", required=True)
    ap.add_argument("--include", action="append", default=[])
    ap.add_argument("--libs", action="append", default=[])
    ap.add_argument("--cflags", default="")
    ap.add_argument("sources", nargs="+")
    args = ap.parse_args()

    # Two pairs far apart, and deliberately in the other order the second time:
    # a bias that happens to be zero, or two parts that keep their distance,
    # would hide exactly the mistakes this is looking for.
    a_bases = (0x42000000, 0x3C000000)
    b_bases = (0x42100000, 0x40810000)

    with tempfile.TemporaryDirectory(prefix="axereloc-") as workdir:
        a = build(args, os.path.join(workdir, "a.axe"), *a_bases,
                  workdir=workdir)
        b = build(args, os.path.join(workdir, "b.axe"), *b_bases,
                  workdir=workdir)

        for field in ("code_size", "data_size", "reloc_count",
                      "ireloc_count"):
            if a[field] != b[field]:
                raise SystemExit(
                    "check_axe_relocs: the two links disagree about %s (%u vs "
                    "%u); they must differ only in where the parts sit"
                    % (field, a[field], b[field]))

        relocate(a, *b_bases)

        problems = [p for p in (first_difference(a["code"], b["code"], "code"),
                                first_difference(a["data"], b["data"], "data"))
                    if p]
        if problems:
            for p in problems:
                sys.stderr.write("check_axe_relocs: " + p + "\n")
            return 1

        print("check_axe_relocs: ok - %u words and %u instructions moved "
              "%s to %#x/%#x byte for byte"
              % (a["reloc_count"], a["ireloc_count"],
                 os.path.basename(args.sources[0]), *b_bases))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
