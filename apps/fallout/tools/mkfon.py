#!/usr/bin/env python3
"""Write Fallout .FON files from the ArgonOS console font.

This exists to get the engine past one door.  win_init loads the text fonts
before it opens the window, so without a font there is no video path to test at
all - and the game's own fonts live inside MASTER.DAT, which is not ours to
have.  The OS has a perfectly good 8x16 font of its own, and the .FON format is
five integers and a bitmap.

The result is not Fallout's font and does not pretend to be: it is the console
font, monospaced, in a game that expects proportional letters.  Menus will be
wider than they were drawn to be.  What it buys is the ability to reach
svga_init, and everything after it, before the real data exists.

    python apps/fallout/tools/mkfon.py -o build/sd_card/data

The format, from plib/gnw/text.cc:

    int32  num          glyphs
    int32  height       pixels
    int32  spacing      pixels between glyphs
    int32  glyphs_ptr   a pointer in the 1997 file; unused
    int32  data_ptr     the same
    { int32 width; int32 offset; } * num
    uint8  data[]       rows of ((width + 7) / 8) bytes, MSB first

Bit order is the one difference that matters: the kernel's font has the
leftmost pixel in bit 0, and Fallout reads it from bit 7.  Every row byte is
reversed on the way out.  Getting that wrong does not fail - it draws every
letter mirrored, which is a thing you have to see to believe.

Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
"""
from __future__ import annotations

import argparse
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
SRC = ROOT / "components" / "argon_kernel" / "src" / "dev" / "font8x16.c"

GLYPHS = 256
HEIGHT = 16
WIDTH = 8
SPACING = 1

# The engine's font manager claims numbers 0 through 8, and the interface asks
# for several of them by number.  They are all this one font: a missing font is
# an error the game reports, a plain-looking font is not.
FONT_NUMBERS = range(9)


def load_kernel_font() -> list[list[int]]:
    text = SRC.read_text(encoding="utf-8")
    rows: list[list[int]] = []
    for m in re.finditer(
            r"\{\s*((?:0x[0-9A-Fa-f]{2}\s*,\s*){15}0x[0-9A-Fa-f]{2})\s*\}", text):
        vals = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", m.group(1))]
        if len(vals) == HEIGHT:
            rows.append(vals)
    if len(rows) != GLYPHS:
        raise SystemExit("expected %d glyphs in %s, found %d"
                         % (GLYPHS, SRC, len(rows)))
    return rows


def flip(byte: int) -> int:
    """Leftmost pixel from bit 0 to bit 7."""
    out = 0
    for i in range(8):
        if byte & (1 << i):
            out |= 0x80 >> i
    return out


def build(glyphs: list[list[int]]) -> bytes:
    stride = (WIDTH + 7) // 8

    header = struct.pack("<5i", GLYPHS, HEIGHT, SPACING, 0, 0)

    info = b""
    data = b""
    for g in glyphs:
        info += struct.pack("<2i", WIDTH, len(data))
        for row in g:
            data += bytes([flip(row)])

    # What the loader will compute and read; if this does not match what was
    # written, it reads past the end of the file and gives up.
    expected = HEIGHT * stride + (GLYPHS - 1) * HEIGHT * stride
    if len(data) != expected:
        raise SystemExit("data is %d bytes, loader expects %d"
                         % (len(data), expected))

    return header + info + data


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--outdir", default=str(ROOT / "build" / "sd_card" / "data"),
                    help="where the .FON files go; the game's patches directory")
    args = ap.parse_args()

    blob = build(load_kernel_font())

    out = pathlib.Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)
    for n in FONT_NUMBERS:
        (out / ("font%d.fon" % n)).write_bytes(blob)

    print("%s: font0..font%d.fon, %d glyphs %dx%d, %d bytes each"
          % (out, FONT_NUMBERS[-1], GLYPHS, WIDTH, HEIGHT, len(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
