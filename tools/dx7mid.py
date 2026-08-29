#!/usr/bin/env python3
"""Write a Standard MIDI File for dx7.axe: a short demo, or something to listen to.

    python tools/dx7mid.py -o build/sd_card/demo.mid
    python tools/dx7mid.py --piece prelude -o build/sd_card/prelude.mid

`demo` is four notes and a chord: enough to prove a sound path carries pitch
and polyphony, and short enough to read the whole thing in a transcript.

`prelude` is the first eight bars of Bach's Prelude in C, BWV 846 (1722, and
therefore nobody's property).  It exists because a scale proves the wires and
tells you nothing about the sound: an arpeggio that moves through eight
harmonies makes a bad envelope, a wrong tuning or a starved DMA buffer obvious
to anyone in the room, and none of those are audible in a single tone.
"""
from __future__ import annotations

import argparse
import struct
from pathlib import Path

PPQN = 480


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


def deltas(abs_events: list[tuple[int, bytes]]) -> list[tuple[int, bytes]]:
    """Absolute ticks to deltas.  Note-offs sort before note-ons at the same
    tick, so a repeated pitch is retriggered rather than silenced."""
    abs_events.sort(key=lambda e: (e[0], e[1][0] & 0xF0))
    out: list[tuple[int, bytes]] = []
    last = 0
    for at, payload in abs_events:
        out.append((at - last, payload))
        last = at
    return out


def make_demo() -> bytes:
    """
    Longer notes so slow-attack DX7 patches still speak.
    120 BPM, 480 ppqn: quarter = 480 ticks (~500 ms).
    """
    q = PPQN
    ev: list[tuple[int, bytes]] = [(0, bytes([0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20]))]

    # C4 E4 G4 C5 - each a quarter note
    for i, n in enumerate((60, 64, 67, 72)):
        ev.append((0 if i == 0 else q, bytes([0x90, n, 100])))
        ev.append((q, bytes([0x80, n, 0])))

    # Held C-major chord, 2 beats
    for i, n in enumerate((60, 64, 67)):
        ev.append((0 if i else q // 2, bytes([0x90, n, 100])))
    ev.append((q * 2, bytes([0x80, 60, 0])))
    ev.append((0, bytes([0x80, 64, 0])))
    ev.append((0, bytes([0x80, 67, 0])))

    hdr = b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQN)
    return hdr + track(ev)


# Bach, BWV 846, bars 1-8.  Each bar is five pitches: two that are held and
# three that repeat above them.  Written the way the piece is built rather than
# as a list of notes, because that is what makes it eight lines instead of two
# hundred - and because the shape is the point: the same figure, moved.
PRELUDE = (
    (48, 52, 55, 60, 64),  # C  major
    (48, 50, 57, 62, 65),  # D  minor 7, over C
    (47, 50, 55, 62, 65),  # G  7, over B
    (48, 52, 55, 60, 64),  # C  major
    (48, 52, 57, 64, 69),  # A  minor 7, over C
    (48, 50, 54, 57, 62),  # D  7, over C
    (47, 50, 55, 62, 67),  # G  major, over B
    (47, 48, 52, 55, 60),  # C  major, over B
)


def make_prelude(transpose: int = 0) -> bytes:
    """`transpose` moves the whole thing in semitones.  It is here for one
    question and it is a question about the room, not about the file: an
    amplifier and a small speaker distort on the bottom two octaves long before
    anything else complains, and the same eight bars an octave up separate
    "the loudspeaker cannot do this" from "the sound path is broken"."""
    sixteenth = PPQN // 4
    half_bar = sixteenth * 8
    ev: list[tuple[int, bytes]] = [(0, bytes([0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20]))]
    abs_ev: list[tuple[int, bytes]] = []

    def note(at: int, pitch: int, ticks: int, vel: int) -> None:
        pitch += transpose
        abs_ev.append((at, bytes([0x90, pitch, vel])))
        # A hair short of its written length, so a repeated pitch has a moment
        # of silence to be a second note rather than one long one.
        abs_ev.append((at + ticks - 10, bytes([0x80, pitch, 0])))

    at = 0
    for bar in PRELUDE:
        p1, p2, p3, p4, p5 = bar
        for _ in range(2):  # each bar is its own first half, twice
            note(at, p1, half_bar, 88)
            note(at + sixteenth, p2, half_bar - sixteenth, 80)
            for i, p in enumerate((p3, p4, p5, p3, p4, p5)):
                note(at + sixteenth * (2 + i), p, sixteenth, 96)
            at += half_bar

    ev += deltas(abs_ev)
    hdr = b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQN)
    return hdr + track(ev)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--piece", choices=("demo", "prelude"), default="demo")
    ap.add_argument("--transpose", type=int, default=0,
                    metavar="SEMITONES",
                    help="move the piece up or down; 12 is an octave "
                         "(prelude only)")
    args = ap.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    data = (make_demo() if args.piece == "demo"
            else make_prelude(args.transpose))
    args.output.write_bytes(data)
    print(f"wrote {args.output} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
