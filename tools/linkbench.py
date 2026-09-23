#!/usr/bin/env python3
"""
How fast the cluster link between two boards really is, end to end.

    python tools/linkbench.py --master COM6 --slave COM9 [--spi] [--file c:\\desktop.axe]

Brings the link up on both boards and waits for each step to answer before the
next one - which is the whole reason this exists beside spiprobe.py: that tool
talks to the slave while it is still loading, and a slave that never bound its
end leaves MISO floating beside MOSI, so the master reads its OWN frames back.
It printed "this node is zero, the other is zero" and looked exactly like a
timing fault on the wire, and it was taken for one.

Then it asks the master for the peer's name (a wrong name means the link is
not up, whatever else it says) and fetches a file off the slave with
`link get`, timing it. The number is file bytes over wall time: what a user of
the link gets, not what the wire clocked.

--spi uses LINKSPI on both ends instead of the UART link each board binds at
boot. The clock is whatever [linkspi] khz in each board's SYSTEM.CFG says.
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boardtest import Board, visible  # noqa: E402


class Sink(object):
    def write(self, b):
        pass

    def flush(self):
        pass


def clean(t):
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", t).replace("\r", "")


def say(b, cmd, timeout=10.0, settle=0.5):
    """Send a command and read until the prompt comes back after it."""
    b.buf = bytearray()
    b.send(cmd.encode() + b"\r\n")
    deadline = time.time() + timeout
    seen = ""
    while time.time() < deadline:
        b.pump(0.25)
        seen = clean(visible(bytes(b.buf)).decode("latin-1"))
        # the echo, then something, then a prompt
        at = seen.find(cmd)
        if at >= 0 and seen.find("C:\\>", at + len(cmd)) >= 0:
            b.pump(settle)
            return clean(visible(bytes(b.buf)).decode("latin-1"))
    return seen


def up(b, name):
    b.reset()
    # a prompt, not a number of seconds: the screen board's boot waits on a
    # card that is not there and its length moves from 0.2 s to 5 s
    b.wait_for(b"C:\\>", 40.0)
    b.pump(1.0)
    say(b, "", 3.0)
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--master", required=True)
    ap.add_argument("--slave", required=True)
    ap.add_argument("--spi", action="store_true")
    ap.add_argument("--file", default="c:\\desktop.axe")
    a = ap.parse_args()

    m = Board(a.master, 115200, Sink(), False)
    s = Board(a.slave, 115200, Sink(), False)
    try:
        up(m, "master")
        up(s, "slave")

        if a.spi:
            # one wire, one driver: the UART link and the SPI link share pins
            for b in (s, m):
                say(b, "drv unload NODELINK", 10.0)
            for b in (s, m):
                t = say(b, "drv load c:\\drv\\linkspi.sys", 15.0)
                ok = re.search(r"/dev/node1 on spi\d cs\d+ as \w+ at (\d+) kHz", t)
                print("%s: linkspi %s" % ("slave" if b is s else "master",
                                          ok.group(0) if ok else "DID NOT LOAD"))
            for b in (s, m):
                say(b, "link use node1", 10.0)
            wire = "node1"
        else:
            wire = "node0"

        time.sleep(2.0)
        t = say(m, "link", 10.0)
        who = re.findall(r"the other is (\w+)", t)
        print("master sees: %s over %s" % (who[-1] if who else "nobody", wire))

        t0 = time.time()
        t = say(m, "link get %s c:\\probe.bin" % a.file, 180.0)
        dt = time.time() - t0
        got = re.findall(r"(\d+) bytes from the other node", t)
        if got:
            n = int(got[-1])
            print("link get: %d bytes in %.2f s = %d B/s" % (n, dt, n / dt))
        else:
            err = re.findall(r"link get: (-?\d+)", t)
            print("link get FAILED: %s after %.1f s" % (err[-1] if err else "?", dt))
        t = say(m, "link", 10.0)
        f = re.findall(r"frames\s+(\d+) sent \((\d+) again\), (\d+) received", t)
        l = re.findall(r"losses\s+(\d+) gaps, (\d+) duplicates, (\d+) bad", t)
        if f:
            print("master frames: sent %s (%s again), received %s" % f[-1])
        if l:
            print("master losses: %s gaps, %s duplicates, %s bad" % l[-1])
        for b, who in ((m, "master"), (s, "slave")):
            tt = say(b, "link", 10.0)
            ak = re.findall(r"acks\s+(\d+) sent alone", tt)
            tm = re.findall(r"timer\s+(\d+) ms, round trip ([^()]+?) \(", tt)
            print("%s: acks alone %s, timer %s" % (who, ak[-1] if ak else "?",
                                                   tm[-1] if tm else "?"))
        t = say(s, "link", 10.0)
        f = re.findall(r"frames\s+(\d+) sent \((\d+) again\), (\d+) received", t)
        if f:
            print("slave frames:  sent %s (%s again), received %s" % f[-1])
        sp = re.findall(r"(\d+) blocks clocked by the peer.{0,200}", t)
        if sp:
            print("slave wire:    %s" % sp[-1][:200])
        dr = re.findall(r"(\d+) into nothing armed, (\d+) bytes with no room,"
                        r" (\d+) timeouts, (\d+) stalls", t)
        if dr:
            print("slave faults:  %s unarmed, %s dropped, %s timeouts, %s stalls"
                  % dr[-1])
        for b, who in ((m, "master"), (s, "slave")):
            j = say(b, "log", 10.0)
            for hit in re.findall(r"W \(\d+\) (?:linkspi|spilink|link|rdev)[^|]{0,160}", j)[-4:]:
                print("%s journal: %s" % (who, hit.strip()[:170]))
    finally:
        m.ser.close()
        s.ser.close()


if __name__ == "__main__":
    main()
