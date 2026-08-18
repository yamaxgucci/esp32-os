#!/usr/bin/env python3
"""
Run a board the way `argon test` runs the emulator.

The point is not convenience.  Every hardware result this project has is going
to be quoted somewhere, and a result typed into a terminal by hand cannot be
repeated by anyone else - including by the person who typed it, a week later.
So the board gets the same shape of harness the emulator has: reset it, wait
for the prompt, send a list of commands, keep every byte that came back, and
render the final screen through the kernel's own screen module (build-host
\\vtdump.exe), which is the only thing that knows what the bytes meant.

    python tools/boardtest.py -p COM3 ver mem
    python tools/boardtest.py -p COM3 --seconds 8          (just watch it boot)
    python tools/boardtest.py -p COM3 "io 22 out" "io 22 1" "io 27"

Raw bytes go to build/board.log, byte for byte: the transcript is written in
binary and nothing in this file decodes it.  Trap 22 was exactly this mistake
made in the emulator harness - a transcript written as text, so every byte
above 0x7f arrived as two - and it cost an afternoon of blaming the renderer
for a fault in the tool that was checking it.

`~\\xNN` as a command sends raw bytes instead of a line, same as `argon test`.
"""
import argparse
import os
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("boardtest: pyserial is missing.  Run this with the IDF python "
             "(argon env), which has it because esptool needs it.")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "board.log")

# The shell prompt, as it reaches the wire: "C:\>" or "(1) C:\>" with a slot.
PROMPT = re.compile(rb"(?:\(\d+\) )?[A-Z]:\\[^\r\n\x1b]*>\s*$")

# What the renderer puts around it.  A prompt does not arrive as a prompt: it
# arrives as "move to row 20, print, erase to end of line, move to column 9",
# because what comes down this wire is a screen being drawn and not a log.
ANSI = re.compile(rb"\x1b(?:\[[0-9;?]*[ -/]*[@-~]|[()][A-Za-z0-9]|[@-Z\\-_])")


def visible(data):
    """The characters a person would see, with the escapes taken out."""
    return ANSI.sub(b"", data).replace(b"\x00", b"")


def unescape(text):
    """`\\xNN`, `\\r`, `\\n` in an argument, resolved here rather than by the
    shell that started us - PowerShell 5.1 eats trailing backslashes and
    -replace does not call script blocks (trap 13)."""
    out = bytearray()
    i = 0
    while i < len(text):
        c = text[i]
        if c != "\\" or i + 1 >= len(text):
            out += c.encode("latin-1", "replace")
            i += 1
            continue
        n = text[i + 1]
        if n == "x" and i + 3 < len(text):
            out.append(int(text[i + 2:i + 4], 16))
            i += 4
        elif n == "r":
            out.append(0x0d)
            i += 2
        elif n == "n":
            out.append(0x0a)
            i += 2
        elif n == "e":
            out.append(0x1b)
            i += 2
        elif n == "\\":
            out.append(0x5c)
            i += 2
        else:
            # Not an escape this file knows: a backslash is a backslash.  Guest
            # paths are full of them (`copy d:\con c:\board.cfg`) and silently
            # eating one turns a path into a different, valid-looking path.
            out += c.encode("latin-1", "replace")
            out += n.encode("latin-1", "replace")
            i += 2
    return bytes(out)


class Board:
    def __init__(self, port, baud, log, echo):
        # XON/XOFF on, because the guest sends XOFF when its input buffer
        # fills and a sender that ignores it loses bytes *silently* - trap 17,
        # which arrived as a file that transferred cleanly and was wrong.
        self.ser = serial.Serial(port, baud, timeout=0.05, xonxoff=True)
        self.log = log
        self.echo = echo
        self.buf = bytearray()

    def reset(self):
        """Pulse EN through the auto-reset circuit every ESP32 board with a
        USB bridge has: DTR idle, RTS asserted holds the chip in reset.  IO0 is
        left alone, so it comes up running rather than in the ROM loader."""
        self.ser.dtr = False
        self.ser.rts = True
        time.sleep(0.1)
        self.ser.rts = False
        self.ser.reset_input_buffer()

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            data = self.ser.read(4096)
            if not data:
                continue
            self.buf += data
            self.log.write(data)
            self.log.flush()
            if self.echo:
                sys.stdout.write(data.decode("latin-1"))
                sys.stdout.flush()

    def wait_prompt(self, timeout):
        """True when the tail of what has arrived is a prompt.  Matching the
        tail rather than counting occurrences: the transcript is a rendered
        screen, and on a scroll old prompts are drawn again (trap 18)."""
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.1)
            tail = visible(bytes(self.buf[-400:]))
            if PROMPT.search(tail.rstrip(b" \r\n")):
                return True
        return False

    def send(self, data):
        self.ser.write(data)
        self.ser.flush()

    def wait_for(self, needle, timeout):
        """Wait for a string to appear in what has arrived since now."""
        mark = len(self.buf)
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.05)
            if needle in visible(bytes(self.buf[mark:])):
                return True
        return False

    def put(self, host_path, guest_path):
        """Deliver a file through `recv`, the hex path documented in
        05-status.md.  Slow (two characters on the wire per byte, and the
        guest echoes them back) but it is the only path a board has before it
        has a card, and it is the same one a person would use by hand."""
        with open(host_path, "rb") as f:
            data = f.read()

        self.send(b"recv " + guest_path.encode("latin-1") + b"\r")
        if not self.wait_for(b"send hex", 5.0):
            raise RuntimeError(f"recv {guest_path}: the guest never asked for hex")

        # 32 bytes a line: short enough that a lost line is cheap to see in the
        # transcript, long enough that the per-line echo is not the whole cost.
        for i in range(0, len(data), 32):
            chunk = data[i:i + 32].hex().encode("ascii")
            self.send(chunk + b"\r")
            # The handshake is the echo of the line itself.  Not a newline:
            # what comes back is a screen being redrawn, and the renderer
            # expresses the end of a line as a cursor move, not as \n.
            if not self.wait_for(chunk[-8:], 5.0):
                raise RuntimeError(f"recv {guest_path}: stalled at byte {i}")

        self.send(b"END\r")
        if not self.wait_for(b"bytes", 10.0):
            raise RuntimeError(f"recv {guest_path}: no summary line")

        # The guest says how much it wrote.  Comparing it with what we sent is
        # the only check that the transfer was not quietly short.
        tail = visible(bytes(self.buf[-400:]))
        got = re.search(rb"(\d+)\s+bytes", tail)
        if not got or int(got.group(1)) != len(data):
            raise RuntimeError(
                f"recv {guest_path}: guest wrote {got.group(1).decode() if got else '?'} "
                f"of {len(data)} bytes")
        print(f"[boardtest] put {os.path.basename(host_path)} -> {guest_path} "
              f"({len(data)} bytes)", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", required=True, help="COM3, /dev/ttyUSB0, ...")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=6.0,
                    help="how long to watch the boot before sending anything")
    ap.add_argument("--timeout", type=float, default=20.0,
                    help="how long one command may take")
    ap.add_argument("--no-reset", action="store_true",
                    help="attach to a board that is already running")
    ap.add_argument("--quiet", action="store_true", help="do not echo the wire")
    ap.add_argument("--put", action="append", default=[],
                    metavar="HOST=GUEST",
                    help="deliver a file through `recv` before the commands, "
                         "e.g. build\\apps\\DEVS.AXE=a:\\devs.axe")
    ap.add_argument("--log", default=LOG)
    ap.add_argument("commands", nargs="*", help="shell lines, or ~\\xNN raw bytes")
    args = ap.parse_args()

    os.makedirs(os.path.dirname(args.log), exist_ok=True)
    with open(args.log, "wb") as log:
        board = Board(args.port, args.baud, log, not args.quiet)
        if not args.no_reset:
            board.reset()

        if args.no_reset:
            # Nothing is coming unprompted from a board that is sitting at a
            # prompt; ask for one.
            board.send(b"\r")

        booted = board.wait_prompt(args.seconds)
        if not booted:
            # Not necessarily broken: it may simply be slower than asked for.
            # Say so, and keep the transcript - but do not start sending.  A
            # board that is not at a prompt is usually one that is still inside
            # the last command, and every line sent to it then is read as an
            # argument to that command rather than as a command.  An aborted
            # `recv` swallowing the next `recv` is how this was found.
            print(f"\n[boardtest] no prompt within {args.seconds}s", flush=True)
            if args.put or args.commands:
                print("[boardtest] refusing to send into an unknown state; "
                      "run without --no-reset", flush=True)
                return 2

        for spec in args.put:
            host, _, guest = spec.partition("=")
            if not guest:
                sys.exit(f"boardtest: --put wants HOST=GUEST, got '{spec}'")
            board.put(host, guest)

        failed = []
        for cmd in args.commands:
            if cmd.startswith("!wait "):
                # `run` starts an application and comes straight back - the
                # prompt is not the end of the work, it is the beginning of it.
                # Nothing here can tell when the application has finished, so
                # the caller says how long to watch.
                board.pump(float(cmd.split(None, 1)[1]))
                continue
            if cmd.startswith("~"):
                board.send(unescape(cmd[1:]))
                board.pump(1.0)
                continue
            board.send(cmd.encode("latin-1", "replace") + b"\r")
            if not board.wait_prompt(args.timeout):
                failed.append(cmd)

        board.pump(0.3)

    print(f"\n[boardtest] transcript: {args.log} ({os.path.getsize(args.log)} bytes)")
    if failed:
        print("[boardtest] no prompt after: " + ", ".join(failed))
        return 1
    return 0 if booted else 2


if __name__ == "__main__":
    sys.exit(main())
