#!/usr/bin/env python3
"""Build Harbor Quest procedural RGB565 LE atlas (16x16 tiles, magenta key).

Original procedural look (not Cute Fantasy / Pixel Crawler packs).

  python tools/rpg_atlas.py -o apps/cc/games/harbor/assets/atlas.bin --png preview.png
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

TW = 16
TH = 16
ATLAS_W = 16
ATLAS_H = 8
KEY = (255, 0, 255)


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def put(tile, x, y, c):
    if 0 <= x < TW and 0 <= y < TH:
        tile[y][x] = c


def fill(tile, c):
    for y in range(TH):
        for x in range(TW):
            tile[y][x] = c


def rect(tile, x0, y0, x1, y1, c):
    for y in range(y0, y1):
        for x in range(x0, x1):
            put(tile, x, y, c)


def new_tile(c=KEY):
    return [[c for _ in range(TW)] for _ in range(TH)]


def noise_fill(tile, base, a, b, seed):
    fill(tile, base)
    for y in range(TH):
        for x in range(TW):
            n = (x * 3 + y * 7 + seed * 11) & 7
            if n < 2:
                put(tile, x, y, a)
            elif n == 5:
                put(tile, x, y, b)


def make_person(tile, skin, shirt, pants, hair, frame, facing):
    fill(tile, KEY)
    bob = 1 if frame else 0
    if facing == 2:
        rect(tile, 5, 11 + bob, 8, 15, pants)
        rect(tile, 8, 11 + (1 - bob), 11, 15, pants)
    elif facing == 3:
        rect(tile, 5, 11 + (1 - bob), 8, 15, pants)
        rect(tile, 8, 11 + bob, 11, 15, pants)
    else:
        rect(tile, 5, 11 + bob, 8, 15, pants)
        rect(tile, 8, 11 + (1 - bob), 11, 15, pants)
    rect(tile, 4, 6, 12, 12, shirt)
    rect(tile, 5, 2, 11, 7, skin)
    rect(tile, 5, 1, 11, 3, hair)
    if facing == 0:
        put(tile, 6, 4, (20, 20, 40))
        put(tile, 9, 4, (20, 20, 40))
    elif facing == 1:
        rect(tile, 5, 1, 11, 4, hair)
    elif facing == 2:
        put(tile, 6, 4, (20, 20, 40))
    else:
        put(tile, 9, 4, (20, 20, 40))


def make_slime(tile, frame):
    fill(tile, KEY)
    y0 = 4 + (frame & 1)
    rect(tile, 3, y0, 13, 14, (60, 180, 80))
    rect(tile, 4, y0 - 1, 12, y0 + 1, (80, 220, 100))
    put(tile, 6, y0 + 3, (20, 40, 20))
    put(tile, 9, y0 + 3, (20, 40, 20))


def make_bandit(tile, frame):
    make_person(tile, (220, 180, 140), (90, 40, 40), (40, 40, 50), (30, 20, 10), frame, 0)
    rect(tile, 4, 5, 12, 7, (20, 20, 20))


def make_boss(tile, frame):
    fill(tile, KEY)
    bob = frame & 1
    rect(tile, 2, 3 + bob, 14, 14, (120, 40, 40))
    rect(tile, 4, 1 + bob, 12, 5 + bob, (160, 60, 60))
    put(tile, 6, 4 + bob, (255, 220, 40))
    put(tile, 9, 4 + bob, (255, 220, 40))
    rect(tile, 1, 8 + bob, 3, 12 + bob, (90, 30, 30))
    rect(tile, 13, 8 + bob, 15, 12 + bob, (90, 30, 30))


def build_tiles():
    tiles = [new_tile() for _ in range(ATLAS_W * ATLAS_H)]
    noise_fill(tiles[0], (40, 120, 40), (50, 140, 50), (30, 100, 30), 1)
    noise_fill(tiles[1], (45, 130, 45), (60, 150, 55), (35, 110, 35), 2)
    noise_fill(tiles[2], (120, 90, 50), (140, 105, 60), (100, 75, 40), 3)
    noise_fill(tiles[3], (200, 180, 100), (210, 190, 120), (180, 160, 80), 4)
    noise_fill(tiles[4], (30, 70, 180), (40, 90, 200), (20, 50, 140), 5)
    for x in range(TW):
        put(tiles[4], x, 4 + (x % 3), (60, 120, 220))
    noise_fill(tiles[5], (140, 120, 80), (150, 130, 90), (120, 100, 70), 6)
    fill(tiles[6], (90, 90, 100))
    rect(tiles[6], 0, 0, 16, 2, (60, 60, 70))
    rect(tiles[6], 0, 14, 16, 16, (60, 60, 70))
    fill(tiles[7], (160, 140, 110))
    for y in range(0, TH, 4):
        for x in range(TW):
            put(tiles[7], x, y, (140, 120, 90))
    fill(tiles[8], (100, 70, 40))
    rect(tiles[8], 3, 2, 13, 15, (60, 40, 20))
    put(tiles[8], 11, 8, (200, 180, 40))
    fill(tiles[9], KEY)
    rect(tiles[9], 7, 10, 9, 15, (100, 70, 30))
    rect(tiles[9], 3, 2, 13, 11, (30, 110, 40))
    fill(tiles[10], KEY)
    rect(tiles[10], 3, 6, 13, 14, (120, 120, 130))
    rect(tiles[10], 5, 4, 11, 7, (140, 140, 150))
    fill(tiles[11], KEY)
    rect(tiles[11], 2, 8, 14, 14, (40, 130, 50))
    rect(tiles[11], 4, 6, 12, 9, (50, 150, 60))
    fill(tiles[12], (160, 40, 40))
    rect(tiles[12], 2, 2, 14, 14, (180, 50, 50))
    fill(tiles[13], (140, 100, 60))
    rect(tiles[13], 0, 0, 16, 6, (160, 120, 70))
    noise_fill(tiles[14], (70, 60, 80), (90, 80, 100), (50, 40, 60), 9)
    noise_fill(tiles[15], (50, 45, 55), (60, 55, 65), (40, 35, 45), 10)
    fill(tiles[16], (120, 50, 50))
    rect(tiles[16], 0, 8, 16, 16, (140, 60, 60))
    fill(tiles[17], (160, 140, 110))
    rect(tiles[17], 3, 3, 13, 13, (80, 140, 200))
    fill(tiles[18], KEY)
    rect(tiles[18], 1, 6, 15, 14, (80, 80, 140))
    rect(tiles[18], 1, 4, 15, 8, (200, 200, 220))
    fill(tiles[19], KEY)
    rect(tiles[19], 3, 7, 13, 14, (180, 140, 40))
    rect(tiles[19], 3, 5, 13, 8, (200, 160, 50))
    put(tiles[19], 8, 8, (255, 220, 80))
    fill(tiles[20], KEY)
    rect(tiles[20], 7, 8, 9, 15, (120, 80, 40))
    rect(tiles[20], 3, 3, 13, 9, (180, 150, 80))
    fill(tiles[21], (90, 80, 70))
    for i in range(4):
        rect(tiles[21], 2 + i, 12 - i * 3, 14, 14 - i * 3, (130, 120, 100))
    fill(tiles[22], (100, 80, 50))
    for x in range(0, TW, 2):
        rect(tiles[22], x, 4, x + 1, 12, (120, 90, 50))
    fill(tiles[23], KEY)
    rect(tiles[23], 0, 8, 16, 10, (140, 100, 50))
    for x in range(2, TW, 4):
        rect(tiles[23], x, 6, x + 1, 14, (140, 100, 50))
    fill(tiles[24], (50, 100, 160))
    fill(tiles[25], (180, 160, 60))
    fill(tiles[26], (200, 200, 210))
    for y in range(0, TH, 4):
        for x in range(TW):
            put(tiles[26], x, y, (160, 160, 170))
    fill(tiles[27], (0, 0, 0))
    fill(tiles[28], (220, 200, 80))
    put(tiles[28], 8, 8, (200, 40, 40))
    fill(tiles[29], KEY)
    rect(tiles[29], 6, 4, 10, 15, (100, 70, 40))
    rect(tiles[29], 2, 4, 14, 8, (200, 200, 220))
    noise_fill(tiles[30], (80, 70, 60), (90, 80, 70), (70, 60, 50), 11)
    fill(tiles[31], (255, 240, 100))

    skin, shirt, pants, hair = (230, 190, 150), (40, 80, 160), (40, 40, 70), (40, 30, 20)
    for d in range(4):
        for f in range(2):
            make_person(tiles[32 + d * 2 + f], skin, shirt, pants, hair, f, d)
    make_person(tiles[40], (240, 200, 170), (200, 80, 120), (80, 40, 60), (180, 120, 40), 0, 0)
    make_person(tiles[41], (240, 200, 170), (200, 80, 120), (80, 40, 60), (180, 120, 40), 1, 0)
    make_person(tiles[42], (200, 160, 120), (80, 80, 80), (50, 50, 60), (20, 20, 20), 0, 0)
    make_person(tiles[43], (200, 160, 120), (80, 80, 80), (50, 50, 60), (20, 20, 20), 1, 0)
    make_person(tiles[44], (220, 180, 140), (160, 120, 40), (60, 40, 20), (80, 50, 20), 0, 0)
    make_person(tiles[45], (220, 180, 140), (60, 120, 60), (50, 50, 50), (90, 60, 30), 0, 0)
    make_person(tiles[46], (220, 200, 180), (120, 120, 140), (70, 70, 80), (200, 200, 200), 0, 0)
    make_person(tiles[47], (220, 180, 140), (140, 60, 40), (50, 40, 40), (60, 40, 20), 0, 0)
    make_slime(tiles[48], 0)
    make_slime(tiles[49], 1)
    make_bandit(tiles[50], 0)
    make_bandit(tiles[51], 1)
    fill(tiles[52], KEY)
    rect(tiles[52], 2, 6, 7, 10, (80, 60, 100))
    rect(tiles[52], 9, 6, 14, 10, (80, 60, 100))
    rect(tiles[52], 6, 7, 10, 12, (60, 40, 80))
    fill(tiles[53], KEY)
    rect(tiles[53], 1, 5, 6, 9, (80, 60, 100))
    rect(tiles[53], 10, 5, 15, 9, (80, 60, 100))
    rect(tiles[53], 6, 7, 10, 12, (60, 40, 80))
    make_boss(tiles[54], 0)
    make_boss(tiles[55], 1)
    fill(tiles[56], (20, 20, 40))
    fill(tiles[57], (40, 40, 80))
    fill(tiles[58], KEY)
    rect(tiles[58], 2, 2, 14, 14, (255, 220, 40))
    fill(tiles[59], KEY)
    rect(tiles[59], 4, 2, 12, 14, (180, 180, 200))
    put(tiles[59], 7, 4, (200, 40, 40))
    fill(tiles[60], KEY)
    rect(tiles[60], 3, 3, 13, 13, (160, 160, 180))
    rect(tiles[60], 7, 1, 9, 15, (200, 200, 220))
    fill(tiles[61], (0, 0, 0))
    fill(tiles[62], (255, 255, 255))
    fill(tiles[63], KEY)
    for i in range(64, ATLAS_W * ATLAS_H):
        fill(tiles[i], KEY)
    return tiles


def tiles_to_bin(tiles) -> bytes:
    out = bytearray()
    for row in range(ATLAS_H):
        for ty in range(TH):
            for col in range(ATLAS_W):
                tile = tiles[row * ATLAS_W + col]
                for tx in range(TW):
                    r, g, b = tile[ty][tx]
                    out += struct.pack("<H", rgb565(r, g, b))
    return bytes(out)


def tiles_to_png(tiles, path: Path) -> None:
    from PIL import Image

    im = Image.new("RGB", (ATLAS_W * TW, ATLAS_H * TH))
    for i, tile in enumerate(tiles):
        col, row = i % ATLAS_W, i // ATLAS_W
        for y in range(TH):
            for x in range(TW):
                im.putpixel((col * TW + x, row * TH + y), tile[y][x])
    im.save(path)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", required=True, type=Path)
    ap.add_argument("--png", type=Path)
    args = ap.parse_args()
    tiles = build_tiles()
    blob = tiles_to_bin(tiles)
    args.o.parent.mkdir(parents=True, exist_ok=True)
    args.o.write_bytes(blob)
    print(f"wrote {args.o} ({len(blob)} bytes)")
    if args.png:
        tiles_to_png(tiles, args.png)
        print(f"wrote {args.png}")
    stage = Path("build/sd_card/atlas.bin")
    if stage.parent.is_dir():
        stage.write_bytes(blob)
        print(f"staged {stage}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
