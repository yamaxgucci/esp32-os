#!/usr/bin/env python3
"""
Make a Mega Drive cartridge image ready for MD.AXE: a .SWP beside it.

    python tools/mdrom.py Sonic.bin [more.md game.smd ...]

MD reads the cartridge out of flash in place, a 16-bit word at a time, and
the ESP32 reads a word with its bytes the other way round from the 68000 - so
it wants the image with each pair of bytes swapped, a .SWP.  It makes one
itself the first time a game is started, but on the board that takes a while
and needs room on the card for a second copy; this does the same on the
computer (docs/mega-drive.md, section 3).

Takes a plain dump (.BIN, .MD, .GEN: "SEGA" at 0x100) and a Super Magic
Drive copier file (.SMD: a 512-byte header, then 16 KB blocks with the odd
bytes in the first half and the even ones in the second), which MD cannot
open at all.  A file that is swapped already ("ESAG" at 0x100) is left alone.
"""
import os
import sys


def deinterleave(data):
    """An .SMD file to a plain dump."""
    body = data[512:]
    out = bytearray(len(body))
    for b in range(0, len(body), 16384):
        block = body[b:b + 16384]
        half = len(block) // 2
        out[b + 1:b + 2 * half:2] = block[:half]
        out[b:b + 2 * half:2] = block[half:2 * half]
    return out


def is_smd(data):
    # The copier's header: byte 8 is 0xAA and byte 9 is 0xBB, and the file is
    # 512 bytes over a whole number of 16 KB blocks.
    return (len(data) % 16384 == 512 and len(data) > 512 and
            data[8] == 0xAA and data[9] == 0xBB)


def main(paths):
    if not paths:
        print(__doc__.strip().splitlines()[2].strip())
        return 2
    bad = 0
    for path in paths:
        data = bytearray(open(path, 'rb').read())
        if is_smd(data):
            data = deinterleave(data)
            kind = 'SMD'
        else:
            kind = 'plain'
        head = bytes(data[0x100:0x104])
        if head == b'ESAG':
            print('%s: swapped already, nothing to do' % path)
            continue
        if head != b'SEGA':
            print('%s: no "SEGA" at 0x100 - not a Mega Drive image MD can read'
                  % path)
            bad += 1
            continue
        if len(data) % 2:
            data.append(0xFF)
        data[0::2], data[1::2] = data[1::2], data[0::2]
        out = os.path.splitext(path)[0] + '.SWP'
        open(out, 'wb').write(data)
        print('%s (%s) -> %s, %u KB' % (path, kind, out, len(data) // 1024))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
