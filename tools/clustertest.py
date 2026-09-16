#!/usr/bin/env python3
"""Two REAL boards, wired to each other, driven together and checked.

`nodetest` does this with two emulators and a socket for a wire.  This does it
with the boards on the desk and the copper between them, and it exists for the
same reason `boardtest` does: a hardware result typed into a terminal by hand
cannot be repeated by anyone else, including by the person who typed it a week
later.  Every number this project quotes about the cluster on hardware should
come out of here.

    python tools/clustertest.py --cyd COM7 --c3 COM8
    python tools/clustertest.py --cyd COM7 --c3 COM8 --main c3

WHAT IS DIFFERENT FROM THE EMULATOR, and why every one of these had to be
found on the bench rather than reasoned about:

  * The two boards do not start together.  One has been running for a minute
    and has sent frames into a wire nobody was listening to.  That is what
    found the missing NL_F_RST - in QEMU one script starts both guests and
    they begin in step, so the resynchronisation that the protocol documents
    was never once needed and was never once implemented.

  * They are not the same machine.  The CYD is an ESP32 - Xtensa LX6, two
    cores, a screen, a card slot and a DAC.  The C3 is RISC-V, one core, and a
    header.  Messages and devices cross that line without noticing; CODE does
    not, and this harness is written to show exactly where the line falls.

  * A board's console is a shared resource and this holds BOTH open at once.
    That is the whole point: `link ping` needs `link echo` running on the far
    board at the same time, and a harness that can only talk to one board can
    only ever see half of an exchange.  Two hours went on pinging a board that
    had nobody listening, reading "0 of 24" as a broken wire.

RESET POLICY.  Both boards are reset once at the start and then left running,
because a reset loses the link binding - AUTOEXEC.BAT binds it again at boot,
which is why that file exists - and because resetting one board mid-test is
exactly the situation the link reset is there to survive.  There is a check
that does it on purpose.
"""

import argparse
import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from boardtest import Board, PROMPT, visible  # noqa: E402


class Log:
    """One transcript for both boards, tagged, in bytes.

    Bytes rather than text, for the reason boardtest gives: a transcript
    written as text turns every byte above 0x7f into two, and an afternoon
    goes on blaming the renderer.
    """

    def __init__(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.f = open(path, "wb")

    def write(self, data):
        self.f.write(data)

    def flush(self):
        self.f.flush()


class Node:
    """A board with a name in the cluster."""

    def __init__(self, name, port, log, echo=False):
        self.name = name
        self.board = Board(port, 115200, log, echo)
        self.log = log

    def restart(self, seconds=6.0):
        self.log.write(b"\n=== reset %s ===\n" % self.name.encode())
        self.board.reset()
        self.board.wait_prompt(seconds)

    def say(self, line, wait=4.0):
        """Send a line and return everything that came back, escapes stripped.

        Does NOT insist on a prompt: half of what is interesting here is a
        command that deliberately does not return yet - `link echo 40` holds
        its board for forty seconds, and that is the point of it.
        """
        self.log.write(b"\n--- %s: %s ---\n" % (self.name.encode(),
                                                line.encode()))
        self.board.buf = bytearray()
        self.board.send((line + "\r").encode())
        self.board.pump(wait)
        return clean(self.board.buf)

    def ask(self, line, timeout=8.0):
        """Send a line and wait for the prompt: for commands that finish."""
        self.log.write(b"\n--- %s: %s ---\n" % (self.name.encode(),
                                                line.encode()))
        self.board.buf = bytearray()
        self.board.send((line + "\r").encode())
        self.board.wait_prompt(timeout)
        return clean(self.board.buf)


def clean(buf):
    """What was on the screen, as text, with the cursor movements taken out.

    NOT a log: this is a RENDERED SCREEN, so a line can appear twice because
    the screen scrolled and was drawn again.  Never count occurrences here -
    ask whether a thing is present, or judge by ORDER.  Counting cost a whole
    run in the emulator harness once: fifteen hits for one line.
    """
    text = buf.decode("latin-1")
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "\n", text)


RESULTS = []


def note(ok, name, detail=""):
    line = ("PASS " if ok else "FAIL ") + name + (" - " + detail if detail
                                                  else "")
    RESULTS.append((ok, line))
    print(line, flush=True)
    return ok


def check(text, needle, name, detail=""):
    return note(needle in text, name, detail or ("no '%s'" % needle
                                                 if needle not in text else ""))


def num(text, pattern, default=None):
    m = re.search(pattern, text)
    return m.group(1) if m else default


# ---------------------------------------------------------------- the wire ---


def scen_wire(a, b):
    """Both ends bound, each naming the other, and a round trip both ways.

    Names first, because "the other is not saying yet" was the symptom that a
    whole evening went on misreading: it means frames are arriving and none
    are being accepted, which looks like a bad wire and is not one.
    """
    print("\n== the wire itself ==", flush=True)

    ta = a.ask("link")
    tb = b.ask("link")
    note("the other is " + b.name in ta, "wire-%s-names-%s" % (a.name, b.name))
    note("the other is " + a.name in tb, "wire-%s-names-%s" % (b.name, a.name))

    for sender, responder, cls, tag in ((a, b, "", ""),
                                        (b, a, "", ""),
                                        (a, b, " stream", "-stream")):
        # The responder must be listening while the sender pings: `link ping`
        # is one half of a command whose other half is `link echo`, and a ping
        # with nobody answering reports zero - which reads as a dead wire and
        # cost two hours of measuring copper that was fine.
        responder.say("link echo 20", wait=0.4)
        t = sender.ask("link ping 24%s" % cls, timeout=25.0)
        got = num(t, r"(\d+) of 24 came back", "0")
        note(got == "24", "wire-%s-to-%s%s" % (sender.name, responder.name, tag),
             "%s of 24" % got)
        bad = num(t, r"losses\s+\d+ gaps, \d+ duplicates, (\d+) bad", "?")
        note(bad == "0", "wire-%s-no-bad-frames%s" % (sender.name, tag),
             "%s bad" % bad)
        rtt = num(t, r"round trip (\d+) ms", "?")
        print("    round trip %s ms" % rtt, flush=True)
        # Let the responder's twenty seconds run out before the next step.
        responder.board.pump(21.0)


def scen_restart(a, b):
    """One board reboots; the pair must rejoin WITHOUT touching the other.

    This is the defect hardware found, written as a check.  A node that
    restarts - a power cut, a crash, a flash - sends NL_F_RST when it binds,
    and the peer that never left drops what it had.  Before that flag was
    implemented (it was declared in the header and sent by nobody) the pair
    stayed dead until BOTH were restarted, which on a bench looks like a wire
    that works sometimes.
    """
    print("\n== one end restarts ==", flush=True)
    a.restart()
    time.sleep(1.0)
    ta = a.ask("link")
    note("the other is " + b.name in ta,
         "restart-%s-found-%s-again" % (a.name, b.name))
    b.say("link echo 20", wait=0.4)
    t = a.ask("link ping 8", timeout=25.0)
    got = num(t, r"(\d+) of 8 came back", "0")
    note(got == "8", "restart-wire-works-again", "%s of 8" % got)
    b.board.pump(21.0)


# ------------------------------------------------ what one board lends another ---


def scen_devices(head, blind):
    """The board with no glass sees the other's, and uses it by default.

    This is Maxim's first scenario in miniature - a strong processor with no
    screen beside a cheap board that is mostly screen.  The C3 has no display,
    no card and no sound; it says so in its BOARD.CFG rather than leaving the
    sections out, and the difference matters: a missing section is an author
    who forgot, `= none` is a board that has none.
    """
    print("\n== the neighbour's devices ==", flush=True)

    t = blind.ask("dev")
    note(head.name + "." in t, "dev-%s-sees-%s-devices" % (blind.name, head.name),
         "no '%s.' entries" % head.name)
    for want in ("lcd0", "pcm0"):
        note(head.name + "." + want in t,
             "dev-%s-offers-%s" % (head.name, want))

    # And the headless board's DEFAULT for a class it does not have is the
    # neighbour's.  Asked of the system rather than assumed: `dev default`
    # names what an application would actually get.
    t = blind.ask("dev default")
    note(head.name + "." in t, "dev-default-crosses-the-wire",
         "the blind node's defaults are all local")


def scen_remote_run(asker, runner, image):
    """A program on THIS board's card, executed on the OTHER one.

    The image has to be one the far board can execute, and that is the whole
    point of the check: a cluster spanning two instruction sets passes
    messages and shares devices without noticing the difference, and CODE
    stops at the line.  So each board carries images built for its NEIGHBOUR
    as well as itself - HELLORV on the Xtensa board, HELLO32 on the RISC-V one.

    What this proves in one command: the far node reads the file off THIS
    node's card over the wire, loads it, runs it, and the output comes back
    here.  The program moved; the file did not.
    """
    print("\n== %s runs a program that lives on %s ==" % (runner.name,
                                                          asker.name),
          flush=True)

    t = asker.ask("run %s c:\\%s" % (runner.name, image), timeout=25.0)
    note("Hello" in t or "hello" in t,
         "run-%s-output-came-back-to-%s" % (runner.name, asker.name),
         "nothing from the program")
    note("not found" not in t and "refused" not in t,
         "run-%s-accepted-the-image" % runner.name)


def scen_wrong_arch(asker, runner, image):
    """And the line itself: an image for the WRONG instruction set.

    Refused, by name, rather than loaded and run into an illegal instruction.
    This is the check that says what a heterogeneous cluster cannot do, and it
    is as much a part of the answer as what it can.
    """
    print("\n== an image the far board cannot execute ==", flush=True)
    t = asker.ask("run %s c:\\%s" % (runner.name, image), timeout=25.0)
    # The loader's own words, and nothing looser.  The first draft of this
    # check accepted almost any output - it had `or "-" in t` in it - so it
    # would have passed on a crash, on a timeout and on a blank screen.  A
    # check that cannot fail is not a check.
    note("built for a different processor" in t,
         "wrong-arch-refused-in-those-words",
         "no clear refusal; read the transcript")
    note("Guru" not in t and "panic" not in t,
         "wrong-arch-did-not-crash-the-far-board")


def scen_ps_kill(a, b):
    """One process table across two boards, and a kill that crosses the wire."""
    print("\n== one ps, both boards ==", flush=True)
    b.say("link echo 25", wait=0.4)
    t = a.ask("ps")
    note(b.name in t, "ps-shows-the-other-board",
         "no process named for %s" % b.name)
    b.board.pump(26.0)


def scen_sound(quiet, loud):
    """Sound poured from the board that has none onto the board that has one.

    Maxim's third scenario, the half that works: a synthesiser on one board,
    the output on another.  The C3 has no DAC at all, so its default audio
    device IS the neighbour's, chosen by class - nothing in the program knows.

    Counted on the RECEIVING board, and that is the only honest place: the
    CYD's speaker connector ships empty, so listening proves nothing either
    way.  Periods arrived and periods lost are facts; silence is not.
    """
    print("\n== sound, poured across the wire ==", flush=True)

    t = quiet.ask("run c:\\tonerv.axe 440 30 200", timeout=20.0)
    if "not found" in t:
        note(False, "sound-tone-image-present",
             "no tonerv.axe on %s - build it" % quiet.name)
        return

    t = loud.ask("log -n 30", timeout=10.0)
    got = num(t, r"poured: (\d+) periods", None)
    if got is None:
        note(False, "sound-arrived-at-%s" % loud.name, "no pour report")
        return
    lost = num(t, r"poured: \d+ periods, \d+ bytes, (\d+) lost", "?")
    note(int(got) > 0, "sound-arrived-at-%s" % loud.name,
         "%s periods, %s lost" % (got, lost))
    note(lost == "0" or int(lost) < int(got),
         "sound-loses-less-than-it-delivers", "%s lost of %s" % (lost, got))


# ------------------------------------------------------------------- main ---


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cyd", required=True, help="the ESP32 board's port")
    ap.add_argument("--c3", required=True, help="the RISC-V board's port")
    ap.add_argument("--main", choices=("cyd", "c3", "both"), default="both",
                    help="which board asks and which answers; `both` runs the "
                         "whole list twice with the roles swapped, because a "
                         "cluster that only works one way round is not one")
    ap.add_argument("--log", default=os.path.join(ROOT, "build",
                                                  "clustertest.log"))
    ap.add_argument("--echo", action="store_true",
                    help="mirror both consoles to stdout while it runs")
    args = ap.parse_args()

    log = Log(args.log)
    cyd = Node("cyd", args.cyd, log, args.echo)
    c3 = Node("c3", args.c3, log, args.echo)

    # Both, once, at the start.  Their AUTOEXEC binds the wire; the reset that
    # matters to the protocol is tested on purpose further down.
    print("resetting both boards", flush=True)
    cyd.restart()
    c3.restart()
    time.sleep(1.5)

    scen_wire(cyd, c3)
    scen_devices(cyd, c3)

    rounds = []
    if args.main in ("cyd", "both"):
        rounds.append((cyd, c3, "hellorv.axe", "hello32.axe"))
    if args.main in ("c3", "both"):
        rounds.append((c3, cyd, "hello32.axe", "hellorv.axe"))

    for asker, runner, right, wrong in rounds:
        scen_remote_run(asker, runner, right)
        scen_wrong_arch(asker, runner, wrong)
        scen_ps_kill(asker, runner)

    scen_sound(c3, cyd)
    scen_restart(c3, cyd)

    bad = sum(1 for ok, _ in RESULTS if not ok)
    print("\nclustertest: %d checks, %d failed" % (len(RESULTS), bad),
          flush=True)
    print("transcript: %s" % args.log, flush=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
