#!/usr/bin/env python3
"""Ask the SPI pair one question at a time, and write the answers OUTSIDE build/.

    python tools/spiprobe.py --master COM6 --slave COM9 look
    python tools/spiprobe.py --master COM6 --slave COM9 get --file c:\\nodehi.axe

WHY THIS EXISTS RATHER THAN clustertest.py: that harness asserts, and an
assertion is the wrong instrument for a number nobody knows yet.  This one takes
no view of what should happen - it runs commands on both boards, times the one
that matters, and prints both ends' counters before and after so the difference
can be read.  The same shape as tools/xcluprobe.ps1 on the emulated pair.

AND THE LOGS DO NOT GO IN build/.  Every raw log of the first SPI round was
written to build/cluster-stash/ and is gone: `argon target` fullcleans build/,
which is the tree's own documented behaviour.  Evidence lives in logs/.
"""

import argparse
import io
import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from boardtest import Board, visible  # noqa: E402

LOGDIR = os.path.join(ROOT, "logs")


class Sink:
    """What Board.pump writes every byte it reads into, tagged.

    Board takes a `log` with a one-argument write and calls it from pump, so
    the transcript holds everything that arrived and not only what a command
    happened to return.
    """

    def __init__(self, tee, tag):
        self.tee = tee
        self.tag = tag

    def write(self, data):
        self.tee.write(self.tag, data)

    def flush(self):
        """Board.pump flushes after every read; Tee already does."""


class Tee:
    """One transcript for both boards, tagged, in bytes - boardtest's rule."""

    def __init__(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.f = open(path, "wb")

    def write(self, tag, data):
        if not data:
            return
        for line in data.replace(b"\r\n", b"\n").split(b"\n"):
            self.f.write(("[%s] " % tag).encode() + line + b"\n")
        self.f.flush()

    def note(self, text):
        self.f.write(("=== %s\n" % text).encode())
        self.f.flush()
        print(text)


def run(board, tee, tag, cmd, settle=1.0, timeout=20.0):
    """Send one command, wait for its prompt, return what came back."""
    board.buf = bytearray()
    board.send(cmd.encode() + b"\r")
    ok = board.wait_prompt(timeout)
    board.pump(settle)
    out = bytes(board.buf)
    if not ok:
        tee.note("%s: no prompt after %r" % (tag, cmd))
    # Text, not bytes: visible() strips the escapes and leaves bytes, and every
    # caller here wants to read it.  Latin-1 so no byte can fail to decode -
    # boardtest's own rule about transcripts.
    return visible(out).decode("latin-1")


def last(pattern, text):
    """The LAST match, never the first.

    What comes back from a board is a RENDERED SCREEN, not a log: when it
    scrolls, rows that are still on it are drawn again, so a transcript of one
    command contains the output of the previous ones as well.  Taking the first
    match reads the stale painting - and then two samples around a transfer
    subtract to zero and the transfer looks free.  Measured here first time out.
    """
    found = re.findall(pattern, text)
    return found[-1] if found else None


def counters(text):
    """Pull the numbers out of a `link` printout, so two of them subtract."""
    flat = " ".join(text.split())
    out = {}
    m = last(r"frames\s+(\d+) sent \((\d+) again\), (\d+) received", flat)
    if m:
        out["sent"], out["again"], out["recv"] = (int(x) for x in m)
    m = last(r"losses\s+(\d+) gaps, (\d+) duplicates, (\d+) bad", flat)
    if m:
        out["gaps"], out["dups"], out["bad"] = (int(x) for x in m)
    m = last(r"bytes\s+(\d+) out, (\d+) in", flat)
    if m:
        out["tx"], out["rx"] = int(m[0]), int(m[1])
    m = last(r"round trip (\d+) ms", flat)
    if m:
        out["srtt"] = int(m)
    m = last(r"spi\d\s+(\d+) blocks clocked by the peer, (\d+) with", flat)
    if m:
        out["blocks"], out["blocks_data"] = int(m[0]), int(m[1])
    return out


def delta(before, after, keys):
    return {k: after.get(k, 0) - before.get(k, 0) for k in keys
            if k in before or k in after}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--master", required=True)
    ap.add_argument("--slave", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--reset", action="store_true",
                    help="reset both boards first (they rebind at boot)")
    ap.add_argument("--file", default="c:\\nodehi.axe",
                    help="what to fetch off the slave")
    ap.add_argument("--to", default="c:\\probe.bin")
    ap.add_argument("--log", default=os.path.join(LOGDIR, "spiprobe.log"))
    ap.add_argument("--up", action="store_true",
                    help="load linkspi and bind node1 on both first")
    ap.add_argument("--khz", type=int, default=0,
                    help="rewrite [linkspi] khz on BOTH boards and reboot them")
    ap.add_argument("--gap", type=int, default=-1,
                    help="rewrite [linkspi] gap_us on the master the same way")
    ap.add_argument("--gapmax", type=int, default=-1,
                    help="[linkspi] gap_max_us: 0 turns the adaptive growth off")
    ap.add_argument("--put", action="append", default=[],
                    metavar="HOST=GUEST",
                    help="deliver a file to BOTH boards before anything else")
    ap.add_argument("what", choices=("look", "get"))
    a = ap.parse_args()

    tee = Tee(a.log)
    m = Board(a.master, a.baud, Sink(tee, 'master'), False)
    s = Board(a.slave, a.baud, Sink(tee, 'slave'), False)

    # A setting swept from here rather than from a reflash.  The two boards
    # carry their pack's SYSTEM.CFG byte for byte (checked: 3983 and 6791), so
    # rewriting one key in the pack copy and delivering it leaves everything
    # else exactly as it was - which is what makes two runs comparable.
    packs = {"master": os.path.join(ROOT, "boards", "esp32s3-zero",
                                    "SYSTEM.CFG"),
             "slave": os.path.join(ROOT, "boards", "esp32s3-touch-lcd-2",
                                   "SYSTEM.CFG")}

    def retune(tag, path):
        text = io.open(path, encoding="utf-8", newline="").read()
        # Inside [linkspi] only: `khz` appears in [spi2] and [spi3] as well, and
        # those are where the bus comes up before the driver asks for its own
        # rate - changing them would move a second variable.
        head, _, tail = text.partition("[linkspi]")
        if a.khz:
            tail = re.sub(r"(?m)^khz\s*=\s*\d+", "khz  = %d" % a.khz, tail, 1)
        if a.gap >= 0 and "gap_us" in tail:
            tail = re.sub(r"(?m)^gap_us\s*=\s*\d+", "gap_us = %d" % a.gap,
                          tail, 1)
        if a.gapmax >= 0:
            # The key may not be in the pack at all - it is new - so set it if
            # it is there and append it to the section if it is not.  Appended
            # right after `dev`, which every [linkspi] has, so it lands inside
            # the section and not in whatever follows it.
            if re.search(r"(?m)^gap_max_us\s*=", tail):
                tail = re.sub(r"(?m)^gap_max_us\s*=\s*\d+",
                              "gap_max_us = %d" % a.gapmax, tail, 1)
            else:
                tail = re.sub(r"(?m)^(dev\s*=.*)$",
                              r"\1\ngap_max_us = %d" % a.gapmax, tail, 1)
        out = os.path.join(LOGDIR, "SYSTEM-%s.CFG" % tag)
        os.makedirs(LOGDIR, exist_ok=True)
        io.open(out, "w", encoding="utf-8", newline="").write(
            head + "[linkspi]" + tail)
        return out

    try:
        if a.reset:
            tee.note("resetting both")
            m.reset()
            s.reset()
            # Long enough for the boot to finish and the wire to bind.
            m.pump(6.0)
            s.pump(6.0)
        else:
            # A bare return, to find the prompt without doing anything.
            run(m, tee, "master", "")
            run(s, tee, "slave", "")

        if a.khz or a.gap >= 0 or a.gapmax >= 0:
            tee.note("retuning: khz=%s gap=%s gapmax=%s"
                     % (a.khz or "-", a.gap if a.gap >= 0 else "-",
                        a.gapmax if a.gapmax >= 0 else "-"))
            for board, tag in ((m, "master"), (s, "slave")):
                board.put(retune(tag, packs[tag]), "c:\\system.cfg",
                          chunk_bytes=127)
            # Read at boot and nowhere else, so the change is a reboot away.
            tee.note("rebooting into the new settings")
            m.reset()
            s.reset()
            m.pump(6.0)
            s.pump(6.0)

        for spec in a.put:
            host, guest = spec.split("=", 1)
            for board, tag in ((m, "master"), (s, "slave")):
                tee.note("%s: %s -> %s" % (tag, host, guest))
                # Board.put compares what the guest says it wrote with what was
                # sent and raises when they differ, which is the only thing that
                # makes the console path safe to use for a driver image.
                board.put(host, guest, chunk_bytes=127)

        if a.up:
            # BOTH SIDES, and the slave FIRST: the master starts clocking the
            # moment it binds, and blocks clocked into a board that has not
            # armed anything are blocks lost with nothing counted anywhere.
            tee.note("bringing the SPI link up")
            for board, tag in ((s, "slave"), (m, "master")):
                print(run(board, tee, tag,
                          "drv load c:\\drv\\linkspi.sys", timeout=60.0))
                print(run(board, tee, tag, "link use node1", timeout=60.0))

        if a.what == "look":
            for board, tag in ((m, "master"), (s, "slave")):
                tee.note("--- %s ---" % tag)
                for cmd in ("ver", "link", "link nodes", "dir c:\\"):
                    print(run(board, tee, tag, cmd))
            return 0

        # ---- the one measurement ------------------------------------------
        tee.note("baseline")
        mb = counters(run(m, tee, "master", "link"))
        sb = counters(run(s, tee, "slave", "link"))
        tee.note("master before: %r" % mb)
        tee.note("slave  before: %r" % sb)

        tee.note("fetching %s" % a.file)
        t0 = time.time()
        got = run(m, tee, "master", "link get %s %s" % (a.file, a.to),
                  settle=0.5, timeout=180.0)
        secs = time.time() - t0
        print(got)

        tee.note("after")
        ma = counters(run(m, tee, "master", "link"))
        sa = counters(run(s, tee, "slave", "link"))

        keys = ("sent", "again", "recv", "gaps", "dups", "bad", "tx", "rx",
                "blocks", "blocks_data")
        dm = delta(mb, ma, keys)
        ds = delta(sb, sa, keys)
        tee.note("wall %.2f s" % secs)
        tee.note("master delta: %r" % dm)
        tee.note("slave  delta: %r" % ds)

        # The arithmetic that names the bottleneck, printed rather than left to
        # be redone by hand every time.
        nbytes = 0
        mm = re.search(r"(\d+) bytes from the other node", " ".join(got.split()))
        if mm:
            nbytes = int(mm.group(1))
        if nbytes and secs > 0:
            tee.note("payload %d B in %.2f s = %.0f B/s" %
                     (nbytes, secs, nbytes / secs))
        if dm.get("tx"):
            # The master counts a whole BLOCK per transaction, both ways.
            tee.note("master blocks (tx/512) = %.0f, %.0f blocks/s" %
                     (dm["tx"] / 512.0, dm["tx"] / 512.0 / secs))
        if ds.get("blocks"):
            tee.note("slave blocks %d (%.0f/s), with bytes %d" %
                     (ds["blocks"], ds["blocks"] / secs,
                      ds.get("blocks_data", 0)))
        if dm.get("sent"):
            tee.note("master frames %d (%d again) = %.0f B per frame" %
                     (dm["sent"], dm.get("again", 0),
                      nbytes / dm["sent"] if dm["sent"] else 0))
        return 0
    finally:
        m.ser.close()
        s.ser.close()
        print("transcript: %s" % a.log)


if __name__ == "__main__":
    sys.exit(main())
