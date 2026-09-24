#!/usr/bin/env python3
"""
Blocks across the wire between the S3-Zero and the Touch-LCD-2, counted.

    python tools/wireblk.py --zero COM6 --lcd COM9 --wire uart \\
        --rates 4000000,8000000 --blocks 64,512,4096 [--secs 5]
    python tools/wireblk.py --zero COM6 --lcd COM9 --wire spi \\
        --rates 20000,40000 --blocks 512,4096

Both directions for every case.  Over SPI the Zero always holds the clock:
Zero->LCD rides MOSI (`tx spim` / `rx spis`), LCD->Zero rides MISO (`tx spis` /
`rx spim`).  The slave's end is started first, because a master that clocks
into a slave that is not listening measures nothing.

One line per case, from the RECEIVER's own count (c:\\wireblk.txt on the board
that received), never from what the sender says it sent.
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boardtest import Board, visible  # noqa: E402

LOGDIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "logs")


class Sink(object):
    def __init__(self, f, tag):
        self.f, self.tag = f, tag

    def write(self, b):
        self.f.write(b)

    def flush(self):
        self.f.flush()


def clean(t):
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", t).replace("\r", "")


def say(b, cmd, timeout=10.0):
    b.buf = bytearray()
    b.send(cmd.encode() + b"\r\n")
    deadline = time.time() + timeout
    while time.time() < deadline:
        b.pump(0.25)
        seen = clean(visible(bytes(b.buf)).decode("latin-1"))
        at = seen.find(cmd)
        if at >= 0 and seen.find("C:\\>", at + len(cmd)) >= 0:
            return seen
    return clean(visible(bytes(b.buf)).decode("latin-1"))


def start(b, cmd):
    """Start an application and do not wait for it to finish."""
    b.buf = bytearray()
    b.send(cmd.encode() + b"\r\n")
    b.pump(1.0)


def boot(b):
    b.reset()
    b.wait_for(b"C:\\>", 40.0)
    b.pump(1.0)
    # The C3's USB-Serial-JTAG ignores the RTS pulse: its uptime went on
    # through every "reset", and what a case before left in the peripherals
    # (a UHCI controller on the port, the SPI slave's staging) was there for
    # the next.  So the system is asked to restart itself as well.
    b.buf = bytearray()
    b.send(b"reboot\r\n")
    b.wait_for(b"C:\\>", 40.0)
    b.pump(1.0)
    say(b, "", 3.0)
    say(b, "drv unload NODELINK", 10.0)


DOC = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "docs", "plans", "wire-blocks.md")


HEADER = ("| время | провод | load | скорость | блок, Б | направление | sent | good "
          "| bad | lost | КБ/с | %max |\n|---|---|---|---|---|---|---|---|---|---|---|---|\n")


def doc_row(cells):
    """One finished case appended to the table in docs/plans/wire-blocks.md.

    If the file does not end in a table row - a note was written under the
    last table - a header comes first, or the rows render as plain text."""
    with open(DOC, encoding="utf-8") as f:
        lines = [x for x in f.read().split("\n") if x.strip()]
    with open(DOC, "a", encoding="utf-8", newline="") as out:
        if not lines or not lines[-1].startswith("|"):
            out.write("\n" + HEADER)
        out.write("| " + " | ".join(str(c) for c in cells) + " |\n")


def _last(pattern, text):
    found = list(re.finditer(pattern, text))
    return found[-1] if found else None


def verdict(b, rate, block):
    # the LCD-2's console is forty columns, so the line comes back wrapped in
    # the middle of a number: join the pieces before reading it
    t = say(b, "type c:\\wireblk.txt", 10.0).replace("\n", "")
    m = _last(r"wireblk\s*rx\s*(\w+?)\s*rate=(\d+)\s*block=(\d+)\s*good=(\d+)\s*bad=(\d+)\s*"
                  r"missing=(\d+)\s*stray=(\d+)\s*ms=(\d+)\s*kbs=(\d+)", t)
    if not m or int(m.group(2)) != rate or int(m.group(3)) != block:
        return None
    return dict(good=int(m.group(4)), bad=int(m.group(5)),
                missing=int(m.group(6)), stray=int(m.group(7)),
                ms=int(m.group(8)), kbs=int(m.group(9)))


def sent_count(b, rate, block):
    t = say(b, "type c:\\wireblk_tx.txt", 10.0).replace("\n", "")
    m = _last(r"wireblk\s*tx\s*(\w+?)\s*rate=(\d+)\s*block=(\d+)\s*sent=(\d+)\s*"
                  r"refused=(\d+)\s*ms=(\d+)", t)
    if not m or int(m.group(2)) != rate or int(m.group(3)) != block:
        return None
    return int(m.group(4))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zero", required=True)
    ap.add_argument("--lcd", required=True)
    ap.add_argument("--wire", choices=("uart", "uartcpu", "uartl", "spi"), required=True)
    ap.add_argument("--rates", required=True)
    ap.add_argument("--blocks", required=True)
    ap.add_argument("--secs", type=int, default=5)
    ap.add_argument("--dirs", default="zl,lz")
    # the far board need not be the LCD-2: its name in the table, and the
    # UART its end of the wire is on (the C3's is UART0)
    ap.add_argument("--far-name", default="LCD")
    # after each case, the receiver's sorting of its damaged blocks
    ap.add_argument("--diag", action="store_true")
    # an experiment, not a measurement for the table
    ap.add_argument("--no-doc", action="store_true")
    # the sender and the receiver in different UART modes, to tell whose
    # fault a loss is (uart = DMA, uartcpu = FIFO and interrupt)
    ap.add_argument("--tx-mode", default=None)
    ap.add_argument("--rx-mode", default=None)
    ap.add_argument("--far-port", type=int, default=None)
    # extra words for the receiver's command line (e.g. "sink=1", "frame=2048")
    ap.add_argument("--rx-args", default="")
    ap.add_argument("--slave-first", action="store_true")
    ap.add_argument("--tx-args", default="")
    # the stress: compute threads on the sending board, the receiving one,
    # or both, for as long as the transfer runs (wireblk burn=2)
    ap.add_argument("--burn", choices=("none", "tx", "rx", "both"),
                    default="none")
    ap.add_argument("--log", default=os.path.join(LOGDIR, "wireblk.log"))
    a = ap.parse_args()
    if a.no_doc:
        global doc_row
        doc_row = lambda cells: None  # noqa: E731

    os.makedirs(LOGDIR, exist_ok=True)
    f = open(a.log, "ab")
    z = Board(a.zero, 115200, Sink(f, "zero"), False)
    l = Board(a.lcd, 115200, Sink(f, "lcd"), False)
    try:
        boot(z)
        boot(l)
        unit = "baud" if a.wire.startswith("uart") else "kHz"
        print("%-6s %-9s %-6s %-9s %7s %7s %6s %7s %8s %6s" %
              ("wire", unit, "block", "dir", "sent", "good", "bad", "lost",
               "KB/s", "%max"))
        for rate in [int(x) for x in a.rates.split(",")]:
            for block in [int(x) for x in a.blocks.split(",")]:
                for d in a.dirs.split(","):
                    if a.wire.startswith("uart"):
                        tx, rx = (z, l) if d == "zl" else (l, z)
                        first, second = ((rx, "rx " + (a.rx_mode or a.wire)),
                                         (tx, "tx " + (a.tx_mode or a.wire)))
                    else:
                        if d == "zl":
                            first, second = (l, "rx spis"), (z, "tx spim")
                            rx = l
                        else:
                            # The receiving master first, and longer: it only
                            # clocks empty frames until the slave opens, and
                            # started second it stopped while the slave still
                            # had a pipeline's worth (24 KB) queued for it -
                            # counted as lost, and not the wire's doing.
                            if a.slave_first:
                                first, second = (l, "tx spis"), (z, "rx spim")
                            else:
                                first, second = (z, "rx spim"), (l, "tx spis")
                            rx = z
                    def load(board, role):
                        on = (a.burn == "both" or a.burn == role[:2])
                        extra = " burn=2" if on else ""
                        if board is l and a.far_port is not None:
                            extra += " port=%d" % a.far_port
                        more = a.rx_args if role.startswith("rx") else a.tx_args
                        if more:
                            extra += " " + more
                        return extra
                    # the first one runs longer so it covers the second
                    start(first[0], "run c:\\wireblk.axe %s %d %d %d%s" %
                          (first[1], rate, block, a.secs + 4,
                           load(first[0], first[1])))
                    time.sleep(1.5)
                    start(second[0], "run c:\\wireblk.axe %s %d %d %d%s" %
                          (second[1], rate, block, a.secs,
                           load(second[0], second[1])))
                    time.sleep(a.secs + 6)
                    z.pump(0.5)
                    l.pump(0.5)
                    v = verdict(rx, rate, block)
                    txb = second[0] if second[1].startswith("tx") else first[0]
                    sent = sent_count(txb, rate, block)
                    name = ("Z->" + a.far_name if d == "zl"
                            else a.far_name + "->Z")
                    stamp = time.strftime("%d.%m %H:%M")
                    rate_s = ("%g Мбод" % (rate / 1e6) if a.wire.startswith("uart")
                              else "%g МГц" % (rate / 1e3))
                    if v is None:
                        print("%-6s %-9d %-6d %-9s %s" %
                              (a.wire, rate, block, name, "NO REPORT"))
                        doc_row([stamp, a.wire, a.burn, rate_s, block, name,
                                 "—", "—", "—", "—", "нет отчёта", "—"])
                        boot(z)
                        boot(l)
                        continue
                    # the most the line can carry: 8N1 is ten bits a byte on
                    # the UART, and SPI clocks one bit a cycle
                    top = (rate / 10.0 if a.wire.startswith("uart")
                           else rate * 1000.0 / 8.0) / 1024.0
                    # LOST is every block the sender counted out that did not
                    # arrive intact: damaged, missing between two good ones, or
                    # gone before the first or after the last
                    lost = (sent - v["good"]) if sent is not None else -1
                    print("%-6s %-9d %-6d %-9s %7s %7d %6d %7s %8d %5.1f%%" %
                          (a.wire, rate, block, name,
                           str(sent) if sent is not None else "?",
                           v["good"], v["bad"],
                           str(lost) if lost >= 0 else "?",
                           v["kbs"], 100.0 * v["kbs"] / top))
                    doc_row([stamp, a.wire, a.burn, rate_s, block, name,
                             sent if sent is not None else "?", v["good"],
                             v["bad"], lost if lost >= 0 else "?", v["kbs"],
                             "%.1f" % (100.0 * v["kbs"] / top)])
                    if a.diag:
                        t = say(rx, "type c:\\wireblk_bad.txt", 10.0)
                        t = t.replace("\n", " ")
                        m = _last(r"flip1=.*?bits=[\d,\s]*", t)
                        print("    diag:", m.group(0) if m else "none")
                        for mm in list(re.finditer(
                                r"(?:nxt=\d+/\d+\s*)?"
                                r"(flip1|flipN|drop|dup|other)\s*seq=\d+\s*"
                                r"at=\d+\s*wrong=\d+\s*want=\d+\s*got=\d+",
                                t))[-6:]:
                            print("     ", re.sub(r"\s+", " ", mm.group(0)))
                    if a.diag and not a.wire.startswith("uart"):
                        f.flush()
                        with open(a.log, "rb") as lf:
                            lf.seek(0, 2)
                            lf.seek(max(0, lf.tell() - 400000))
                            t = clean(lf.read().decode("latin-1"))
                        t = t.replace("\n", " ")
                        for role in ("spim", "spis"):
                            mm = _last(r"wireblk\s*link\s*" + role +
                                       r":.*?errors=\d+", t)
                            print("    link %s:" % role,
                                  re.sub(r"\s+", " ", mm.group(0))[:130]
                                  if mm else "?")
                    sys.stdout.flush()
    finally:
        z.ser.close()
        l.ser.close()
        f.close()


if __name__ == "__main__":
    main()
