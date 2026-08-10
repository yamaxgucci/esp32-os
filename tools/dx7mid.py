#!/usr/bin/env python3
"""Write a tiny looping demo Standard MIDI File for dx7.axe."""
from __future__ import annotations

import argparse
import struct
from pathlib import Path


def vlq(value: int) -> bytes:
    out = [value & 0x7F]
    value >>= 7
    while value:
        out.append(0x80 | (value & 0x7F))
        value >>= 7
    return bytes(reversed(out))


def track(events: list[tuple[int, bytes]]) -> bytes:
    body = bytearray()
    for delta, payload in events:
        body += vlq(delta)
        body += payload
    body += vlq(0) + bytes([0xFF, 0x2F, 0x00])
    return b"MTrk" + struct.pack(">I", len(body)) + bytes(body)


def make_demo() -> bytes:
    """
    Longer notes so slow-attack DX7 patches still speak.
    120 BPM, 480 ppqn: quarter = 480 ticks (~500 ms).
    """
    ppqn = 480
    q = ppqn
    meta_tempo = bytes([0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20])
    ev: list[tuple[int, bytes]] = [(0, meta_tempo)]

    # C4 E4 G4 C5 — each a quarter note
    for i, n in enumerate((60, 64, 67, 72)):
        ev.append((0 if i == 0 else q, bytes([0x90, n, 100])))
        ev.append((q, bytes([0x80, n, 0])))

    # Held C-major chord, 2 beats
    for i, n in enumerate((60, 64, 67)):
        ev.append((0 if i else q // 2, bytes([0x90, n, 100])))
    ev.append((q * 2, bytes([0x80, 60, 0])))
    ev.append((0, bytes([0x80, 64, 0])))
    ev.append((0, bytes([0x80, 67, 0])))

    hdr = b"MThd" + struct.pack(">IHHH", 6, 0, 1, ppqn)
    return hdr + track(ev)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", "--output", type=Path, required=True)
    args = ap.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    data = make_demo()
    args.output.write_bytes(data)
    print(f"wrote {args.output} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
