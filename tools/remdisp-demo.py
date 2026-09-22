#!/usr/bin/env python3
"""One board's application, the other board's glass, over the wire that is there.

    python tools/remdisp-demo.py --host COM6 --screen COM9

--host    the board with no display of its own (the S3-Zero).  It loads
          REMDISP.SYS, which publishes a display whose panel is uart1, and then
          runs something that draws.
--screen  the board that is mostly a panel (the Touch-LCD-2).  It runs
          REMTERM.AXE, which puts what arrives on the glass.

NOTHING IS RESOLDERED.  The four wires between these two boards were put there
for the SPI link - 10, 11, 12, 13 and a ground - and a UART needs two of them;
both chips route a UART to any pin through the matrix.  11 and 13 carry it, so
10 and 12 are still free and the SPI link is a configuration away.

AND THIS PATH IS NOT THE CLUSTER LINK.  REMDISP writes pixels straight to the
port: no frames, no window, no acknowledgements, no retransmissions.  That is
why it runs at the wire's own rate - 400 067 B/s measured at 4 Mbaud with zero
corrupt blocks - while the reliable link over the same copper manages a few
thousand.  The two exist for different jobs and the numbers are not comparable.
"""

import argparse
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from boardtest import Board, visible  # noqa: E402

LOGDIR = os.path.join(ROOT, "logs")


class Sink:
    def __init__(self, f, tag):
        self.f, self.tag = f, tag

    def write(self, data):
        for line in data.replace(b"\r\n", b"\n").split(b"\n"):
            self.f.write(("[%s] " % self.tag).encode() + line + b"\n")
        self.f.flush()

    def flush(self):
        pass


def say(board, cmd, wait=3.0, timeout=20.0):
    board.buf = bytearray()
    board.send(cmd.encode() + b"\r")
    board.wait_prompt(timeout)
    board.pump(wait)
    return visible(bytes(board.buf)).decode("latin-1")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--screen", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--secs", type=int, default=90,
                    help="how long the screen end serves")
    ap.add_argument("--app", default="c:\\gfxdemo.axe")
    ap.add_argument("--log", default=os.path.join(LOGDIR, "remdisp.log"))
    a = ap.parse_args()

    os.makedirs(LOGDIR, exist_ok=True)
    f = open(a.log, "wb")
    h = Board(a.host, a.baud, Sink(f, "host"), False)
    g = Board(a.screen, a.baud, Sink(f, "screen"), False)

    try:
        h.reset()
        g.reset()
        h.pump(8.0)
        g.pump(8.0)

        # THE RECEIVER FIRST.  The sender's first band would otherwise land on a
        # board that is not listening, and a picture that starts half way
        # through is the kind of thing that gets blamed on the wire.
        print(say(g, "run c:\\remterm.axe 4000000 %d" % a.secs, wait=2.0,
                  timeout=15.0))
        time.sleep(2.0)

        # THE CLUSTER LINK LETS GO OF THE WIRE FIRST.
        #
        # NODELINK binds uart1 at boot and REMDISP wants the same port at a
        # different baud, so both drivers end up writing to one UART.  The first
        # run of this showed 11343 STRAY bytes at the screen end and a -60 from
        # the link: the two of them talking over each other.  One wire, one
        # driver - and that is the whole reason this demo is a separate script
        # and not a line added to the cluster's harness.
        print(say(h, "drv unload NODELINK", wait=2.0, timeout=20.0))
        print(say(h, "drv load c:\\drv\\remdisp.sys", wait=2.0, timeout=30.0))
        print(say(h, "dev", wait=2.0))
        print(say(h, "run %s" % a.app, wait=6.0, timeout=60.0))

        # What the screen end saw of it, in its own words.
        g.pump(3.0)
        print(visible(bytes(g.buf)).decode("latin-1")[-1200:])
        return 0
    finally:
        h.ser.close()
        g.ser.close()
        f.close()
        print("transcript: %s" % a.log)


if __name__ == "__main__":
    sys.exit(main())
