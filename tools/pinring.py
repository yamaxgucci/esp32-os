#!/usr/bin/env python3
"""
Ring out the bundle between the S3-Zero and the C3, both ways.

    python tools/pinring.py --zero COM6 --c3 COM8

For every wire: one board drives it alone high (the reader pulled down), then
alone low (the reader pulled up), the other board reads all four pins.  The
expected pin shows the driven level and only it; anything else is printed.
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wireblk import Sink, boot, say, start, LOGDIR  # noqa: E402
from boardtest import Board  # noqa: E402

# Zero pin, C3 pin, and what the wire is for
WIRES = [(12, 6, "CLK"), (11, 7, "MOSI / UART Zero->C3"),
         (13, 5, "MISO / UART C3->Zero"), (10, 10, "CS")]


def read(b, pull, pins):
    t = say(b, "run c:\\pins.axe in %s %s" % (pull, " ".join(map(str, pins))),
            8.0).replace("\n", "")
    m = list(re.finditer(r"pins in((?:\s*\d+=\d)+)\s*end", t))
    if not m:
        return None
    return {int(p): int(v) for p, v in re.findall(r"(\d+)=(\d)", m[-1].group(1))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zero", required=True)
    ap.add_argument("--c3", required=True)
    a = ap.parse_args()
    os.makedirs(LOGDIR, exist_ok=True)
    f = open(os.path.join(LOGDIR, "pinring.log"), "ab")
    z = Board(a.zero, 115200, Sink(f, "zero"), False)
    c = Board(a.c3, 115200, Sink(f, "c3"), False)
    bad = 0
    try:
        boot(z)
        boot(c)
        for side in ("zero", "c3"):
            drv, rd = (z, c) if side == "zero" else (c, z)
            dpins = [x[0] if side == "zero" else x[1] for x in WIRES]
            rpins = [x[1] if side == "zero" else x[0] for x in WIRES]
            for i, w in enumerate(WIRES):
                for lone, pull in ((1, "down"), (0, "up")):
                    levels = " ".join("%d=%d" % (p, lone if j == i else 1 - lone)
                                      for j, p in enumerate(dpins))
                    start(drv, "run c:\\pins.axe out 4 " + levels)
                    got = read(rd, pull, rpins)
                    time.sleep(3.5)
                    want = {p: (lone if j == i else 1 - lone)
                            for j, p in enumerate(rpins)}
                    ok = got == want
                    bad += 0 if ok else 1
                    print("%-4s drives %-2d %-22s alone=%d  %s  read %s" %
                          (side, dpins[i], w[2], lone, "OK " if ok else "BAD",
                           got))
                    sys.stdout.flush()
        print("ALL WIRES OK" if bad == 0 else "%d CHECKS FAILED" % bad)
    finally:
        z.ser.close()
        c.ser.close()
        f.close()


if __name__ == "__main__":
    main()
