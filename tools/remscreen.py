#!/usr/bin/env python3
"""
What a neighbour's screen gets over the cluster link, with no screen on the
bench.

    python tools/remscreen.py --master COM6 --slave COM8 [--secs 10] [--rows 8]

The slave board (the one that would have the panel) loads NULLDISP.SYS, a
display that draws nothing and says in its journal, once a second, what
reached it: frames/s (pixels over the surface), KB/s, rectangles, text rows.
Both boards swap NODELINK for LINKSPI, the master attaches the slave's
devices (`link devs`), and then:

  graphics  GFXNOISE on the master for --secs, in bands of --rows: noise, so
            nothing compresses and every band crosses whole;
  text      a directory listing and a file typed on the master, whose
            console is now on the slave's "screen".

Nothing is written to flash on either side: the picture is drawn into
memory, sent, counted and thrown away.
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from linkbench import Board, Sink, say, up  # noqa: E402


def journal(b, pat, n=60):
    t = say(b, "log -n %d" % n, 10.0)
    return re.findall(pat, t)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--master", required=True)
    ap.add_argument("--slave", required=True)
    ap.add_argument("--secs", type=int, default=10)
    ap.add_argument("--rows", type=int, default=8)
    a = ap.parse_args()

    m = Board(a.master, 115200, Sink(), False)
    s = Board(a.slave, 115200, Sink(), False)
    try:
        up(m, "master")
        up(s, "slave")
        for b in (s, m):
            say(b, "drv unload NODELINK", 10.0)
        t = say(s, "drv load c:\\drv\\nulldisp.sys", 10.0)
        print("slave: %s" % ("NULLDISP loaded" if "NULLDISP:" in t
                             else "NULLDISP DID NOT LOAD"))
        for b in (s, m):
            t = say(b, "drv load c:\\drv\\linkspi.sys", 15.0)
            ok = re.search(r"/dev/node1 on spi\d cs\d+ as \w+ at (\d+) kHz", t)
            print("%s: %s" % ("slave" if b is s else "master",
                              ok.group(0) if ok else "LINKSPI DID NOT LOAD"))
        for b in (s, m):
            say(b, "link use node1", 10.0)
        time.sleep(2.0)
        t = say(m, "link devs", 20.0)
        lines = [l for l in t.splitlines() if "lcd0" in l or "display" in l]
        print("master link devs: %s" % (" | ".join(l.strip() for l in lines)
                                        or t[-300:]))

        # graphics
        t0 = time.time()
        m.buf = bytearray()
        m.send(("gfxnoise %d %d\r\n" % (a.secs, a.rows)).encode())
        # its own last line, not the first prompt that comes by
        m.wait_for(b"frames/s", a.secs + 30.0)
        m.pump(1.0)
        from linkbench import clean, visible
        t = clean(visible(bytes(m.buf)).decode("latin-1"))
        dt = time.time() - t0
        g = re.findall(r"gfxnoise: [^(\n]*?(?=gfxnoise:|\(\d\)|$|\n)", t)
        for line in (g or [t[-300:]]):
            print("master: %s" % line.strip()[:120])
        print("master: gfxnoise took %.1f s" % dt)
        for hit in journal(s, r"(?:nulldisp|rdev): [^\n]*?(?=[IWE] \(\d+\) |\(\d\) C:|$)",
                           3 * a.secs + 20)[-(2 * a.secs + 6):]:
            print("slave:  %s" % hit.strip()[:120])

        # text
        for cmd in ("dir c:\\", "type c:\\system.cfg", "dir c:\\drv"):
            say(m, cmd, 20.0)
        time.sleep(1.5)
        for hit in journal(s, r"nulldisp: [^\n(]*\([^)]*\)[^\n]*")[-4:]:
            print("slave text: %s" % hit.strip()[:120])

        for b, who in ((m, "master"), (s, "slave")):
            tt = say(b, "link", 10.0)
            fr = re.findall(r"frame \d+ B:[^\n]*", tt)
            by = re.findall(r"bytes\s+\d+ out, \d+ in", tt)
            print("%s link: %s; %s" % (who, fr[-1].strip() if fr else "?",
                                       by[-1] if by else "?"))
    finally:
        m.ser.close()
        s.ser.close()


if __name__ == "__main__":
    main()
