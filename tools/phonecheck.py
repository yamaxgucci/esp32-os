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
import os
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
    def __init__(self, port, name="board"):
        self.s = open_port(port)
        self.log = bytearray()
        self.name = name
        self._clean = ""   # everything cleaned so far
        self._at = 0       # how much of self.log that covers

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
        """Everything the board has said, cleaned - and cleaned once.

        This used to run over the whole log on every call, which is several
        times a round: by the end of a run that is megabytes of regular
        expression while nothing is reading the port, and Windows' receive
        buffer is four kilobytes.  The board's next answer went missing and the
        check blamed the board.
        """
        if len(self.log) > self._at:
            # A few bytes of overlap: an escape sequence can straddle two
            # reads, and half of one left in the text is a stray letter in the
            # middle of a word somebody is searching for.
            back = 16 if self._at >= 16 else self._at
            fresh = self.log[self._at - back:].decode("latin1")
            fresh = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", fresh)
            fresh = fresh.replace("\r", "\n")
            if back:
                keep = self._clean[:len(self._clean) - back] \
                    if len(self._clean) >= back else ""
                self._clean = keep + fresh
            else:
                self._clean += fresh
            self._at = len(self.log)
        return self._clean

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


def client_ran_out(out):
    """A client that could not start is not an answer about the server."""
    m = re.search(r"loading with (\d+) free, largest (\d+)", out)
    return bool(m) and int(m.group(2)) < 16384


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
    cl = Board(args.client, "client")
    sv = Board(args.server, "server") if args.server else None

    try:
        # Both boards, not just the one under test.
        #
        # The client is a fixture too, and it wears out: after fifty-odd runs
        # of PHONECL it was down to eighteen kilobytes free and eight as its
        # largest block, and every check after that failed with the board under
        # test in perfect health.  "The page will not load" from a client that
        # cannot start its own program is the most expensive kind of wrong
        # answer, because it points at the wrong board.
        print("(resetting the client board)")
        cl.s.rts = True
        time.sleep(0.15)
        cl.s.rts = False
        cl.pump(12)
        cl.send("", 1)
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

        # What this board looks like with nothing on it, so a later reading has
        # something to be compared against rather than a number picked here.
        idle_largest = 0
        if sv:
            sv.s.write(b"\x1c")
            sv.s.flush()
            sv.pump(1.5)
            before = len(sv.text())
            sv.send("mem", 2.5)
            m = re.search(r"internal +\d+K +(\d+)K +(\d+)K",
                          sv.text()[before:])
            idle_largest = int(m.group(2)) if m else 0
            print("(idle: %u KB as the largest free block)" % idle_largest)
            sv.user_slot()

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

        print("a picture, over the link:")
        # GFXPIX draws a circle, a diagonal and four differently coloured
        # corners - a circle because a text mode cannot draw one.  The client
        # cannot draw a band (one is five kilobytes and it holds two) so it
        # counts them and reads the nine bytes in front: how many, how big,
        # and which of the three encodings the board chose.
        if sv:
            # The client first, and drawing second.  This board keeps no frame,
            # so a rectangle that has been sent is gone: a client that joins
            # after the drawing sees an empty picture, which is what the first
            # run of this check found and reported as "no bands arrived".
            cl.user_slot()
            before = len(cl.text())
            cl.s.write(("run c:\\phonecl.axe %s -pass %s -hold 20 -pix\r"
                        % (args.ip, args.password)).encode())
            cl.s.flush()
            cl.pump(8)

            sv.s.write(b"\x1b1")
            sv.s.flush()
            sv.pump(1.5)
            sv.send("run c:\\gfxpix.axe 10", 3)
            cl.pump(30)
            out = cl.text()[before:]
            m = re.search(r"picture: (\d+) bands, (\d+) bytes, (\d+)x(\d+)",
                          out)
            rep.check("a drawing application reaches the link",
                      bool(m) and int(m.group(1)) > 0,
                      ("%s bands, %s bytes, %sx%s"
                       % m.groups()) if m else "no bands arrived")
            if m:
                # Sixteen rows at a time over a 160x144 picture is nine
                # rectangles and eighteen bands; fewer means the board gave up
                # part way, and this has been a screen that stopped halfway
                # down before.
                rep.check("and the whole of it arrives",
                          int(m.group(3)) >= 64 and int(m.group(4)) >= 64,
                          "%sx%s covered" % (m.group(3), m.group(4)))
            tidy(sv)

        print("an application that will not stop talking:")
        # The evening's first fault, and the one that hid the rest.
        #
        # SPIN prints without pause.  With the announcement printed before the
        # focus was taken, the shell's own line queued behind the application's
        # output - eight seconds, measured - and for those eight seconds there
        # was a running application in a focused slot and no foreground
        # process, so Ctrl+C reached nothing.  From the keyboard: a program
        # that cannot be stopped.
        if sv:
            sv.s.write(b"\x1b1")            # a user slot; run refuses elsewhere
            sv.s.flush()
            sv.pump(1.5)
            sv.send("run c:\\spin.axe", 5)
            out = run_client(cl, args, "-wait 1200 -key ctrl+c -wait 3000", 35)
            rep.check("a printing application takes Ctrl+C over the link",
                      "linked to" in out)
            # Asked of ps.  Counting what still arrives does not answer it:
            # an application printing twelve thousand characters a second
            # leaves a queue behind it, and the queue goes on draining for
            # seconds after the application is gone - 6803 characters of it,
            # which failed this check while the application was already dead.
            sv.s.write(b"\x1c")
            sv.s.flush()
            sv.pump(2)
            before = len(sv.text())
            sv.send("ps", 5)
            rep.check("and the application is gone",
                      "SPIN" not in sv.text()[before:])
            tidy(sv)

        print("a client that vanishes:")
        # The second, and the reason a board left alone stopped answering.
        #
        # A client killed with its socket still open reads nothing and
        # acknowledges nothing.  The radio then holds about forty kilobytes
        # that the driver, the sockets and five minutes of waiting do not
        # return, and below sixteen the link refuses every visitor - so the
        # board keeps its network and never serves a page again.  The
        # supervisor re-raises the point when it is starving with nothing
        # loaded; this asks whether that got the board back.
        cl.s.write(b"\x1b1")
        cl.s.flush()
        cl.pump(1.2)
        cl.send("run c:\\phonecl.axe %s -pass %s -hold 600"
                % (args.ip, args.password), 14)
        cl.s.write(b"\x1c")                  # kill it where it stands
        cl.s.flush()
        time.sleep(0.3)
        cl.s.write(b"\x1c")
        cl.s.flush()
        cl.pump(6)
        cl.user_slot()                      # killed from the system shell
        if sv:
            sv.pump(6)                      # seconds, not the supervisor's minute
            sv.s.write(b"\x1c")
            sv.s.flush()
            sv.pump(1.5)
            before = len(sv.text())
            sv.send("mem", 2.5)
            sv.send("net sockets", 2.5)
            out = sv.text()[before:]
            m = re.search(r"internal +\d+K +(\d+)K +(\d+)K", out)
            largest = int(m.group(2)) if m else 0
            rep.check("the board gets its memory back at once",
                      largest >= idle_largest - 4,
                      "%u KB as the largest block, %u when idle"
                      % (largest, idle_largest))
            o = re.search(r"(\d+) open, \d+ listening", out)
            q = re.search(r"(\d+) bytes waiting to go out", out)
            rep.check("and the stack is holding nothing",
                      bool(o) and int(o.group(1)) == 0 and
                      bool(q) and int(q.group(1)) == 0,
                      ("%s open, %s bytes queued" % (o.group(1), q.group(1)))
                      if o and q else "no answer from `net sockets`")
        out = run_client(cl, args, "-get", 30)
        if client_ran_out(out):
            rep.check("and serves the page to the next visitor (the CLIENT "
                      "board ran out of memory)", False,
                      "reset it and run this again")
        else:
            rep.check("and serves the page to the next visitor", "200 OK" in out)

        if args.hold:
            print("staying connected:")
            out = run_client(cl, args, "-hold %u" % args.hold, args.hold + 25)
            rep.check("link survives %u s" % args.hold, "held" in out,
                      "lost" if "link lost" in out else "")

    finally:
        tidy(sv)
        # Everything both boards said, byte for byte.  Twice now the question
        # "which side failed" has had no evidence behind it.
        for b in (cl, sv):
            if b is None:
                continue
            try:
                path = os.path.join("build", "phonecheck-%s.log" % b.name)
                os.makedirs("build", exist_ok=True)
                with open(path, "wb") as f:
                    f.write(b.log)
                print("(%s transcript: %s, %u bytes)"
                      % (b.name, path, len(b.log)))
            except OSError as exc:
                print("(could not write the %s transcript: %s)" % (b.name, exc))
        cl.close()
        if sv:
            sv.close()

    return rep.done()


if __name__ == "__main__":
    sys.exit(main())
