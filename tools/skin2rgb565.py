#!/usr/bin/env python3
"""Generate Winamp-inspired AMP skin PNGs + raw RGB565 for HostFS override.

Usage:
  python tools/skin2rgb565.py
  python tools/skin2rgb565.py --out build/sd_card/amp/skin

Writes:
  <out>/vga/{main,eq,pl}.png + .rgb565
  <out>/qvga/{main,eq,pl}.png + .rgb565
  Also copies PNG sources into apps/amp/skins/{vga,qvga}/
"""
from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def fill(buf: list[int], w: int, h: int, c: int) -> None:
    buf[:] = [c] * (w * h)


def rect(buf: list[int], w: int, h: int, x: int, y: int, rw: int, rh: int, c: int) -> None:
    for yy in range(y, y + rh):
        if yy < 0 or yy >= h:
            continue
        for xx in range(x, x + rw):
            if 0 <= xx < w:
                buf[yy * w + xx] = c


def bevel(buf: list[int], w: int, h: int, x: int, y: int, rw: int, rh: int, hi: int, lo: int) -> None:
    rect(buf, w, h, x, y, rw, 1, hi)
    rect(buf, w, h, x, y, 1, rh, hi)
    rect(buf, w, h, x, y + rh - 1, rw, 1, lo)
    rect(buf, w, h, x + rw - 1, y, 1, rh, lo)


def button(buf: list[int], w: int, h: int, x: int, y: int, bw: int, bh: int) -> None:
    face = rgb565(0xB8, 0xB8, 0xC0)
    hi = rgb565(0xE8, 0xE8, 0xF0)
    lo = rgb565(0x58, 0x58, 0x68)
    rect(buf, w, h, x, y, bw, bh, face)
    bevel(buf, w, h, x, y, bw, bh, hi, lo)


def sx(v: int, dw: int, base: int = 275) -> int:
    return v * dw // base


def sy(v: int, dh: int, base: int = 116) -> int:
    return v * dh // base


def paint_main(w: int, h: int) -> list[int]:
    buf: list[int] = []
    bg = rgb565(0x28, 0x34, 0x48)
    title = rgb565(0x18, 0x20, 0x30)
    lcd = rgb565(0x00, 0x20, 0x18)
    accent = rgb565(0x00, 0xE0, 0x68)
    fill(buf, w, h, bg)
    rect(buf, w, h, 0, 0, w, sy(14, h), title)
    bevel(buf, w, h, 0, 0, w, h, rgb565(0x60, 0x70, 0x88), rgb565(0x10, 0x14, 0x20))
    rect(buf, w, h, sx(10, w), sy(18, h), sx(76, w), sy(44, h), lcd)
    bevel(buf, w, h, sx(10, w), sy(18, h), sx(76, w), sy(44, h), rgb565(0x00, 0x40, 0x30), accent)
    rect(buf, w, h, sx(110, w), sy(20, h), sx(150, w), sy(14, h), lcd)
    rect(buf, w, h, sx(16, w), sy(70, h), sx(240, w), sy(8, h), rgb565(0x10, 0x14, 0x20))
    rect(buf, w, h, sx(110, w), sy(40, h), sx(60, w), sy(8, h), rgb565(0x10, 0x14, 0x20))
    rect(buf, w, h, sx(180, w), sy(40, h), sx(40, w), sy(8, h), rgb565(0x10, 0x14, 0x20))
    for i in range(5):
        button(buf, w, h, sx(16 + i * 28, w), sy(88, h), sx(24, w), sy(18, h))
    button(buf, w, h, sx(160, w), sy(88, h), sx(24, w), sy(18, h))
    for x, y in ((200, 88), (226, 88), (200, 102), (226, 102)):
        button(buf, w, h, sx(x, w), sy(y, h), sx(22, w), sy(12, h))
    return buf


def paint_eq(w: int, h: int) -> list[int]:
    buf: list[int] = []
    bg = rgb565(0x28, 0x34, 0x48)
    title = rgb565(0x18, 0x20, 0x30)
    slot = rgb565(0x10, 0x14, 0x20)
    fill(buf, w, h, bg)
    rect(buf, w, h, 0, 0, w, sy(14, h), title)
    bevel(buf, w, h, 0, 0, w, h, rgb565(0x60, 0x70, 0x88), rgb565(0x10, 0x14, 0x20))
    button(buf, w, h, sx(12, w), sy(20, h), sx(28, w), sy(14, h))
    button(buf, w, h, sx(44, w), sy(20, h), sx(28, w), sy(14, h))
    for i in range(11):
        x = sx(14 + i * 22, w)
        rect(buf, w, h, x, sy(40, h), sx(10, w), sy(60, h), slot)
        bevel(buf, w, h, x, sy(40, h), sx(10, w), sy(60, h), rgb565(0x40, 0x48, 0x58), rgb565(0x08, 0x0C, 0x10))
    return buf


def paint_pl(w: int, h: int) -> list[int]:
    buf: list[int] = []
    bg = rgb565(0x20, 0x28, 0x38)
    title = rgb565(0x18, 0x20, 0x30)
    lst = rgb565(0x00, 0x18, 0x10)
    fill(buf, w, h, bg)
    rect(buf, w, h, 0, 0, w, 14, title)
    bevel(buf, w, h, 0, 0, w, h, rgb565(0x60, 0x70, 0x88), rgb565(0x10, 0x14, 0x20))
    rect(buf, w, h, 6, 18, w - 12, h - 26, lst)
    return buf


def write_rgb565(path: Path, buf: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"".join(struct.pack("<H", c) for c in buf))


def write_png(path: Path, buf: list[int], w: int, h: int) -> None:
    """Minimal RGB PNG writer (no PIL dependency)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            c = buf[y * w + x]
            r = ((c >> 11) & 0x1F) << 3
            g = ((c >> 5) & 0x3F) << 2
            b = (c & 0x1F) << 3
            raw.extend((r, g, b))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def emit(profile: str, sizes: dict[str, tuple[int, int]], out: Path, src: Path) -> None:
    painters = {"main": paint_main, "eq": paint_eq, "pl": paint_pl}
    for name, (w, h) in sizes.items():
        buf = painters[name](w, h)
        write_rgb565(out / profile / f"{name}.rgb565", buf)
        write_png(out / profile / f"{name}.png", buf, w, h)
        write_png(src / profile / f"{name}.png", buf, w, h)
        # region table stub
        (src / profile / "regions.txt").write_text(
            f"# AMP skin {profile} logical classic 275x116 mapped to panel sizes\n"
            f"main={sizes['main'][0]}x{sizes['main'][1]}\n"
            f"eq={sizes['eq'][0]}x{sizes['eq'][1]}\n"
            f"pl={sizes['pl'][0]}x{sizes['pl'][1]}\n",
            encoding="utf-8",
        )


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "sd_card" / "amp" / "skin")
    args = ap.parse_args()
    src = ROOT / "apps" / "amp" / "skins"
    emit(
        "vga",
        {"main": (550, 148), "eq": (550, 100), "pl": (550, 120)},
        args.out,
        src,
    )
    emit(
        "qvga",
        {"main": (275, 116), "eq": (275, 116), "pl": (312, 232)},
        args.out,
        src,
    )
    print(f"skins written to {args.out} and {src}")


if __name__ == "__main__":
    main()
