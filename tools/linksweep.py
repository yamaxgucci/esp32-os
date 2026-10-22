#!/usr/bin/env python3
"""Sweep the two-board wire: one block size after another, both consoles at once.

`link` measures one run.  The question that needs answering is a comparison -
whether a line fast enough to make mistakes beats a slower line that makes
none - and that is several runs whose conditions have to be identical, which
is not something to do by hand at two terminals.

    python tools/linksweep.py --sender COM4 --receiver COM3 \
        --baud 8000000 --kb 8192 --blocks 68,132,260,516,1028

Both boards must be sitting at a shell prompt in a user slot with LINK.AXE on
their current drive.  Nothing here flashes or reboots anything: it types the
same two commands a person would, and reads the numbers back.

The figure to compare is `good` - the payload of the blocks that arrived
intact, per second.  `bps` is everything that came off the wire including the
headers and the wreckage, and a line can raise it while delivering less.

Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
"""
import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("linksweep: pyserial is missing - run this with the IDF python.")

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def open_board(port, baud=115200):
    ser = serial.Serial(port, baud, timeout=0.02, xonxoff=False,
                        write_timeout=10)
    # The console is a screen, not a stream: it positions the cursor, and DTR
    # or RTS left asserted resets an ESP32 the moment the port opens.
    try:
        ser.rts = False
        ser.dtr = False
    except Exception:
        pass
    return ser


def settle(ser, seconds=1.0):
    """Get back to a prompt: stop whatever is running, take slot 1, drain."""
    ser.write(b"\x03")            # Ctrl+C - stops an app that reads keys
    time.sleep(0.2)
    ser.write(b"\x1b1")           # Alt+1 - a user slot, not the system shell
    time.sleep(0.2)
    ser.write(b"\r\n")
    end = time.time() + seconds
    while time.time() < end:
        ser.read(4096)


def clean(raw):
    """The console's screen paint, back into the lines that were printed."""
    text = ANSI.sub("\n", raw.decode("utf-8", "replace"))
    out, seen = [], set()
    for line in text.split("\n"):
        line = line.strip()
        if line and line not in seen:
            seen.add(line)
            out.append(line)
    return out


def numbers(lines, prefix):
    """Every key=value on the lines that start with prefix, as one dict."""
    got = {}
    for line in lines:
        if not line.startswith(prefix):
            continue
        for k, v in re.findall(r"([a-z]+)=(\d+)", line):
            got[k] = int(v)
    return got


def one_run(tx, rx, baud, kb, block, verbose=False):
    """One block size: start the receiver, push at it, collect both ends."""
    # The receiver has to outlive the transfer, and a run that ends early
    # reports as loss.  Half again on top of the ideal time, plus a floor.
    ideal = (kb * 1024.0) / (baud / 10.0)
    secs = int(ideal * 1.5) + 6

    # Clear both screens first.  The console is a screen that repaints itself,
    # so anything still on it comes back down the wire during the next run and
    # is indistinguishable from this run's output - which is how a sweep once
    # reported the previous block size's numbers three times in a row.
    for ser in (tx, rx):
        ser.write(b"cls\r\n")
    time.sleep(0.6)
    tx.reset_input_buffer()
    rx.reset_input_buffer()
    rx.write(("link recv %d %d %d\r\n" % (baud, secs, block)).encode())
    time.sleep(1.0)
    tx.write(("link send %d %d %d\r\n" % (baud, kb, block)).encode())

    tx_raw, rx_raw = bytearray(), bytearray()
    deadline = time.time() + secs + 20
    tx_done = rx_done = False
    while time.time() < deadline and not (tx_done and rx_done):
        tx_raw += tx.read(8192)
        rx_raw += rx.read(8192)
        if b"tx bytes=" in tx_raw:
            tx_done = True
        if b"rx bytes=" in rx_raw:
            rx_done = True
        time.sleep(0.02)

    tx_lines, rx_lines = clean(tx_raw), clean(rx_raw)
    if verbose:
        for line in tx_lines:
            print("   tx| " + line)
        for line in rx_lines:
            print("   rx| " + line)
    sent = numbers(tx_lines, "tx ")
    got = numbers(rx_lines, "rx ")
    return sent, got


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sender", required=True, help="console port of the board that sends")
    ap.add_argument("--receiver", required=True, help="console port of the board that listens")
    ap.add_argument("--baud", type=int, default=8000000)
    ap.add_argument("--kb", type=int, default=4096, help="kilobytes per run")
    ap.add_argument("--blocks", default="68,132,260,516,1028",
                    help="block sizes on the wire, comma separated")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--pre", default=None,
                    help="a command to run on both boards first, e.g. 'wifi off'")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    blocks = [int(b) for b in args.blocks.split(",") if b.strip()]
    tx = open_board(args.sender)
    rx = open_board(args.receiver)
    settle(tx)
    settle(rx)
    if args.pre:
        for ser in (tx, rx):
            ser.write((args.pre + "\r\n").encode())
        time.sleep(2.0)
        for ser in (tx, rx):
            ser.reset_input_buffer()

    print("%d baud, %d KB per run, %s" % (args.baud, args.kb,
                                          time.strftime("%Y-%m-%d %H:%M")))
    print("%5s %8s %8s %7s %5s %5s %6s %8s %8s %6s"
          % ("block", "sent", "arrived", "ok", "bad", "lost", "resync",
             "tx bps", "good", "loss%"))
    rows = []
    for _ in range(args.repeat):
        for block in blocks:
            sent, got = one_run(tx, rx, args.baud, args.kb, block, args.verbose)
            payload = block - 12
            blocks_sent = -(-(args.kb * 1024) // block)
            ok = got.get("ok", 0)
            bad = got.get("bad", 0)
            lost = got.get("lost", 0)
            missing = blocks_sent - ok
            loss = (100.0 * missing / blocks_sent) if blocks_sent else 0.0
            # `arrived` against `sent` is the byte deficit: what the line
            # (or the receiver) swallowed whole, as opposed to mangled.  A
            # corrupt block still arrives and is counted here.
            print("%5d %8d %8d %7d %5d %5d %6d %8d %8d %6.2f"
                  % (block, sent.get("bytes", 0), got.get("bytes", 0), ok,
                     bad, lost, got.get("resync", 0), sent.get("bps", 0),
                     got.get("good", 0), loss))
            sys.stdout.flush()
            rows.append((block, ok, bad, lost, got.get("good", 0)))
            time.sleep(1.0)

    tx.close()
    rx.close()
    if rows:
        best = max(rows, key=lambda r: r[4])
        print("best payload rate: %d B/s at a block of %d" % (best[4], best[0]))


main()
