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


def note_if_unlinked(rep, out, what):
    """A run that never connected fails every check made of it, and the
    failure says nothing about the thing being checked.  Say which it was."""
    if "linked to" not in out:
        why = "connect failed" if "connect failed" in out else "no link"
        rep.check(what + " (the client never connected)", False, why)
        return False
    return True


def run_client(cl, args, actions, wait):
    """One PHONECL run, returning everything it printed.

    With a pause first.  Runs back to back had the next connection arrive
    while the board was still finishing with the last one, and it came back
    "connect failed: -5" - which made the console checks fail in a way that
    looked exactly like the console being broken.  Three seconds is not a
    measurement, it is politeness; a person with a phone never reconnects this
    fast, and if this ever becomes a real complaint it is the board's to fix,
    not this script's to hide.
    """
    # A breath between runs, and no more than that.
    #
    # This was five seconds on the theory that reconnecting too fast was being
    # refused - "connect failed: -5" kept appearing.  Measured afterwards: the
    # board accepts a new connection immediately, with no gap at all, four
    # times out of four.  The refusals were the memory floor, from a leftover
    # application, and pacing never had anything to do with it.
    time.sleep(2)
    # Measured on the cleaned text, not on the raw log: the two have
    # different lengths (escape sequences are stripped), so slicing one by
    # the other's length walks off by however much the board redrew - which
    # showed up as console checks failing while the console plainly worked.
    before = len(cl.text())
    cl.send("run c:\\phonecl.axe %s -pass %s %s" % (args.ip, args.password,
                                                    actions), wait)
    return cl.text()[before:]


def tidy(sv):
    """Leave the board with nothing running, and say so if it took work.

    By pid: "kill last app" (Ctrl+backslash twice) does not take the file
    manager - tried repeatedly - and an application left running holds memory
    the driver needs, so every check after it fails with "connect failed: -5"
    for reasons unrelated to the link.  That sequence has cost time four times
    now, which is why this is noisy about what it finds.
    """
    if sv is None:
        return
    sv.s.write(b"\x1c")           # the system shell, where kill lives
    sv.s.flush()
    sv.pump(1.5)
    sv.send("", 0.5)

    for attempt in range(5):
        before = len(sv.text())
        sv.send("ps", 3)
        out = sv.text()[before:]
        if "no applications loaded" in out:
            return
        # No anchors: the console redraws with cursor moves rather than
        # newlines, so `ps` arrives as one long line.
        pids = sorted(set(re.findall(
            r"(\d+)\s+[A-Z][A-Z0-9_.]*\s+(?:running|ready|loading)", out)))
        if not pids:
            return
        print("  (stopping %s left over from an earlier run)" % ", ".join(pids))
        for pid in pids:
            sv.send("kill " + pid, 3)
    print("  (warning: the board still has something running)")


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

        # Start from a board that has just booted.
        #
        # Tidying by pid works and was still not enough: a run that died
        #half way leaves applications, slots and a client socket behind, and
        # chasing those states one at a time cost more time than the ten
        # seconds a reset costs.  The board under test is a test fixture for
        # the length of this script; it can be restarted.
        if sv:
            print("(resetting the board under test)")
            sv.s.rts = True
            time.sleep(0.15)
            sv.s.rts = False
            sv.pump(10)
            sv.send("", 1)

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

        print("an application, over the link:")
        # Asked of the client, not of its output: printing a whole screen
        # scrolls the client's console and a word can be lost in the scroll,
        # which failed this check three times while the application was there.
        out = run_client(cl, args,
                         '-type "fm" -enter -wait 2500 -find "file manager"', 40)
        rep.check("application appears", "found file manager: yes" in out,
                  "the file manager names itself on its bottom line")
        out = run_client(cl, args, "-key ctrl+c -wait 1500", 30)
        if sv:
            sv.pump(0.5)
            sv.send("log -n 12", 6)
            j = sv.text()
            rep.check("Ctrl+C reaches the board", "chord: hid 6, mods 2" in j)
            rep.check("the application stops", "asked to stop" in j)
            rep.check("nothing left running",
                      "unbind" in j or "returned 0" in j)

        print("joining while something is running:")
        # Two different questions, and only one of them has a good answer on a
        # board this size.
        #
        # A client that is already connected keeps working while an
        # application runs - that is the fix from 15 Sep and it is checked
        # above.  A *new* client arriving afterwards is another matter: the
        # file manager leaves about five kilobytes as the largest free block,
        # and the driver refuses visitors below sixteen because accepting one
        # with no memory aborts the board.  So this measures rather than
        # demands, and says plainly when the board is too full to be joined.
        run_client(cl, args, '-type "fm" -enter -wait 2000', 35)
        if sv:
            sv.s.write(b"\x1c")
            sv.s.flush()
            sv.pump(1.5)
            sv.send("", 0.5)
            before = len(sv.text())
            sv.send("mem", 2.5)
            m = re.search(r"internal +\d+K +(\d+)K +(\d+)K", sv.text()[before:])
            largest = int(m.group(2)) if m else 0
            print("  (with the file manager up: largest free block %u KB)"
                  % largest)
            if largest >= 16:
                out = run_client(cl, args, "-get", 30)
                rep.check("a new client can still join", "200 OK" in out)
            else:
                print("  (too full for a new client, which is expected here "
                      "and not a fault)")
        tidy(sv)

        print("the repair sweep:")
        out = run_client(cl, args, "-hold 12", 40)
        m = re.search(r"held \d+ s, (\d+) rows arrived", out)
        rep.check("an idle screen still repairs itself",
                  bool(m) and int(m.group(1)) >= 20,
                  (m.group(1) + " rows in 12 s") if m else "no hold line")

        if args.hold:
            print("staying connected:")
            out = run_client(cl, args, "-hold %u" % args.hold, args.hold + 25)
            rep.check("link survives %u s" % args.hold, "held" in out,
                      "lost" if "link lost" in out else "")

    finally:
        tidy(sv)
        cl.close()
        if sv:
            sv.close()

    return rep.done()


if __name__ == "__main__":
    sys.exit(main())
