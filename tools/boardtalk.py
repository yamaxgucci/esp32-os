#!/usr/bin/env python3
"""Talk to a board's console without resetting it, and stream what comes back.

Two faults of the obvious approach cost most of an afternoon, and both are
here rather than in a habit:

**Opening the port reboots the board.**  DTR and RTS are wired to EN and IO0
on every one of these development boards - that is the auto-reset circuit the
flasher uses - and a serial library that asserts them at open pulses EN.  So a
program that "just looks at the console" restarts whatever was running, and a
game that had been up for a minute dies at the moment somebody tries to
measure it.  Clearing the lines *after* open, which is what tools/term.py
does, is too late: the pulse has already happened.  They have to be off before
the handle exists, which pyserial can do - construct, set dtr/rts, then open -
because on Windows it puts DTR_CONTROL_DISABLE into the DCB at open time.

**Buffered output is useless for a run in progress.**  A script that collects
everything and prints at the end tells you nothing while a board is doing the
thing you wanted to watch, and a background job holding the port is the only
way to keep the board alive - so the two conspire to make the run invisible.
Every line goes out as it arrives here, flushed.

    python tools/boardtalk.py COM4 --hold 300 --send "run a:/game.axe"
    python tools/boardtalk.py COM4 --hold 0 --send cls --send ver
    python tools/boardtalk.py COM4 --raw 1b31        # Alt+1, as bytes

Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
"""
import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("boardtalk: pyserial is missing - run this with the IDF python.")

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def open_quiet(port, baud):
    """A handle on the port that did not reboot the board to get it."""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.05
    ser.write_timeout = 10
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def pump(ser, seconds, seen):
    """Read for a while, printing whole lines once, as they arrive.

    The console is a screen: it repaints itself with cursor moves, so the same
    text comes down the wire again and again.  Printing every repaint buries
    the one new line among forty old ones, so a line already shown is dropped -
    which is why this keeps a set rather than a scrollback.
    """
    end = time.time() + seconds
    buf = ""
    while time.time() < end:
        chunk = ser.read(4096)
        if not chunk:
            continue
        buf += ANSI.sub("\n", chunk.decode("utf-8", "replace"))
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            line = line.rstrip()
            if line and line not in seen:
                seen.add(line)
                print(line, flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--send", action="append", default=[],
                    help="a command line to type; repeatable, in order")
    ap.add_argument("--raw", action="append", default=[],
                    help="hex bytes to send with no newline, e.g. 03 for Ctrl+C")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="seconds to read after each --send")
    ap.add_argument("--hold", type=float, default=0.0,
                    help="seconds to keep reading afterwards.  The port stays "
                         "open for this long, which is what keeps the board "
                         "from being reset by the next opener")
    args = ap.parse_args()

    ser = open_quiet(args.port, args.baud)
    seen = set()
    try:
        for raw in args.raw:
            ser.write(bytes.fromhex(raw))
            pump(ser, args.settle, seen)
        for cmd in args.send:
            ser.write(cmd.encode() + b"\r\n")
            pump(ser, args.settle, seen)
        if args.hold > 0:
            pump(ser, args.hold, seen)
    finally:
        ser.close()


main()
