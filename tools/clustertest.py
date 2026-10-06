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

    def restart(self, seconds=20.0):
        self.log.write(b"\n=== reset %s ===\n" % self.name.encode())
        # The RTS pulse, and `reboot` only where it did nothing: a board on its
        # own USB-Serial-JTAG (the C3) ignores the pulse, and without the
        # command the C3 went into a run still playing the last run's tone,
        # with its memory and its wire state (6 Oct 2026).  Not both on every
        # board: the CYD rebooted twice in a row and missed the first checks.
        self.board.buf = bytearray()
        self.board.reset()
        self.board.pump(2.0)
        if not re.search(rb"rst:|ESP-ROM|boot:", bytes(self.board.buf)):
            self.log.write(b"(the pulse did nothing; `reboot`)\n")
            self.board.send(b"reboot\r")
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


def note(ok, name, detail="", why=""):
    """One result.

    `detail` is a FACT and is printed either way - "24 of 24" is worth seeing
    when it passes.  `why` is the reason it failed and is printed only then:
    the first draft printed the failure reason beside PASS, so the summary read
    "PASS run-output-came-back - nothing from the program", which is a sentence
    that cannot be true.
    """
    if not ok and why:
        detail = why
    line = ("PASS " if ok else "FAIL ") + name + (" - " + detail if detail
                                                  else "")
    RESULTS.append((ok, line))
    print(line, flush=True)
    return ok


def check(text, needle, name, detail=""):
    return note(needle in text, name, detail or ("no '%s'" % needle
                                                 if needle not in text else ""))


def flat(text):
    """One line of it, for looking up a PHRASE.

    A console has a width, and the CYD's is forty characters: "built for a
    different processor" arrives split across two rows, so a search for the
    sentence finds nothing while the sentence is on the screen.  The C3's
    console is eighty wide and the same check passed there, which is how the
    asymmetry gave it away.

    Only for phrases.  Anything that depends on WHICH line a thing is on must
    use the text as it came.
    """
    return re.sub(r"\s+", " ", text)


def num(text, pattern, default=None):
    m = re.search(pattern, text)
    return m.group(1) if m else default


def last_complaint(journal):
    """The last thing a board's JOURNAL called a problem, or "".

    For failure details.  A board says what went wrong in its journal whatever
    the screen was doing - that rule is already in this tree - and a check that
    fails without carrying the other board's sentence sends the next reader
    looking in the wrong place.  Measured: `standby-work-came-home` failed three
    runs saying only "it is still on the neighbour", while the C3 had said
    "no task to send the guest home" in the same second.

    Only the last one: a journal tail holds the whole run's noise, and the line
    that matters is the one nearest the failure.
    """
    hits = re.findall(r"\b[EW] \(\d+\) [^EW]*", journal)
    if not hits:
        return ""
    # The journal's own furniture - how many lines it is holding, and the
    # prompt under it - is not part of the sentence.
    clue = re.split(r"\s+\d+ lines held|\s+\(\d+\) [A-Z]:\\", hits[-1])[0]
    return clue.strip()[:140]


# ---------------------------------------------------------------- the wire ---


def bind_to(node, other, tries=4):
    """Point `node`'s link at the wire that reaches `other`.

    A no-op on a bench of two boards, and not one on a bench of three: the
    middle board holds two wires, and whichever of them the shell last used is
    the one `link ping` goes out of.  A ping that leaves by the wrong wire is
    answered by nobody and reports "0 of 24" - the exact reading that once cost
    two hours of measuring copper that was fine.  So the wire is chosen by
    NAME (the far end announces its own), and by looking rather than assuming.
    """
    for i in range(tries):
        t = node.ask("link")
        if ("the other is " + other.name) in t:
            return True
        node.ask("link use node%d" % i)
    t = node.ask("link")
    return ("the other is " + other.name) in t


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
    # a board that holds two wires comes up on whichever bound last
    bind_to(a, b)
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

    # ASKED FOR, not automatic.  `link devs` pulls the far node's registry
    # into this one; a board does not adopt its neighbour's hardware merely by
    # being wired to it, and that is the right default - two boards on a bench
    # are not one machine until somebody says so.
    #
    # The first draft of this check went straight to `dev` and failed four
    # times over, which looked like a broken feature and was a harness that
    # did not know the command.
    t = blind.ask("link devs", timeout=15.0)
    note("are in this registry now" in t, "dev-%s-pulled-the-registry" % blind.name,
         why="link devs said nothing")

    t = blind.ask("dev")
    note(head.name + "." in t, "dev-%s-sees-%s-devices" % (blind.name, head.name),
         why="no '%s.' entries" % head.name)
    for want in ("lcd0", "pcm0"):
        note(head.name + "." + want in t,
             "dev-%s-offers-%s" % (head.name, want))

    # And the headless board's DEFAULT for a class it does not have is the
    # neighbour's.  Not asked of a command that does not exist - the first
    # draft invented `dev default` - but PROVED by using it: a program that
    # opens the default audio device on a board with no DAC can only be heard
    # on the other board, and the sound scenario below counts it there.


def scen_remote_run(asker, runner, image, expect):
    """A program THIS board does not have, run on the one that does.

    THERE IS NO `run <node> <program>` - I invented that syntax and spent a
    round of the matrix watching the shell take the node name for a program,
    fail to find it, and send the whole line to the far board, which tried to
    run a program called "c3" and said -2.

    The real rule is better than the one I imagined, and it is written out in
    shell.c: `run <program>`, and if the file is not HERE and a wire is bound,
    the line goes to the node that has it.  A file on the other node IS the
    statement that the program belongs there - no manifest, no policy table,
    no names.  (The manifest still decides the harder half, "this wants a
    sound card", which cannot be read off a directory.)

    So the check is: ask for something that is only on the OTHER board.  What
    comes back proves three things at once - the far node found it, ran it,
    and its output came home to the node that asked.
    """
    print("\n== %s runs what only %s has ==" % (runner.name, runner.name),
          flush=True)

    t = asker.ask("run c:\\%s" % image, timeout=30.0)
    note("not here" in t, "run-%s-knew-it-was-not-here" % asker.name,
         why="it did not even look at the wire")
    note(expect in t, "run-%s-ran-it-and-answered-%s" % (runner.name,
                                                         asker.name),
         why="nothing recognisable came back")


def scen_wrong_arch(node, image):
    """An image for the other instruction set, on the board that cannot run it.

    Refused BY NAME - "built for a different processor" - rather than loaded
    and run into an illegal instruction somewhere inside.  This is the line a
    heterogeneous cluster cannot cross, and saying where it falls is as much
    of the answer as saying what works.

    Tested locally on each board because that is where it actually bites: a
    copy of the wrong build lands on a card, and the message has to name the
    reason.  The first draft of this check accepted almost any output, so it
    would have passed on a crash.
    """
    print("\n== %s is handed an image for the other processor ==" % node.name,
          flush=True)
    t = node.ask("run c:\\%s" % image, timeout=15.0)
    note("Guru" not in t and "panic" not in t,
         "wrong-arch-%s-did-not-crash" % node.name)

    # IN THE JOURNAL, NOT ON THE SCREEN.
    #
    # The refusal always happens; whether its words are still VISIBLE depends
    # on what else wanted the screen in the same half-second.  Once, on the
    # CYD, the loading view opened over them and the transcript kept only
    # "returned -45" - a check that failed while the board was behaving
    # exactly as designed.  The journal holds what was said whatever the
    # screen did, which is the rule this tree already wrote down for driver
    # messages, and it applies here for the same reason.
    j = flat(node.ask("log -n 12", timeout=20.0))
    note("built for a different processor" in j,
         "wrong-arch-%s-refused-in-those-words" % node.name,
         why="no clear refusal in the journal either")


def scen_cross_arch(asker, runner, foreign, copied):
    """The line a cluster cannot cross, crossed - by sending the PROGRAM.

    The runner is handed a path to an image of the OTHER instruction set.  It
    cannot execute it, but it can READ it, so it takes the program's name out
    of the header and runs its own build of that program instead.

    Set up by COPYING rather than by deleting.  The first plan was to remove
    one board's own NODEHI so the run would have to travel, and the two
    directions then wanted the same file gone and present at once; worse, a
    check that deletes from a card leaves the board different afterwards.  A
    copy under a name the ASKER does not have sends the run across just as
    surely, destroys nothing, and tests the actual claim - that the answer
    comes from the header and not from what the file is called.
    """
    print("\n== %s asks for an image %s cannot execute ==" % (asker.name,
                                                             runner.name),
          flush=True)

    runner.ask("copy c:\\%s c:\\%s" % (foreign, copied), timeout=20.0)
    t = asker.ask("run c:\\%s" % copied, timeout=40.0)
    f = flat(t)
    note("not here" in f, "cross-%s-sent-it-to-%s" % (asker.name, runner.name),
         why="it never reached the wire")
    note("nodehi: here" in f, "cross-%s-ran-its-own-build" % runner.name,
         why="no output from the substituted program")
    # NOT "-45 is absent": the number is there on purpose.  The first attempt
    # IS the refusal - that is what starts the substitution - so the journal
    # and the transcript both carry it, and a check that forbids it fails on a
    # run that worked perfectly.  What must not survive is the ASKER being
    # told the run came to nothing.
    note("did not run there either" not in f,
         "cross-%s-did-not-refuse" % runner.name,
         why="the asker was told it failed")


def scen_standby(fast, small, image):
    """Work handed to the neighbour, and fetched back - across two ISAs.

    Maxim's fourth shape: a board with the clock and the current beside one
    that costs nothing to leave on.  `link standby` is what the idle watcher
    does on its own, said by hand so the check does not have to wait out the
    quiet period; `link wake` is what it does when work turns up again.

    It exercises the cascade as well, and not by arrangement: the image that
    travels is the ASKING board's, so the receiving board is always handed one
    it cannot execute and has to find its own build of that program - in both
    directions, since the work comes home the same way.

    The number is the point.  MIGRANT counts, and a run that says "carried on
    from 6" has proved the state crossed; one that starts at 0 has proved only
    that a program can be started twice.
    """
    print("\n== %s hands its work to %s and takes it back ==" % (fast.name,
                                                                 small.name),
          flush=True)

    fast.ask("run /b c:\\%s" % image, timeout=25.0)
    time.sleep(2.0)

    t = flat(fast.ask("link standby", timeout=45.0))
    note("now on" in t, "standby-%s-handed-over" % fast.name,
         why="nothing moved; see the journal for what refused")
    note("carried on from" in t, "standby-state-crossed",
         why="it started fresh instead of carrying on")

    time.sleep(2.0)
    t = flat(small.ask("ps", timeout=25.0))
    note("MIGRANT" in t, "standby-%s-is-running-it" % small.name,
         why="the neighbour has no such process")
    t = flat(small.ask("log -n 8", timeout=30.0))
    note("another processor" in t, "standby-%s-substituted-its-own" %
         small.name, why="no substitution line in the journal")

    fast.ask("link wake", timeout=30.0)
    time.sleep(8.0)
    t = flat(fast.ask("ps", timeout=25.0))
    if "MIGRANT" in t and small.name + "." not in t:
        note(True, "standby-work-came-home")
    else:
        # AND ASK THE BOARD THAT WAS ASKED FOR IT, because that is where the
        # reason is.  This check failed three runs in a row saying only "it is
        # still on the neighbour", which reads as the harness not waiting long
        # enough - and the answer was in the C3's own journal the whole time:
        #
        #   E (82544) node: no task to send the guest home
        #
        # Nobody read it, because this harness only reads the board that ASKS
        # after `link wake`.  A cluster failure is a sentence one board says and
        # the other hears, so a failure detail that does not carry what the
        # other board said is a failure detail that costs a run.
        why = "it is still on the neighbour"
        clue = last_complaint(flat(small.ask("log -n 12", timeout=30.0)))
        if clue:
            why += "; %s journal: %s" % (small.name, clue)
        note(False, "standby-work-came-home", why=why)

    # Tidy up: the check that leaves a counter running poisons the next one.
    name = num(flat(fast.ask("ps", timeout=20.0)), r"(\d+)\s+MIGRANT")
    if name:
        fast.ask("kill %s" % name, timeout=20.0)


def scen_ps_kill(near, far, image):
    """One process table across two boards, and a kill that crosses the wire.

    A REAL process on the far board, started in the background - the first
    draft used `link echo`, which is a shell command and not a process, so
    there was nothing for `ps` to find and the check failed for a reason that
    had nothing to do with the cluster.
    """
    print("\n== one ps, both boards ==", flush=True)

    far.ask("run /b c:\\%s" % image, timeout=20.0)
    time.sleep(1.5)
    t = near.ask("ps", timeout=15.0)
    note(far.name + "." in t, "ps-%s-sees-a-process-on-%s" % (near.name,
                                                              far.name),
         why="no process named for %s" % far.name)

    # And the name it printed is the name that can be typed back at it.
    name = num(t, r"(" + far.name + r"\.\d+)")
    if name:
        t = near.ask("kill %s" % name, timeout=15.0)
        note("no such" not in t.lower(), "kill-crossed-the-wire", name)
    else:
        note(False, "kill-crossed-the-wire", why="nothing to kill")


def scen_sound(quiet, loud, image):
    """Sound from the board that has no DAC, landing on the board that has one.

    Maxim's third scenario, the half that works today: the synthesiser on one
    board, the output on another.  Nothing in the program knows - it opens the
    mixer, and the mixer's default sink on a board with no output of its own is
    the NEIGHBOUR'S, chosen by class.

    NOT tested with TONE, which was the first attempt: tone opens /dev/pcm0 BY
    NAME, so on a board with no pcm0 it simply fails with -2 and the class
    default never gets a chance to do its job.  A test that names the device is
    not testing the thing that finds the device.

    Counted on the RECEIVING board, and that is the only honest place: the
    CYD's speaker connector ships empty, so listening proves nothing either
    way.  Periods delivered and periods lost are facts; silence is not.
    """
    print("\n== sound, poured across the wire ==", flush=True)

    t = quiet.ask("run c:\\%s pcmmix 192 40" % image, timeout=30.0)
    if "file not found" in t or "not found" in t:
        note(False, "sound-%s-has-the-poker" % quiet.name,
             why="no %s on %s" % (image, quiet.name))
        return
    note("devpoke:" in t, "sound-%s-wrote-periods" % quiet.name,
         why="the program said nothing")

    # WAIT BEFORE ASKING.  The receiving board says its count 400 ms after the
    # last period - deliberately, so the end of a burst gets one line instead
    # of forty - and its pour task wakes on a 100 ms timer.  Reading the
    # journal the instant the sending board returns its prompt asks before the
    # answer has been written, and reports "no pour report" while the sound
    # arrived perfectly.  That cost an evening of suspecting the receiver.
    time.sleep(2.0)
    t = loud.ask("log -n 30", timeout=15.0)
    # "poured 8 periods, 1632 bytes, 0 lost" - no colon.  The first version of
    # this looked for "poured:" and reported "no pour report" while the line
    # was sitting in the journal.
    # Flattened first: the CYD's console is forty wide, so "poured 7 periods,
    # 1568 bytes, 0 lost" arrives cut in two and the loss count is on the next
    # row.  The same wrap already hid a refusal message earlier today.
    t = flat(t)
    got = num(t, r"poured (\d+) periods", None)
    if got is None:
        note(False, "sound-arrived-at-%s" % loud.name, why="no pour report")
        return
    lost = num(t, r"poured \d+ periods, \d+ bytes, (\d+) lost", "?")
    # "? lost" is honest and not a failure: this board's console is forty
    # columns and the journal CLIPS an entry to the width, so the tail of
    # "poured 7 periods, 1568 bytes, 0 lost" never reaches the screen.  The
    # count that matters - periods that reached the converter - is at the
    # front of the line, which is where it belongs.
    note(int(got) > 0, "sound-arrived-at-%s" % loud.name,
         "%s periods, lost: %s" % (got, lost if lost != "?" else
                                   "off the 40-column screen"))


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

    # Which wire each board is talking on.  On the desk this ran on, the C3 had
    # two neighbours bound, so this is what makes the pair under test the pair
    # named on the command line.
    print("choosing the wire between them", flush=True)
    if not bind_to(cyd, c3):
        print("  cyd never reported c3 on its wire", flush=True)
    if not bind_to(c3, cyd):
        print("  c3 never reported cyd on any wire it has", flush=True)

    scen_wire(cyd, c3)
    scen_devices(cyd, c3)

    # What is on which board decides who runs what, so the images are chosen
    # for being present on ONE of them: tonerv.axe only on the C3, link32.axe
    # only on the CYD.  No new files needed - and the tone doubles as the
    # sound scenario, since the C3 has no DAC of its own.
    rounds = []
    if args.main in ("cyd", "both"):
        # two seconds, not 200: since 25 Sep a far board's sound is paced to
        # real time, so "200" is a 200 s run, and this check waits 30 for its
        # prompt - the CYD's shell then stayed busy through every check after
        # it (6 Oct 2026)
        rounds.append((cyd, c3, "tonerv.axe 440 20 2", "tone:"))
    if args.main in ("c3", "both"):
        rounds.append((c3, cyd, "link32.axe", "usage: link"))

    for asker, runner, image, expect in rounds:
        scen_remote_run(asker, runner, image, expect)

    # A real process on each board, seen and killed from the other.
    # MIGRANT, because it is the one program on both boards that keeps running
    # until told to stop - and the kill below is what stops it, so the check
    # tidies up after itself.
    #
    # Not TONE, which was the first choice and a bad one: on the C3 it opens
    # /dev/pcm0 by name, finds none, and exits at once - so there was never a
    # process for `ps` to see, and the check failed for a reason that had
    # nothing to do with the wire.  Not LINK either: its recv opens uart1,
    # which the wire driver owns.
    scen_ps_kill(cyd, c3, "migranrv.axe")
    scen_ps_kill(c3, cyd, "migran32.axe")

    # The line a heterogeneous cluster cannot cross, on both sides of it.
    scen_wrong_arch(cyd, "nodehirv.axe")
    scen_wrong_arch(c3, "nodehi32.axe")

    # ...and the same line crossed, when the run comes over the wire.  Local
    # is still a refusal: nobody asked for a program there, they named a file.
    if args.main in ("cyd", "both"):
        scen_cross_arch(cyd, c3, "nodehi32.axe", "xeno32.axe")
    if args.main in ("c3", "both"):
        scen_cross_arch(c3, cyd, "nodehirv.axe", "xenorv.axe")

    # The standby pair.  Last, because it is the only scenario that leaves a
    # process running across boards if it fails half way, and because it uses
    # everything the ones above have already proved separately.
    if args.main in ("cyd", "both"):
        scen_standby(cyd, c3, "migran32.axe")

    scen_sound(c3, cyd, "devpokrv.axe")
    scen_restart(c3, cyd)

    bad = sum(1 for ok, _ in RESULTS if not ok)
    print("\nclustertest: %d checks, %d failed" % (len(RESULTS), bad),
          flush=True)
    print("transcript: %s" % args.log, flush=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
