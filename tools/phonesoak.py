#!/usr/bin/env python3
"""An hour of ordinary use over the phone link, and does the board stay awake.

  python tools/phonesoak.py --client COM4 --server COM7 --pass argon
  python tools/phonesoak.py --client COM4 --server COM7 --minutes 10

Not a feature test - phonecheck.py does that, once, on a freshly booted board.
This is the other question, the one that decides whether the thing is usable:
after an hour of commands, applications starting and stopping, files being
copied and programs crashing outright, is the system still answering?

Maxim's terms, and they are the right ones: applications may fall over, but
the OS underneath them must stay responsive.  So a crash is not a failure here
- an unanswered board is.

What it measures each round:

  * how long the board took to answer a command through the link, which is the
    number a person actually feels
  * whether the link survived, or had to be made again
  * free memory and the largest free block, which is what decides whether a
    new client can arrive at all (16 KB floor; see docs/plans/phone.md)

Applications are stopped by pid at the end of every round, because one left
running holds the memory everything else needs - that mistake has cost hours
of this project's time already.
"""
import argparse
import re
import sys
import time

import serial


def say(*parts):
    """Print and flush.  The first hour-long run was killed with two
    hours of findings still sitting in a buffer, which is the same as
    not having run it."""
    print(*parts, flush=True)


def open_port(name):
    s = serial.Serial()
    s.port = name
    s.baudrate = 115200
    s.timeout = 0.4
    # Deliberately NOT xonxoff.
    #
    # The board does send XOFF when its console cannot keep up, and honouring
    # it looked right - until a run sat for two hours instead of one and had
    # to be killed.  An XOFF whose XON never arrives blocks every write for
    # ever, and a test harness that can hang is worse than one that has to
    # be paced by hand.  The pacing below does that job.
    s.xonxoff = False
    s.dtr = False
    s.rts = False
    s.open()
    return s


class Board:
    def __init__(self, port):
        self.s = open_port(port)
        self.log = bytearray()

    def type_line(self, text):
        """A line, in pieces the board's console can keep up with.

        Written whole, a ninety-character command loses characters in the
        middle: the console asks for a pause with XOFF, this harness cannot
        honour it (an XOFF whose XON never comes blocks for ever), and what
        arrives is a command torn in half.  Three rounds of an hour-long run
        failed that way, all of them the longest command in the set - which
        looked like the board dropping input and was this typing too fast.
        """
        for i in range(0, len(text), 24):
            self.s.write(text[i:i + 24].encode())
            self.s.flush()
            time.sleep(0.05)
        self.s.write(b"\r")
        self.s.flush()

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.log.extend(self.s.read(4096))

    def send(self, text, wait):
        before = len(self.text())
        self.s.write((text + "\r").encode())
        self.s.flush()
        self.pump(wait)
        return self.text()[before:]

    def drain(self):
        """Read until the board has been quiet for a moment, then forget it.

        A round leaves the console still catching up, and a marker looked
        for straight afterwards is found in the previous round's output
        within half a second - which measures nothing.  Quiet first, then
        start counting.
        """
        # Three quiet reads in a row, not one: the board pauses mid-output
        # for longer than a single short read, so one quiet moment is not
        # the end of anything - and clearing too early leaves the tail to
        # arrive during the next round and be mistaken for its answer.
        quiet = 0
        for _ in range(40):
            before = len(self.log)
            self.pump(0.4)
            quiet = quiet + 1 if len(self.log) == before else 0
            if quiet >= 3:
                break
        # And then ask the shell to say it is there.  Quiet only means the
        # board has stopped talking for a moment; a prompt answered on
        # demand means it is ready to be asked something, which is the
        # thing actually needed before the clock starts.
        self.log.clear()
        self.s.write(b"\r")
        self.s.flush()
        for _ in range(20):
            self.pump(0.3)
            if ">" in self.text():
                break
        self.log.clear()

    def send_until(self, text, marker, timeout):
        """Send, and wait for the prompt to come back rather than for a clock.

        The whole point here is how long the board makes a person wait, and
        a fixed pause measures the pause.  The prompt returning is the board
        saying it has finished.
        """
        # Everything the board is still saying from the last round goes
        # first.  Without this the marker is found in the previous run's
        # output within half a second, every time, and the measurement is
        # of nothing at all.
        self.drain()
        before = len(self.text())
        started = time.time()
        self.s.write((text + "\r").encode())
        self.s.flush()
        while time.time() - started < timeout:
            self.pump(0.3)
            out = self.text()[before:]
            if marker in out:
                return out, time.time() - started
        return self.text()[before:], time.time() - started

    def text(self):
        t = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", self.log.decode("latin1"))
        return t.replace("\r", "\n")

    def sys_shell(self):
        self.s.write(b"\x1c")
        self.s.flush()
        self.pump(1.2)
        self.send("", 0.4)

    def user_slot(self):
        self.s.write(b"\x1b1")
        self.s.flush()
        self.pump(1.2)
        self.send("", 0.4)

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


def wait_idle(cl, timeout=40):
    """Until the client board has finished the last round.

    Rounds were being sent faster than the board could run them: the
    commands queued up, the output of one round arrived during the next,
    and the console started sending XOFF.  Everything measured after that
    was a measurement of the queue.
    """
    started = time.time()
    cl.sys_shell()
    while time.time() - started < timeout:
        if "no applications loaded" in cl.send("ps", 2.0):
            break
        time.sleep(1)
    cl.user_slot()


def stop_everything(sv):
    """Nothing left running, by pid.  Ctrl+backslash twice does not take the
    file manager, and an application holding memory makes everything after it
    fail for reasons that have nothing to do with the test."""
    if sv is None:
        return 0
    sv.sys_shell()
    stopped = 0
    for _ in range(4):
        out = sv.send("ps", 2.5)
        if "no applications loaded" in out:
            break
        # One long line, not many: the console redraws with cursor moves.
        pids = sorted(set(re.findall(
            r"(\d+)\s+[A-Z][A-Z0-9_.]*\s+(?:running|ready|loading)", out)))
        if not pids:
            break
        for pid in pids:
            sv.send("kill " + pid, 2.5)
            stopped += 1
    return stopped


def board_memory(sv):
    if sv is None:
        return (0, 0)
    sv.sys_shell()
    out = sv.send("mem", 2.5)
    m = re.search(r"internal +(\d+)K +(\d+)K +(\d+)K", out)
    return (int(m.group(2)), int(m.group(3))) if m else (0, 0)


def through_the_link(cl, args, actions, wait, tag):
    """One PHONECL run, timed until its own tag comes back.

    The tag is the round number.  A console still catching up delivers the
    previous round's output during this one, so waiting for "done" finds
    the last round's and measures nothing; a number chosen here cannot be
    confused with anything earlier.
    """
    return cl.send_until("run c:\\phonecl.axe %s -pass %s %s -tag %u"
                         % (args.ip, args.password, actions, tag),
                         "tag[%u]" % tag, wait)


# The round, in order.  Ordinary use is commands, programs, files - and, on a
# board without an MMU, programs that die badly.
ROUNDS = [
    ("commands", '-type "ver" -enter -wait 700 -type "mem" -enter '
                 '-wait 700 -find ArgonOS', 40),
    ("a directory listing", '-type "dir c:\\\\" -enter -wait 1200 '
                            '-find PHONE', 40),
    ("the file manager", '-type "fm" -enter -wait 2500 -find "file manager" '
                         '-key ctrl+c -wait 800', 45),
    ("copying a file", '-type "copy c:\\\\crash.axe c:\\\\tmp.bin" -enter '
                       '-wait 1500 -type "dir c:\\\\" -enter -wait 1200 '
                       '-find TMP', 45),
    ("deleting it again", '-type "del c:\\\\tmp.bin" -enter -wait 1200 '
                          '-type "ver" -enter -wait 700 -find ArgonOS', 40),
    ("a program that crashes", '-type "run c:\\\\crash.axe" -enter -wait 2000 '
                               '-type "ver" -enter -wait 900 -find ArgonOS', 45),
    ("a crash on a thread", '-type "run c:\\\\crash.axe thread" -enter '
                            '-wait 2000 -type "ver" -enter -wait 900 '
                            '-find ArgonOS', 45),
    ("a null call", '-type "run c:\\\\crash.axe jump" -enter -wait 2000 '
                    '-type "ver" -enter -wait 900 -find ArgonOS', 45),
    # After graphics, the console must come back - that is the check, not
    # whether a word can be found on a screen made of pixels (it cannot:
    # -find reads text cells, and there are none while an application draws).
    # After graphics the console must come back - that is the check.  Looking
    # for a word on the screen is no good here: -find reads text cells and
    # there are none while an application is drawing pixels.
    ("pixels", '-type "run c:\\\\gfxpix.axe" -enter -wait 2500 -key ctrl+c -wait 1500 -type "ver" -enter -wait 900 -find ArgonOS', 45),
    ("switching slots", '-key alt+2 -wait 700 -key alt+1 -wait 700 '
                        '-type "ver" -enter -wait 800 -find ArgonOS', 40),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--client", required=True)
    ap.add_argument("--server", required=True)
    ap.add_argument("--ip", default="192.168.4.1")
    ap.add_argument("--password", default="argon")
    ap.add_argument("--minutes", type=int, default=60)
    args = ap.parse_args()

    cl = Board(args.client)
    sv = Board(args.server)

    # Both boards, from a reboot.
    #
    # The client needs it as much as the server: commands typed at a console
    # queue up, and a run that ended early leaves its queue behind.  The first
    # round of this script was being answered with the output of a round from
    # a previous run - tag 13 arriving for round 1 - which is a measurement of
    # the queue and nothing else.  The client rejoins the network by itself,
    # because `wifi connect` wrote it into its SYSTEM.CFG.
    say("(resetting both boards)")
    for b in (sv, cl):
        b.s.rts = True
        time.sleep(0.15)
        b.s.rts = False
    # Forget everything said before the reset, or the wait below is
    # satisfied by a prompt from the previous life of the board.
    sv.log.clear()
    cl.log.clear()
    sv.pump(12)
    cl.pump(2)
    sv.send("", 1)
    # The client takes longer to boot than a fixed pause: typing into it
    # before it is up leaves the commands queued, and the first round then
    # gets the answer to something else entirely.
    # Wait for the line the shell prints when it is ready, not for a prompt:
    # a prompt flickers past during boot and in echoes, and typing at one of
    # those leaves the command queued until the board is really up - which is
    # how round 1 came to be answered with a boot log.
    for _ in range(40):
        cl.pump(1.0)
        if "for a list of commands" in cl.text():
            break
    else:
        say("the client board did not finish booting")
        return 1
    say("  (client booted; %u bytes of boot log)" % len(cl.log))
    cl.log.clear()
    cl.send("", 1)

    # And wait for the client to be back on the far board's network.
    for _ in range(20):
        if "joined" in cl.send("wifi", 2.5):
            break
        time.sleep(2)
    else:
        say("the client never rejoined the network")
        return 1
    cl.user_slot()

    free0, largest0 = board_memory(sv)
    say("start: %u KB free, largest %u KB\n" % (free0, largest0))

    deadline = time.time() + args.minutes * 60
    n = 0
    unanswered = 0
    worst = 0.0
    worst_what = ""
    low_free, low_largest = free0, largest0
    crashes_survived = 0

    while time.time() < deadline:
        # A hard stop as well as the deadline: a round that takes longer than
        # its own budget plus a minute means something is stuck, and a soak
        # test that hangs is worse than one that stops and says so.
        round_started = time.time()
        wait_idle(cl)
        name, actions, wait = ROUNDS[n % len(ROUNDS)]
        n += 1
        out, took = through_the_link(cl, args, actions, wait, n)

        linked = "linked to" in out
        # "found X: yes" is the board answering through the link about its own
        # screen - the strongest evidence available that it is still awake.
        # ": yes" and not a captured word: the console gives no newlines, so
        # "found X: yes" runs straight into "done 0" and a \w+ grabs
        # "yesdone".
        # ": yes" rather than a captured word: the console gives no
        # newlines, so "found X: yes" runs straight into "done 0" and a
        # word match grabs "yesdone".
        answered = re.search(r"found [^\n]{0,40}?: yes", out) is not None
        if not linked or not answered:
            unanswered += 1
            say("%3u %-24s NO ANSWER  (%s, %.0f s)"
                  % (n, name, "linked" if linked else "not linked", took))
            if unanswered <= 2:
                say("      got: %r" % out[:220])
        else:
            if took > worst:
                worst, worst_what = took, name
            if "crash" in name or "null call" in name:
                crashes_survived += 1

        if time.time() - round_started > 180:
            say("%3u %-24s the harness itself is stuck; stopping" % (n, name))
            unanswered += 1
            break

        stop_everything(sv)
        free, largest = board_memory(sv)
        low_free = min(low_free, free)
        low_largest = min(low_largest, largest)

        if n % 5 == 0 or not linked:
            left = int(deadline - time.time())
            say("%3u %-24s %5.1f s   %2u/%2u KB   %u min left"
                  % (n, name, took, free, largest, max(0, left // 60)))

    say("\n%u rounds in %u minutes" % (n, args.minutes))
    say("unanswered rounds: %u" % unanswered)
    say("crashes survived : %u" % crashes_survived)
    say("slowest answer   : %.1f s (%s)" % (worst, worst_what))
    say("memory low water : %u KB free, largest %u KB"
          % (low_free, low_largest))
    say("memory at start  : %u KB free, largest %u KB" % (free0, largest0))

    stop_everything(sv)
    cl.close()
    sv.close()
    return 1 if unanswered else 0


if __name__ == "__main__":
    sys.exit(main())
