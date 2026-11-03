#!/usr/bin/env python3
"""The Mega Drive front end's backdrop: a cyberpunk city at night, 320x240,
in 16 colours a Mega Drive could show (3 bits a channel).

Drawn, not painted: a seeded generator, so the picture is the same every run
and the source is this file.  Writes

    apps/md/md_city.h     the picture as 4-bit indices, run-length packed,
                          and its palette in RGB565 (md_main.c draws it)
    <out>.png             to look at  (--png)

The packing: a byte n < 0x80 is n+1 literal pairs of pixels follow (one byte
each, two indices); n >= 0x80 is the next byte (two pixels) repeated n-0x7f
times.  Rows are packed one after another, 160 bytes of pixels a row.

Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
"""
import argparse
import os
import random
import struct
import zlib

W, H = 320, 240
HORIZON = 168           # the street begins

# 16 colours on the Mega Drive's grid (each channel 0, 36, 72, ... 252)
def md(r, g, b):
    return (r * 36, g * 36, b * 36)

PAL = [
    md(0, 0, 1),   # 0  night
    md(1, 0, 2),   # 1  deep violet
    md(2, 0, 3),   # 2  violet
    md(3, 0, 4),   # 3  purple
    md(5, 1, 4),   # 4  magenta haze
    md(7, 2, 3),   # 5  pink
    md(7, 4, 2),   # 6  sun orange
    md(7, 6, 2),   # 7  sun yellow
    md(0, 0, 0),   # 8  black (towers)
    md(0, 1, 2),   # 9  tower near
    md(1, 1, 3),   # 10 tower far
    md(0, 6, 7),   # 11 cyan neon
    md(7, 0, 5),   # 12 magenta neon
    md(7, 7, 3),   # 13 window yellow
    md(2, 7, 7),   # 14 pale cyan
    md(7, 7, 7),   # 15 white
]

def make():
    rnd = random.Random(1989)
    px = [[0] * W for _ in range(H)]

    # sky: bands from night to haze, dithered at the seams
    bands = [0, 1, 2, 3, 4, 5]
    for y in range(HORIZON):
        t = y / HORIZON
        f = t * (len(bands) - 1)
        i = int(f)
        frac = f - i
        for x in range(W):
            c = bands[i]
            if i + 1 < len(bands) and frac > 0.5 and (x + y) % 2 == 0:
                c = bands[i + 1]
            px[y][x] = c
    # stars in the dark part
    for _ in range(90):
        x, y = rnd.randrange(W), rnd.randrange(60)
        px[y][x] = 15 if rnd.random() < 0.3 else 14

    # the sun, striped at its foot
    cx, cy, r = 160, 112, 46
    for y in range(cy - r, cy + r):
        for x in range(cx - r, cx + r):
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r and 0 <= y < HORIZON:
                if y > cy - 4 and ((y - cy) // 3) % 3 == 2:
                    continue          # the stripes
                px[y][x] = 7 if y < cy - 10 else 6

    def tower(x0, w, top, body, lit, density):
        for y in range(top, HORIZON):
            for x in range(x0, min(W, x0 + w)):
                if 0 <= x:
                    px[y][x] = body
        # roof details: an antenna or a step
        if rnd.random() < 0.5 and w > 8:
            ax = x0 + w // 2
            for y in range(max(0, top - rnd.randrange(6, 18)), top):
                if 0 <= ax < W:
                    px[y][ax] = body
            if 0 <= ax < W and top - 1 >= 0:
                px[max(0, top - 18)][ax] = 12
        # windows on a 3x4 grid
        for y in range(top + 3, HORIZON - 2, 4):
            for x in range(x0 + 2, x0 + w - 2, 3):
                if 0 <= x < W and rnd.random() < density:
                    px[y][x] = lit
                    if y + 1 < HORIZON:
                        px[y + 1][x] = lit

    # far towers, near towers
    x = -4
    while x < W:
        w = rnd.randrange(14, 30)
        top = rnd.randrange(70, 120)
        if 104 < x < 200:
            top = rnd.randrange(128, 148)   # the sun stays in view
        tower(x, w, top, 10, 14 if rnd.random() < 0.5 else 13, 0.25)
        x += w - rnd.randrange(0, 6)
    x = -6
    while x < W:
        w = rnd.randrange(20, 42)
        top = rnd.randrange(96, 140)
        if 96 < x < 200:
            top = rnd.randrange(142, 156)   # keep the sun showing
        tower(x, w, top, 9, 13 if rnd.random() < 0.6 else 11, 0.35)
        x += w + rnd.randrange(0, 8)

    # neon signs on the near towers: vertical bars
    for _ in range(7):
        sx = rnd.randrange(8, W - 8)
        sy = rnd.randrange(110, 150)
        col = 11 if rnd.random() < 0.5 else 12
        for y in range(sy, min(HORIZON - 4, sy + rnd.randrange(14, 26))):
            px[y][sx] = col
            px[y][sx + 1] = col

    # the street: black, with the city's lights mirrored in streaks
    for y in range(HORIZON, H):
        for x in range(W):
            px[y][x] = 8
    for y in range(HORIZON, H):
        d = y - HORIZON
        src = HORIZON - 1 - d * 2
        if src < 90:
            break
        for x in range(W):
            c = px[src][x]
            if c in (11, 12, 13, 14, 6, 7) and (y + x // 2) % 2 == 0:
                px[y][x] = {6: 5, 7: 6, 13: 6, 14: 11}.get(c, c)
            elif c in (4, 5) and d % 3 == 0 and (x % 4) < 3:
                px[y][x] = 3                 # the haze, faint on the wet street
    # a cyan kerb line at the horizon
    for x in range(W):
        px[HORIZON][x] = 11
    return px

def pack(px):
    out = bytearray()
    for row in px:
        pairs = [(row[x] << 4) | row[x + 1] for x in range(0, W, 2)]
        i = 0
        while i < len(pairs):
            run = 1
            while i + run < len(pairs) and run < 128 and pairs[i + run] == pairs[i]:
                run += 1
            if run >= 3:
                out += bytes([0x7f + run, pairs[i]])
                i += run
                continue
            lit = 0
            while i + lit < len(pairs) and lit < 128 and not (
                    i + lit + 2 < len(pairs) and pairs[i + lit] == pairs[i + lit + 1] == pairs[i + lit + 2]):
                lit += 1
            out += bytes([lit - 1]) + bytes(pairs[i:i + lit])
            i += lit
    return out

def rgb565(c):
    r, g, b = c
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def png(px, path):
    raw = b''.join(b'\0' + bytes(v for x in range(W) for v in PAL[row[x]]) for row in px)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--png', help='also write the picture here')
    ap.add_argument('--header', default=os.path.join(os.path.dirname(__file__), '..', 'apps', 'md', 'md_city.h'))
    a = ap.parse_args()
    px = make()
    data = pack(px)
    if a.png:
        png(px, a.png)
    lines = ['/* Generated by tools/gen_md_city.py - do not edit.  320x240, 16 colours,',
             ' * rows of 4-bit pairs run-length packed (see the tool). */',
             '#ifndef MD_CITY_H', '#define MD_CITY_H', '#include <stdint.h>',
             '#define MD_CITY_W %d' % W, '#define MD_CITY_H_ %d' % H,
             'static const uint16_t k_city_pal[16] = {' +
             ', '.join('0x%04x' % rgb565(c) for c in PAL) + '};',
             'static const uint8_t k_city[%d] __attribute__((aligned(4))) = {' % len(data)]
    for i in range(0, len(data), 16):
        lines.append('    ' + ', '.join('0x%02x' % b for b in data[i:i + 16]) + ',')
    lines += ['};', '#endif', '']
    open(a.header, 'w', newline='\n').write('\n'.join(lines))
    print('%d bytes packed of %d' % (len(data), W * H // 2))

if __name__ == '__main__':
    main()
