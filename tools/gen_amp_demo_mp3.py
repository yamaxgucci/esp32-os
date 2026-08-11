#!/usr/bin/env python3
"""Write a short silent-ish CBR MP3 stub for AMP HostFS demos."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    # MPEG1 Layer III, 128 kbps, 44100 Hz, mono — padded to frame size.
    hdr = bytes.fromhex(
        "fffb906400000000000000000000000000000000000000000000000000000000"
    )
    frame = hdr + bytes(417 - len(hdr))
    data = frame * 80
    for rel in ("build/sd_card/amp/demo.mp3", "apps/amp/samples/demo.mp3"):
        path = ROOT / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        print(path, path.stat().st_size)


if __name__ == "__main__":
    main()
