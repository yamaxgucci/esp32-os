#!/usr/bin/env python3
"""ArgonOS - the remote keyboard, end to end, against a guest in QEMU.

This plays both ends of KVM.AXE at once.  It is the phone: it opens the page,
opens the WebSocket, and sends the same lines a finger on the trackpad would.
And it is the PC at the far end of the cable: it connects to USBHIDVIRT.SYS and
reads the HID reports that come out, then says whether they are the reports
those events should have produced.

    python tools/kvmtest.py                     against a guest already running
    python tools/kvmtest.py --boot              start one, run, stop it

What it cannot check is the last inch: whether a real host agrees that these
bytes are a keyboard.  Nothing in an emulator can - QEMU models no OTG
peripheral at all - and that is what plugging the board into a PC is for.  What
it does check is everything above the port, which is where the bugs that look
like a bad cable actually live.

Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
"""

import argparse
import base64
import hashlib
import os
import re
import socket
import struct
import subprocess
import sys
import time

FRAME = 16
KIND_KBD, KIND_MOUSE, KIND_ABS = 1, 2, 3

RESULTS = []


def note(ok, name, detail=""):
    RESULTS.append((ok, name, detail))
    print(("PASS " if ok else "FAIL ") + name + (" - " + detail if detail else ""),
          flush=True)
    return ok


# --------------------------------------------------------------------------
# The console, for getting the guest to the point where there is anything to
# test: install the driver, start the application.
# --------------------------------------------------------------------------

class Console:
    def __init__(self, host, port, timeout=90):
        deadline = time.time() + timeout
        while True:
            try:
                self.s = socket.create_connection((host, port), 2.0)
                break
            except OSError:
                if time.time() >= deadline:
                    raise
                time.sleep(0.5)
        self.s.settimeout(0.4)
        self.buf = b""

    def wait(self, needle, seconds=45):
        end = time.time() + seconds
        target = needle.encode()
        while time.time() < end:
            if target in self.buf:
                return True
            try:
                chunk = self.s.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                return False
            self.buf += chunk
            # The console redraws whole screens; keep only the recent past.
            if len(self.buf) > 262144:
                self.buf = self.buf[-131072:]
        return target in self.buf

    def send(self, line):
        self.s.sendall(line.encode() + b"\r")
        time.sleep(0.3)

    def tail(self, chars=2500):
        """What the guest last said, with the screen painting taken out.

        The console is a rendered screen rather than a log, so this is only
        ever a hint - but when a check fails it is the difference between "the
        report never arrived" and "the application faulted".
        """
        t = self.buf.decode("latin1")
        t = re.sub("\x1b" + r"\[[0-9;?]*[A-Za-z]", "\n", t)
        t = re.sub(r"\n{2,}", "\n", t)
        # This ends up on a Windows console whose code page is not UTF-8, and a
        # transcript that cannot be printed is worse than a plain one.
        t = t.encode("ascii", "replace").decode("ascii")
        return t[-chars:]

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


# --------------------------------------------------------------------------
# The phone: a WebSocket client, hand-rolled so this needs nothing installed.
# --------------------------------------------------------------------------

class Phone:
    def __init__(self, host, port, path="/ws"):
        self.s = socket.create_connection((host, port), 5.0)
        self.s.settimeout(2.0)
        key = base64.b64encode(os.urandom(16)).decode()
        req = ("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n" % (path, host, port, key))
        self.s.sendall(req.encode())

        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.s.recv(1024)
            if not chunk:
                raise OSError("the guest closed the upgrade")
            head += chunk
        self.head = head.decode("latin1")
        if "101" not in self.head.split("\r\n")[0]:
            raise OSError("not an upgrade: %r" % self.head.split("\r\n")[0])

        want = base64.b64encode(hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        ).decode()
        self.accept_ok = want.lower() in self.head.lower()
        self.rx = head.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in head else b""

    def send(self, text, opcode=1):
        payload = text.encode()
        mask = os.urandom(4)
        n = len(payload)
        if n < 126:
            hdr = bytes([0x80 | opcode, 0x80 | n])
        else:
            hdr = bytes([0x80 | opcode, 0x80 | 126]) + struct.pack(">H", n)
        body = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.s.sendall(hdr + mask + body)

    def recv_frame(self, seconds=3.0):
        """One frame from the guest, which never masks.  (opcode, payload)."""
        end = time.time() + seconds
        while True:
            f = self._take()
            if f is not None:
                return f
            if time.time() >= end:
                return None
            try:
                chunk = self.s.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                return None
            self.rx += chunk

    def _take(self):
        b = self.rx
        if len(b) < 2:
            return None
        opcode = b[0] & 0x0F
        n = b[1] & 0x7F
        pos = 2
        if n == 126:
            if len(b) < 4:
                return None
            n = struct.unpack(">H", b[2:4])[0]
            pos = 4
        if b[1] & 0x80:  # a server must not mask; if it did, say so loudly
            raise OSError("the guest masked a frame")
        if len(b) < pos + n:
            return None
        payload = b[pos:pos + n]
        self.rx = b[pos + n:]
        return opcode, payload

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


# --------------------------------------------------------------------------
# The PC: whatever USBHIDVIRT forwards is what a host would have received.
# --------------------------------------------------------------------------

class Pc:
    def __init__(self, host, port, timeout=30):
        deadline = time.time() + timeout
        while True:
            try:
                self.s = socket.create_connection((host, port), 2.0)
                break
            except OSError:
                if time.time() >= deadline:
                    raise
                time.sleep(0.4)
        self.s.settimeout(0.3)
        self.buf = b""

    def frames(self, seconds=1.5, want=None):
        """Everything that arrives in `seconds`, or until `want` of them."""
        out = []
        end = time.time() + seconds
        while time.time() < end:
            while len(self.buf) >= FRAME:
                out.append(self.buf[:FRAME])
                self.buf = self.buf[FRAME:]
                if want is not None and len(out) >= want:
                    return out
            try:
                chunk = self.s.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            self.buf += chunk
        while len(self.buf) >= FRAME:
            out.append(self.buf[:FRAME])
            self.buf = self.buf[FRAME:]
        return out

    def drain(self):
        self.frames(0.4)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def kbd_keys(f):
    """The usage codes a keyboard frame says are held, and its modifiers."""
    rep = f[2:2 + f[1]]
    return rep[0], [b for b in rep[2:8] if b]


def http_get(host, port, path="/"):
    s = socket.create_connection((host, port), 5.0)
    s.settimeout(4.0)
    s.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n"
               % (path, host)).encode())
    data = b""
    while True:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    s.close()
    return data


# --------------------------------------------------------------------------

def run_checks(host, http_port, hid_port, picture=False):
    # The PC first.  USBHIDVIRT refuses writes with nothing connected - the
    # same answer the real device gives with the cable out - so the phone's
    # events would go nowhere if this were the other way round.
    pc = Pc(host, hid_port)
    note(True, "cable in", "connected to USBHIDVIRT on :%d" % hid_port)

    page = http_get(host, http_port, "/")
    note(page.startswith(b"HTTP/1.1 200"), "GET / is 200",
         page.split(b"\r\n")[0].decode("latin1", "replace"))
    note(b"ArgonOS KVM" in page, "the page is the KVM page")
    note(b"Content-Length:" in page, "the page has a length")

    missing = http_get(host, http_port, "/nope")
    note(missing.startswith(b"HTTP/1.1 404"), "an unknown path is 404",
         missing.split(b"\r\n")[0].decode("latin1", "replace"))

    phone = Phone(host, http_port)
    note("101" in phone.head.split("\r\n")[0], "the WebSocket upgrade is 101")
    note(phone.accept_ok, "Sec-WebSocket-Accept is the hash of our key")

    # The status push, which is also how the page shows whether a PC is there.
    #
    # Patiently, and skipping what is not it.  With an agent running there is a
    # stream of picture messages on this same socket and the status line is one
    # small text frame somewhere inside it.
    #
    got = None
    deadline = time.time() + 12.0
    while time.time() < deadline:
        f = phone.recv_frame(2.0)
        if f is None:
            continue
        if f[0] == 1 and f[1].startswith(b"S "):
            got = f[1].decode()
            break
    note(got is not None, "a status line arrives", got or "nothing in 12 s")
    if got is not None:
        note(got.split()[1] == "2", "the status says a PC is attached", got)

    pc.drain()

    # A key down, then up.  Two reports, and the second one has to be a report
    # that says nothing is held - not the absence of a report.
    phone.send("K d 4 0")
    f = pc.frames(2.0, want=1)
    ok = len(f) == 1 and f[0][0] == KIND_KBD and kbd_keys(f[0]) == (0, [4])
    note(ok, "a key down is one keyboard report",
         "" if ok else repr([x.hex() for x in f]))

    phone.send("K u 4 0")
    f = pc.frames(2.0, want=1)
    ok = len(f) == 1 and kbd_keys(f[0]) == (0, [])
    note(ok, "a key up releases it", "" if ok else repr([x.hex() for x in f]))

    # A chord: the modifier is in the report that carries the key, not in one
    # of its own, or the far machine sees the letter before the shift.
    phone.send("K d 4 2")
    f = pc.frames(2.0, want=1)
    ok = len(f) == 1 and kbd_keys(f[0]) == (2, [4])
    note(ok, "shift rides on the same report as the key",
         "" if ok else repr([x.hex() for x in f]))
    phone.send("K u 4 0")
    pc.frames(1.0, want=1)

    # The pointer.
    phone.send("M 5 -3 0 1")
    f = pc.frames(2.0, want=1)
    ok = (len(f) == 1 and f[0][0] == KIND_MOUSE and f[0][1] == 4 and
          f[0][2] == 1 and f[0][3] == 5 and f[0][4] == 0xFD)
    note(ok, "a relative move keeps its sign",
         "" if ok else repr([x.hex() for x in f]))

    phone.send("A 1000 2000 0 0")
    f = pc.frames(2.0, want=1)
    ok = (len(f) == 1 and f[0][0] == KIND_ABS and f[0][1] == 6 and
          f[0][3] == (1000 & 0xFF) and f[0][4] == (1000 >> 8) and
          f[0][5] == (2000 & 0xFF) and f[0][6] == (2000 >> 8))
    note(ok, "an absolute position arrives little-endian",
         "" if ok else repr([x.hex() for x in f]))

    # Typing: two reports per character, press and release, and the letters in
    # the order somebody meant them.
    phone.send("T hi there")
    f = pc.frames(4.0, want=18)
    typed = []
    for fr in f:
        if fr[0] != KIND_KBD:
            continue
        mods, keys = kbd_keys(fr)
        if not keys:
            continue
        u = keys[0]
        if 0x04 <= u <= 0x1D:
            ch = chr(ord("a") + u - 0x04)
            typed.append(ch.upper() if mods & 0x22 else ch)
        elif u == 0x2C:
            typed.append(" ")
    ok = "".join(typed) == "hi there"
    note(ok, "text is typed out character by character",
         "got %r from %d reports" % ("".join(typed), len(f)))

    # Typing while a key is held.  A report says what is held down, all of it,
    # so text typed on top of a held key must keep saying the key is held -
    # otherwise the far machine sees it released and this end never learns.
    phone.send("K d 65 0")          # 0x41, F8: arbitrary and easy to spot
    pc.frames(1.5, want=1)
    phone.send("T ab")
    f = pc.frames(3.0, want=4)
    ok = len(f) >= 4 and all(0x41 in kbd_keys(x)[1] for x in f if x[0] == KIND_KBD)
    note(ok, "typing does not drop a key that is held",
         "got %s" % repr([x.hex() for x in f]))
    phone.send("K u 65 0")
    f = pc.frames(1.5, want=1)
    ok = len(f) == 1 and kbd_keys(f[0]) == (0, [])
    note(ok, "and the held key can then be released",
         "" if ok else repr([x.hex() for x in f]))

    # Something held when the phone goes away.  This is the failure that a
    # person only notices when a key repeats forever on the far machine.
    phone.send("K d 4 1")
    pc.frames(1.5, want=1)
    phone.close()

    f = pc.frames(3.0, want=3)
    kinds = [x[0] for x in f]
    ok = KIND_KBD in kinds
    if ok:
        kb = [x for x in f if x[0] == KIND_KBD][-1]
        ok = kbd_keys(kb) == (0, [])
    note(ok, "a phone that vanishes releases everything",
         "got %s" % repr([x.hex() for x in f]))
    ok = KIND_MOUSE in kinds and KIND_ABS in kinds
    note(ok, "and both pointers' buttons with it",
         "kinds %s" % kinds)

    pc.close()

    if picture:
        phone = Phone(host, http_port)
        check_picture(phone)
        phone.close()


def png_size(b):
    """Width and height out of a PNG's IHDR, or None if it is not one."""
    if len(b) < 24 or b[:8] != b"\x89PNG\r\n\x1a\n" or b[12:16] != b"IHDR":
        return None
    return (struct.unpack(">I", b[16:20])[0],
            struct.unpack(">I", b[20:24])[0])


def check_picture(phone, seconds=12.0):
    """The screen of the far machine, as the phone would receive it.

    This is the half of the system the keyboard tests cannot reach, and it is
    where the interesting failure lives: the board forwards messages it never
    looks inside, so a framing mistake does not corrupt a pixel - it stops the
    picture entirely and leaves a resync counter climbing.  That is what these
    numbers are for.
    """
    hello = None
    tiles = 0
    bad = 0
    covered = 0
    end = time.time() + seconds

    # Ask for a whole screen, so this does not depend on anything moving over
    # there while the test runs.
    phone.send(bytes([65, 71, 0x11, 0, 0, 0, 0, 0]).decode("latin1"), opcode=2)

    while time.time() < end:
        f = phone.recv_frame(2.0)
        if f is None:
            continue
        op, payload = f
        if op != 2:
            continue
        if len(payload) < 8 or payload[0:2] != b"AG":
            bad += 1
            continue
        t = payload[2]
        n = struct.unpack("<I", payload[4:8])[0]
        body = payload[8:8 + n]
        if t == 0x01 and len(body) >= 8:
            hello = (struct.unpack("<H", body[2:4])[0],
                     struct.unpack("<H", body[4:6])[0],
                     struct.unpack("<H", body[6:8])[0])
        elif t == 0x02 and len(body) >= 8:
            x, y, w, h = struct.unpack("<HHHH", body[0:8])
            size = png_size(body[8:])
            if size is None or size != (w, h):
                bad += 1
            else:
                covered += w * h
            tiles += 1
            if hello is not None and covered >= hello[0] * hello[1]:
                break

    note(hello is not None, "the agent says how big its screen is",
         "%r" % (hello,))
    note(tiles > 0, "tiles arrive", "%d" % tiles)
    note(bad == 0, "every forwarded message is a framed PNG of its rectangle",
         "%d bad" % bad)
    if hello is not None:
        want = hello[0] * hello[1]
        note(covered >= want,
             "a whole screen arrives when the phone asks for one",
             "%d of %d pixels" % (covered, want))
    return hello is not None and tiles > 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--http", type=int, default=8080,
                    help="host port forwarded to the guest's :80")
    ap.add_argument("--hid", type=int, default=5562)
    ap.add_argument("--console", type=int, default=5556)
    ap.add_argument("--no-setup", action="store_true",
                    help="the drivers are installed and the app is running")
    ap.add_argument("--agent", default=None,
                    help="path to kvmagent.exe; starting it turns on the "
                         "picture checks")
    args = ap.parse_args()

    con = None
    if not args.no_setup:
        con = Console(args.host, args.console)
        if not con.wait("Type 'help'", 60):
            print("the guest never reached a prompt", file=sys.stderr)
            return 2
        con.wait(r"C:\>", 10)
        con.send(r"drv install h:\usbhidvirt.sys")
        if not con.wait("usbhidvirt: listen", 20):
            print("USBHIDVIRT did not install", file=sys.stderr)
            return 2
        con.send(r"drv install h:\usbcdcvirt.sys")
        if not con.wait("usbcdcvirt: listen", 20):
            print("USBCDCVIRT did not install", file=sys.stderr)
            return 2
        con.send(r"run h:\kvm.axe")
        if not con.wait("open http", 20):
            print("KVM.AXE did not start", file=sys.stderr)
            return 2

    agent = None
    if args.agent is not None:
        # After the setup, never before: it connects to a listener the guest
        # only opens once USBCDCVIRT is loaded.
        agent = subprocess.Popen(
            [args.agent, "--tcp", "%s:5563" % args.host, "--fps", "5"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)

    try:
        run_checks(args.host, args.http, args.hid, agent is not None)
    except Exception as exc:  # noqa: BLE001 - the transcript matters more
        note(False, "the run finished", repr(exc))
        # The usual cause, and it is not a bug: KVM.AXE keeps one phone at a
        # time and the newest wins, so a browser left open on the page
        # reconnects every second and takes the socket away from this.
        print("if the guest's console says 'phone gone (replaced)', close any "
              "browser tab open on the page and run this again", file=sys.stderr)
    finally:
        if agent is not None:
            agent.terminate()
        bad = [n for ok, n, _ in RESULTS if not ok]
        if con is not None:
            if bad:
                con.wait("~never~", 1)  # one more read of the console
                print("\n--- the guest's console ---\n" + con.tail(),
                      flush=True)
            con.close()

    bad = [n for ok, n, _ in RESULTS if not ok]
    print("\n%d checks, %d failed" % (len(RESULTS), len(bad)), flush=True)
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
