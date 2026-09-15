#!/usr/bin/env python3
"""Check the phone link end to end, with one board testing the other.

  python tools/phonecheck.py --client COM4 --server COM7 --pass argon

The client board runs PHONECL.AXE against the server board's PHONE.SYS over
Wi-Fi; this script drives the client's console, reads back what it saw, and
says which of the scenarios passed.  Nothing here touches a phone.

Why it exists: every fault in this link was found by asking a person to hold a
phone and press a key again - five of them in one day, none where they looked.
"The page does not load" arrives hours after the state that caused it, and
"switching slots does not work" cannot be repeated on demand.  A scenario that
is a script can run after every build; a scenario that is a favour runs when
somebody is awake.

What it cannot judge: how the page looks, and whether it can be used with a
thumb.  That stays a person's job, and no amount of this replaces it.
"""
import argparse
import re
import sys
import time

import serial


def open_port(name):
    """Without touching DTR/RTS, which on an ESP32 are reset and boot select."""
    s = serial.Serial()
    s.port = name
    s.baudrate = 115200
    s.timeout = 0.4
    s.dtr = False
    s.rts = False
    s.open()
    return s


class Board:
    def __init__(self, port):
        self.s = open_port(port)
        self.log = bytearray()

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.log.extend(self.s.read(4096))

    def send(self, text, wait):
        self.s.write((text + "\r").encode())
        self.s.flush()
        self.pump(wait)

    def user_slot(self):
        """Alt+1.  `run` refuses from the system shell, and the shell is where
        a board ends up after anything that used Ctrl+backslash."""
        self.s.write(b"\x1b1")
        self.s.flush()
        self.pump(1.5)
        self.send("", 0.5)

    def text(self):
        t = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", self.log.decode("latin1"))
        return t.replace("\r", "\n")

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


class Report:
    def __init__(self):
        self.rows = []

    def check(self, name, ok, detail=""):
        self.rows.append((name, bool(ok), detail))
        print("  %-28s %s%s" % (name, "pass" if ok else "FAIL",
                                ("  " + detail) if detail else ""))

    def done(self):
        bad = [r for r in self.rows if not r[1]]
        print("\n%u checks, %u failed" % (len(self.rows), len(bad)))
        return 1 if bad else 0


def run_client(cl, args, actions, wait):
    """One PHONECL run, returning everything it printed."""
    before = len(cl.log)
    cl.send("run c:\\phonecl.axe %s -pass %s %s" % (args.ip, args.password,
                                                    actions), wait)
    return cl.text()[len(cl.text()) - (len(cl.log) - before) - 400:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--client", required=True, help="serial port of the board "
                                                    "running PHONECL.AXE")
    ap.add_argument("--server", help="serial port of the board under test, "
                                     "read for its journal")
    ap.add_argument("--ip", default="192.168.4.1")
    ap.add_argument("--password", default="argon")
    ap.add_argument("--hold", type=int, default=0,
                    help="also sit on the link this many seconds")
    args = ap.parse_args()

    rep = Report()
    cl = Board(args.client)
    sv = Board(args.server) if args.server else None

    try:
        cl.user_slot()

        print("the page:")
        out = run_client(cl, args, "-get", 25)
        rep.check("http answers 200", "200 OK" in out)
        m = re.search(r"page: (\d+) bytes of body", out)
        rep.check("page arrives whole", bool(m) and int(m.group(1)) > 8000,
                  m.group(1) + " bytes" if m else "no body line")
        rep.check("websocket upgrades", "linked to" in out)

        print("the console:")
        out = run_client(cl, args, "-wait 800 -screen", 30)
        rep.check("console arrives", "---" in out and "rows received" in out)
        m = re.search(r"--- (\d+)x(\d+)", out)
        rep.check("geometry looks sane",
                  bool(m) and 20 <= int(m.group(1)) <= 200
                  and 10 <= int(m.group(2)) <= 80,
                  ("%sx%s" % m.groups()) if m else "no geometry")

        print("typing:")
        out = run_client(cl, args, '-type "ver" -enter -wait 1200 -screen', 35)
        rep.check("a typed command runs", "ArgonOS" in out,
                  "looking for the version line the shell prints")

        print("chords and the pointer:")
        out = run_client(cl, args, "-key alt+2 -wait 600 -key alt+1 "
                                   "-right 100,60 -wait 400", 30)
        rep.check("client sent the chords", "linked to" in out)
        if sv:
            sv.pump(0.5)
            sv.send("log -n 20", 6)
            j = sv.text()
            rep.check("board saw the right button",
                      "right button at 100,60" in j)
            rep.check("board switched slots",
                      "enter_shell_view slot" in j or "focus" in j)

        if args.hold:
            print("staying connected:")
            out = run_client(cl, args, "-hold %u" % args.hold, args.hold + 25)
            rep.check("link survives %u s" % args.hold, "held" in out,
                      "lost" if "link lost" in out else "")

    finally:
        cl.close()
        if sv:
            sv.close()

    return rep.done()


if __name__ == "__main__":
    sys.exit(main())
