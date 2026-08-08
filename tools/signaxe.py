#!/usr/bin/env python3
"""Sign or verify an ArgonOS .AXE image (HMAC-SHA256-128 in header reserved).

MAC covers the whole file with reserved[6] treated as zero.  Algorithm and
key id match components/argon_kernel/src/loader/axesig.c (dev key id 0).

  python tools/signaxe.py build/HELLO.AXE
  python tools/signaxe.py --verify build/HELLO.AXE
"""
from __future__ import annotations

import argparse
import hashlib
import hmac
import struct
import sys
from pathlib import Path

HEADER_SIZE = 176
RESERVED_OFF = 152
RESERVED_LEN = 24

ALGO_HMAC_SHA256_128 = 1
KEY_DEV = 0

# Must match s_dev_key in axesig.c (exactly 32 bytes).
DEV_KEY = b"ArgonOS-dev-hmac-key-v1!!!!!!!!!"  # 23 + 9×'!'


def mac_over(data: bytes | bytearray, key: bytes = DEV_KEY) -> bytes:
    cleared = bytearray(data)
    cleared[RESERVED_OFF : RESERVED_OFF + RESERVED_LEN] = b"\0" * RESERVED_LEN
    return hmac.new(key, bytes(cleared), hashlib.sha256).digest()[:16]


def sign(path: Path, key_id: int = KEY_DEV) -> None:
    data = bytearray(path.read_bytes())
    if len(data) < HEADER_SIZE or data[:4] != b"AXE1":
        raise SystemExit(f"signaxe: not an AXE1 image: {path}")
    tag = mac_over(data)
    struct.pack_into("<II16s", data, RESERVED_OFF, ALGO_HMAC_SHA256_128, key_id, tag)
    path.write_bytes(data)
    print(f"signed {path} (algo={ALGO_HMAC_SHA256_128} key_id={key_id})")


def verify(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < HEADER_SIZE or data[:4] != b"AXE1":
        print(f"signaxe: not an AXE1 image: {path}", file=sys.stderr)
        return 1
    algo, key_id, tag = struct.unpack_from("<II16s", data, RESERVED_OFF)
    if algo == 0 and tag == b"\0" * 16 and key_id == 0:
        print(f"{path}: unsigned")
        return 0
    if algo != ALGO_HMAC_SHA256_128:
        print(f"{path}: unsupported algo {algo}", file=sys.stderr)
        return 1
    if key_id != KEY_DEV:
        print(f"{path}: unknown key_id {key_id}", file=sys.stderr)
        return 1
    expect = mac_over(data)
    if tag != expect:
        print(f"{path}: BAD signature", file=sys.stderr)
        return 1
    print(f"{path}: OK (algo={algo} key_id={key_id})")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("axe", type=Path, help=".AXE file to sign or verify")
    p.add_argument("--verify", action="store_true", help="check only")
    p.add_argument("--key-id", type=int, default=KEY_DEV)
    args = p.parse_args()
    if args.verify:
        return verify(args.axe)
    sign(args.axe, key_id=args.key_id)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
