#!/usr/bin/env python3
"""The network on the real board, checked the way `argon nettest` checks QEMU.

    python tools/boardnet.py -p COM3

Drives the board over its serial console and, for the half QEMU cannot answer,
uses this PC as the other end of the Wi-Fi:

  the board as a client   an address from DHCP, a name resolved, a page fetched
                          over HTTP, and a file fetched over FTP - two
                          connections at once, and PASV through a router
  the board as a server   `httpd` on the board, this PC asking it for the file
                          it just fetched, compared byte for byte

Both halves need the internet, because a server on *this* PC is behind the
Windows firewall and adding a rule needs an administrator: outbound from the
board and outbound from the PC are the two directions that always work.  Pass
--host-http to use a server on this machine instead, once that hole is open.

Nothing here is a substitute for `argon nettest`; it answers the questions the
emulator cannot: the radio, and the numbers.

Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
"""

from __future__ import annotations

import argparse
import re
import socket
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    sys.exit("boardnet: pyserial is missing.  Run this with the IDF python "
             "(argon env), which has it because esptool needs it.")

PROMPT = re.compile(r"\(\d\)\s[A-Z]:\\>")

# A page that is plain HTTP, small, and has been up longer than this project.
PAGE_URL = "http://example.com/"
# Plain FTP, anonymous, and a file small enough to be quick.
FTP_URL = "ftp://ftp.gnu.org/gnu/hello/hello-2.10.tar.gz.sig"
# Plain HTTP and a few hundred kilobytes: enough to measure with.
BIG_URL = "http://ftp.gnu.org/gnu/hello/hello-2.10.tar.gz"


class Board:
    def __init__(self, port: str, baud: int = 115200, quiet: bool = False):
        self.p = serial.Serial(port, baud, timeout=0.2)
        self.text = ""
        self.quiet = quiet

    def reset(self) -> None:
        """EN low through RTS, the way esptool does it."""
        self.p.setDTR(False)
        self.p.setRTS(True)
        time.sleep(0.15)
        self.p.setRTS(False)

    def pump(self, seconds: float) -> str:
        end = time.time() + seconds
        got = ""
        while time.time() < end:
            data = self.p.read(4096)
            if data:
                s = data.decode("latin-1", "replace")
                got += s
                self.text += s
                if not self.quiet:
                    sys.stdout.write(s)
                    sys.stdout.flush()
        return got

    def wait_prompt(self, seconds: float) -> bool:
        end = time.time() + seconds
        while time.time() < end:
            self.pump(0.2)
            if PROMPT.search(self.text[-400:]):
                return True
        return False

    def send(self, line: str, wait: float = 10.0, until: str = "") -> str:
        """One command; returns everything it printed.

        Finished means a prompt *after* this command's own echo.  Any prompt
        will not do: the one from the last command is still on the screen, and
        every redraw puts it back - which reads as a command that printed
        nothing at all.
        """
        # Clear the screen first.  The console is a screen, not a log: every
        # scroll redraws what is already on it, so the last command's "saved"
        # is still there to be matched by this one.  `cls` makes what comes
        # next the only thing on the screen, which is what makes this readable
        # by a machine at all.
        self.p.write(b"cls\r")
        self.pump(0.6)

        self.text = ""
        self.p.write((line + "\r").encode())
        end = time.time() + wait
        tail_of_command = line[-12:]
        while time.time() < end:
            self.pump(0.2)
            seen = strip(self.text)
            if until:
                if until in seen:
                    break
                continue
            at = seen.rfind(tail_of_command)
            if at >= 0 and PROMPT.search(seen[at + len(tail_of_command):]):
                break
        return strip(self.text)


def strip(s: str) -> str:
    """The transcript is a rendered screen; this is the text on it."""
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "\n", s)


def ask(ip: str, port: int, path: str, timeout: float = 20.0) -> bytes:
    with socket.create_connection((ip, port), timeout=timeout) as s:
        s.settimeout(timeout)
        s.sendall(("GET %s HTTP/1.0\r\nHost: argon\r\n\r\n" % path).encode())
        out = bytearray()
        while True:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            out += chunk
    return bytes(out)


def fetch_page(url: str) -> bytes:
    """The same page the board fetched, from here, for a byte comparison."""
    host = url.split("//", 1)[1].split("/", 1)[0]
    path = "/" + url.split("//", 1)[1].split("/", 1)[1]
    with socket.create_connection((host, 80), timeout=20) as s:
        s.sendall(("GET %s HTTP/1.0\r\nHost: %s\r\n\r\n" % (path, host)).encode())
        out = bytearray()
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            out += chunk
    at = out.find(b"\r\n\r\n")
    return bytes(out[at + 4:]) if at >= 0 else bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-p", "--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--serve-port", type=int, default=8080)
    ap.add_argument("--quiet", action="store_true", help="do not echo the wire")
    ap.add_argument("--big", action="store_true",
                    help="also fetch ~700 KB onto the card, for a speed number")
    args = ap.parse_args()

    results: list[str] = []

    def note(ok: bool, name: str, detail: str = "") -> None:
        line = ("PASS " if ok else "FAIL ") + name + (" - " + detail if detail else "")
        results.append(line)
        print(line, flush=True)

    b = Board(args.port, args.baud, args.quiet)
    b.reset()
    if not b.wait_prompt(20):
        note(False, "boot", "no prompt in twenty seconds")
        return 1
    note(True, "boot")

    out = b.send("net wait", wait=40, until="address ")
    m = re.search(r"address (\d+\.\d+\.\d+\.\d+)", out)
    note(m is not None, "dhcp-address", m.group(1) if m else out.strip()[-60:])
    if m is None:
        return 1
    board_ip = m.group(1)

    out = b.send("net resolve example.com", wait=30, until="is ")
    note("example.com is" in out, "dns")

    out = b.send("wget " + PAGE_URL + " t:\\page.htm", wait=40, until="saved ")
    note("saved" in out, "http-fetch", one_line(out, "bytes in"))

    out = b.send("type t:\\page.htm", wait=20)
    note("</html>" in out.replace("\n", "").replace(" ", "")
         or "html" in out, "http-content")

    out = b.send("wget " + FTP_URL + " t:\\h.sig", wait=60, until="saved ")
    note("saved" in out, "ftp-fetch", one_line(out, "bytes in"))
    note("227 " in out, "ftp-passive", one_line(out, "227 "))

    if args.big:
        # The RAM disk on this board has about thirty kilobytes free, so a
        # transfer worth timing has to land on the card - which is also the
        # only way to see whether FAT keeps up with the radio.
        out = b.send("wget " + BIG_URL + " a:\\big.tmp", wait=240,
                     until="saved ")
        note("saved" in out, "http-big-fetch", one_line(out, "bytes in"))
        b.send("del a:\\big.tmp", wait=30)

    sizes = b.send("dir t:\\", wait=20)
    m = re.search(r"page\.htm\s+(\d+)", sizes)
    served_len = int(m.group(1)) if m else 0
    note(served_len > 0, "ram-disk-holds-it", "%d bytes" % served_len)

    # ---- the board as the server ----------------------------------------
    b.text = ""
    b.p.write(("httpd %d t:\\\r" % args.serve_port).encode())
    b.pump(3.0)
    note("Ctrl+C to stop" in strip(b.text), "httpd-started")

    # Three attempts, because the first one after a long download has been seen
    # to time out on this network while the next one works - see the note in
    # docs/09-esp32-cyd.md.  A retry is worth recording, not hiding.
    reply = b""
    attempts = 0
    for attempt in range(3):
        attempts = attempt + 1
        try:
            reply = ask(board_ip, args.serve_port, "/page.htm", timeout=10)
            break
        except OSError as exc:
            if attempt == 2:
                note(False, "httpd-reachable", str(exc))
            else:
                time.sleep(5)
    if reply and attempts > 1:
        print("      (reachable on attempt %d)" % attempts, flush=True)
    if reply:
        note(reply.startswith(b"HTTP/1.1 200"), "httpd-status",
             reply.split(b"\r\n")[0].decode("latin-1"))
        at = reply.find(b"\r\n\r\n")
        body = reply[at + 4:] if at >= 0 else b""
        note(len(body) == served_len, "httpd-length",
             "%d served, %d on the card" % (len(body), served_len))
        try:
            note(body == fetch_page(PAGE_URL), "httpd-bytes-match")
        except OSError as exc:
            print("SKIP httpd-bytes-match - %s" % exc, flush=True)

    b.pump(1.0)
    b.p.write(b"\x03")
    out = strip(b.pump(4.0))
    note("stopped" in out, "httpd-stops-on-ctrl-c")

    # ---- what it costs ---------------------------------------------------
    mem = b.send("mem", wait=15)
    m = re.search(r"internal\s+(\d+)K\s+(\d+)K\s+(\d+)K", mem)
    if m:
        print("memory with the radio up: %sK total, %sK free, %sK largest"
              % m.groups(), flush=True)

    b.send("del t:\\page.htm", wait=15)
    b.send("del t:\\h.sig", wait=15)

    bad = [r for r in results if r.startswith("FAIL")]
    print("\n%d checks, %d failed" % (len(results), len(bad)), flush=True)
    for r in bad:
        print("  " + r, flush=True)
    return 1 if bad else 0


def one_line(text: str, needle: str) -> str:
    for line in text.split("\n"):
        if needle in line:
            return line.strip()
    return ""


if __name__ == "__main__":
    sys.exit(main())
