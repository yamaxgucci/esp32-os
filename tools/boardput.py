#!/usr/bin/env python3
"""
Put a file on a running board, over the console it is already talking on.

The card comes out, goes into a laptop, comes back: that is thirty seconds of a
person's attention per attempt, and an attempt is how a driver gets debugged.
The board already has a way in - the shell's `recv <file>`, which reads hex and
needs no protocol at either end - so this drives it.

    python tools/boardput.py -p COM4 build/apps/REMDISP.SYS a:\\remdisp.sys

It is not fast and is not meant to be: hex over 115200 with the console echoing
every line is about 5 KB/s, so a driver is a second and a megabyte is four
minutes.  Use it for the small things that change often - a .SYS, a config, a
test image - and the card reader for the game.

Nothing here resets the board.  What is running keeps running, which is the
point: this is for replacing a driver and rebooting once, not for a harness.
"""
import argparse
import os
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("boardput: pyserial is missing.  Run this with the IDF python "
             "(argon env), which has it because esptool needs it.")

# The shell's line editor takes AG_LINE_MAX (256) including the newline, and it
# echoes what it is given.  96 bytes of payload is 192 characters of hex, which
# leaves room and keeps each line one comfortable write.
PER_LINE = 96


def drain(ser, seconds=0.3):
    end = time.time() + seconds
    out = bytearray()
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            out += chunk
            end = time.time() + seconds
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("src", help="file on this machine")
    ap.add_argument("dst", help="path on the board, e.g. a:\\remdisp.sys")
    args = ap.parse_args()

    with open(args.src, "rb") as f:
        data = f.read()
    print("boardput: %s -> %s, %d bytes" % (args.src, args.dst, len(data)))

    # No DTR/RTS handling at all: touching either resets the chip through the
    # auto-reset circuit, and the whole point is to leave the board running.
    ser = serial.Serial(args.port, args.baud, timeout=0.05, xonxoff=True)
    try:
        #
        # END first, in case the last run of this was interrupted.  A `recv`
        # that never finished is still sitting there reading lines as hex, and
        # the next attempt's "recv a:\..." goes into the file as data and comes
        # back as `not hex: 'r'` - which reads like a bug in this tool and is
        # not one.  At a prompt END is merely an unknown command.
        #
        ser.write(b"END\r\n")
        drain(ser, 0.4)
        ser.write(b"\r\n")
        drain(ser, 0.4)

        ser.write(("recv %s\r\n" % args.dst).encode())
        reply = drain(ser, 1.0)
        if b"send hex" not in reply:
            sys.exit("boardput: the board did not start recv:\n%s"
                     % reply.decode("latin-1")[-400:])

        sent = 0
        t0 = time.time()
        for i in range(0, len(data), PER_LINE):
            ser.write((data[i:i + PER_LINE].hex() + "\r\n").encode())
            #
            # Read the echo away, but never *wait* for it.  The port is the
            # floor here - 115200 baud carrying hex is 5.6 KB of file a second -
            # and the first version of this slept twenty milliseconds after
            # every line to let the echo arrive.  Twenty milliseconds times ten
            # thousand lines is three and a half minutes of pure sleeping on top
            # of the three minutes of wire, and worse, the sleep restarted every
            # time a byte turned up, which for an echoing console is always.  A
            # megabyte took a quarter of an hour and the processor was idle for
            # all of it.
            #
            # Nothing needs to be waited for.  The console's own XON/XOFF is the
            # flow control - pyserial honours it - so the only job here is to
            # keep the receive buffer from filling, and in_waiting says exactly
            # how much to take.
            #
            waiting = ser.in_waiting
            if waiting:
                ser.read(waiting)
            sent += min(PER_LINE, len(data) - i)
            if (i // PER_LINE) % 512 == 0:
                el = time.time() - t0
                rate = sent / el / 1024.0 if el > 0 else 0
                print("  %d/%d bytes, %.1f KB/s" % (sent, len(data), rate),
                      flush=True)

        ser.write(b"END\r\n")
        tail = drain(ser, 2.0)
        print(tail.decode("latin-1")[-300:])
    finally:
        ser.close()


if __name__ == "__main__":
    main()
