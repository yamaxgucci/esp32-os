#!/usr/bin/env python3
"""Pack / unpack DX7 .syx banks (128-byte packed / 155-byte edit buffer)."""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# Dexed INIT VOICE (155 bytes), OP6..OP1 then common + name.
# Audible INIT: algorithm 32 (all carriers), OP1 level 99. (Dexed's alg-1
# INIT is silent here because only the top modulator has level.)
INIT_155 = bytes(
    [
        99, 99, 99, 99, 99, 99, 99, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
        99, 99, 99, 99, 99, 99, 99, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
        99, 99, 99, 99, 99, 99, 99, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
        99, 99, 99, 99, 99, 99, 99, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
        99, 99, 99, 99, 99, 99, 99, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 7,
        99, 99, 99, 99, 99, 99, 99, 0, 0, 0, 0, 0, 0, 0, 0, 0, 99, 0, 1, 0, 7,
        99, 99, 99, 99, 50, 50, 50, 50, 31, 0, 1, 35, 0, 0, 0, 1, 0, 3, 24,
        ord("I"), ord("N"), ord("I"), ord("T"), ord(" "),
        ord("V"), ord("O"), ord("I"), ord("C"), ord("E"),
    ]
)


def pack_155_to_128(src: bytes) -> bytes:
    assert len(src) >= 155
    bulk = bytearray(128)
    for op in range(6):
        pp, up = op * 17, op * 21
        bulk[pp : pp + 11] = src[up : up + 11]
        bulk[pp + 11] = (src[up + 11] & 3) | ((src[up + 12] & 3) << 2)
        bulk[pp + 12] = (src[up + 13] & 7) | ((src[up + 20] & 0x0F) << 3)
        bulk[pp + 13] = (src[up + 14] & 3) | ((src[up + 15] & 7) << 2)
        bulk[pp + 14] = src[up + 16]
        bulk[pp + 15] = (src[up + 17] & 1) | ((src[up + 18] & 31) << 1)
        bulk[pp + 16] = src[up + 19]
    bulk[102:111] = src[126:135]
    bulk[111] = (src[135] & 7) | ((src[136] & 1) << 3)
    bulk[112:116] = src[137:141]
    bulk[116] = (src[141] & 1) | ((src[142] & 7) << 1) | ((src[143] & 7) << 4)
    bulk[117] = src[144]
    bulk[118:128] = src[145:155]
    return bytes(bulk)


def checksum(data: bytes) -> int:
    return (-sum(data)) & 0x7F


def make_bank_syx(voices_128: list[bytes]) -> bytes:
    while len(voices_128) < 32:
        voices_128.append(pack_155_to_128(INIT_155))
    body = b"".join(voices_128[:32])
    assert len(body) == 4096
    hdr = bytes([0xF0, 0x43, 0x00, 0x09, 0x20, 0x00])
    return hdr + body + bytes([checksum(body), 0xF7])


def name_voice(v155: bytearray, name: str) -> None:
    nm = (name.upper() + "          ")[:10]
    v155[145:155] = nm.encode("ascii")


def variant(base: bytes, **kw) -> bytes:
    v = bytearray(base)
    if "name" in kw:
        name_voice(v, kw["name"])
    if "algorithm" in kw:
        v[134] = kw["algorithm"] & 31
    if "feedback" in kw:
        v[135] = kw["feedback"] & 7
    if "op1_level" in kw:
        v[105 + 16] = kw["op1_level"] & 99  # OP1 out level in 155 layout
    if "lfo_pmd" in kw:
        v[139] = kw["lfo_pmd"] & 99
    if "transpose" in kw:
        v[144] = kw["transpose"] & 48
    return bytes(v)


def cmd_gen(path: Path) -> None:
    voices = [
        pack_155_to_128(INIT_155),
        pack_155_to_128(variant(INIT_155, name="BRASSY", algorithm=18, feedback=3, lfo_pmd=15)),
        pack_155_to_128(variant(INIT_155, name="E.PIANO", algorithm=4, feedback=4)),
        pack_155_to_128(variant(INIT_155, name="BELL", algorithm=0, feedback=0)),
        pack_155_to_128(variant(INIT_155, name="PAD", algorithm=31, feedback=1)),
        pack_155_to_128(variant(INIT_155, name="BASSOON", algorithm=15, feedback=5)),
        pack_155_to_128(variant(INIT_155, name="SOFTSIN", algorithm=31, feedback=0, op1_level=80)),
        pack_155_to_128(variant(INIT_155, name="HI BRASS", algorithm=18, feedback=4, transpose=36)),
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(make_bank_syx(voices))
    print(f"wrote {path} ({path.stat().st_size} bytes, 32 voices)")


def cmd_list(path: Path) -> None:
    data = path.read_bytes()
    if data[:1] == b"\xf0" and len(data) >= 4104 and data[3] == 0x09:
        body = data[6 : 6 + 4096]
    elif len(data) >= 4096:
        body = data[:4096]
    else:
        print("not a 32-voice bank", file=sys.stderr)
        sys.exit(1)
    for i in range(32):
        chunk = body[i * 128 : (i + 1) * 128]
        name = bytes(chunk[118:128]).decode("ascii", errors="replace").strip()
        alg = chunk[110] & 31
        print(f"{i:02d}: {name!r:12} alg {alg + 1}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen", help="generate demo 32-voice bank")
    g.add_argument("-o", "--output", type=Path, required=True)
    l = sub.add_parser("list", help="list voices in a bank")
    l.add_argument("syx", type=Path)
    args = ap.parse_args()
    if args.cmd == "gen":
        cmd_gen(args.output)
    else:
        cmd_list(args.syx)


if __name__ == "__main__":
    main()
