#!/usr/bin/env python3
"""ArgonOS - the PC at the other end of the cable, when there is no cable.

USBHIDVIRT.SYS publishes /dev/usbhid0 in the emulator and forwards every HID
report it is given to TCP :5562 (QEMU hostfwd).  This is what listens: it takes
the reports apart and prints what a real computer would have understood by
them - keys pressed and released, the pointer moving, buttons going down.

    python tools/usbhidvirt.py                 watch
    python tools/usbhidvirt.py --expect hello  watch, and exit 0 only if that
                                               text was typed

The second form is the point.  Everything above the port - the page, the
WebSocket, the wire format, the report builder, the order reports go out in -
can be verified without a board, and this is the thing that says whether it
was.  What it cannot say is whether a real host agrees; that is what plugging
the board into a PC is for.

A report is a *state*, not an event: it says which keys are down now.  So a
keystroke is a difference between two consecutive reports, which is exactly the
place a report builder's bugs live and exactly why this decodes rather than
dumps.

Frame on the wire (16 bytes, the ABI's ag_usbhid_frame_t):

    [0]    kind: 1 keyboard, 3 absolute pointer (2 was a relative mouse)
    [1]    length of the report
    [2..]  the report itself

Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
"""

import argparse
import socket
import sys
import time

FRAME = 16
# 2 was a relative mouse and is gone: the far machine has one pointer and it
# is absolute.  See docs/14-usb-kvm.md on the endpoint budget that decided it.
KIND_KBD, KIND_ABS = 1, 3

MODS = [
    (0x01, "Ctrl"),
    (0x02, "Shift"),
    (0x04, "Alt"),
    (0x08, "Win"),
    (0x10, "RCtrl"),
    (0x20, "RShift"),
    (0x40, "RAlt"),
    (0x80, "RWin"),
]

# Usage code to a name, for the keys that have no character.  The letters and
# the digits are filled in below rather than typed out.
NAMED = {
    0x28: "Enter", 0x29: "Esc", 0x2A: "Backspace", 0x2B: "Tab",
    0x2C: "Space", 0x2D: "-", 0x2E: "=", 0x2F: "[", 0x30: "]",
    0x31: "\\", 0x33: ";", 0x34: "'", 0x35: "`", 0x36: ",", 0x37: ".",
    0x38: "/", 0x39: "CapsLock", 0x46: "PrintScreen", 0x47: "ScrollLock",
    0x48: "Pause", 0x49: "Insert", 0x4A: "Home", 0x4B: "PageUp",
    0x4C: "Delete", 0x4D: "End", 0x4E: "PageDown", 0x4F: "Right",
    0x50: "Left", 0x51: "Down", 0x52: "Up", 0x53: "NumLock",
    0x65: "Menu", 0x01: "RolloverError",
}
for _i in range(26):
    NAMED[0x04 + _i] = chr(ord("a") + _i)
for _i in range(1, 10):
    NAMED[0x1E + _i - 1] = str(_i)
NAMED[0x27] = "0"
for _i in range(1, 13):
    NAMED[0x3A + _i - 1] = "F%d" % _i

# What each key produces as text, unshifted and shifted, so that a run of
# keystrokes can be compared with the words somebody meant to type.
SHIFTED = {
    "-": "_", "=": "+", "[": "{", "]": "}", "\\": "|", ";": ":",
    "'": '"', "`": "~", ",": "<", ".": ">", "/": "?",
    "1": "!", "2": "@", "3": "#", "4": "$", "5": "%", "6": "^",
    "7": "&", "8": "*", "9": "(", "0": ")",
}


def key_name(usage):
    return NAMED.get(usage, "0x%02X" % usage)


def key_text(usage, mods):
    """The character this key produces, or None if it makes no character."""
    name = NAMED.get(usage)
    if name is None:
        return None
    shift = bool(mods & 0x22)
    if usage == 0x2C:
        return " "
    if usage == 0x28:
        return "\n"
    if len(name) == 1 and name.isalpha():
        return name.upper() if shift else name
    if len(name) == 1:
        return SHIFTED.get(name, name) if shift else name
    return None


def mod_names(mods):
    return "+".join(n for bit, n in MODS if mods & bit)


class Watcher:
    def __init__(self, quiet=False):
        self.quiet = quiet
        self.held = set()
        self.mods = 0
        self.typed = []
        self.counts = {KIND_KBD: 0, KIND_ABS: 0}
        self.buttons = 0

    def say(self, text):
        if not self.quiet:
            print(text, flush=True)

    def keyboard(self, rep):
        mods = rep[0]
        keys = {b for b in rep[2:8] if b}

        if mods != self.mods:
            gone = mod_names(self.mods & ~mods)
            came = mod_names(mods & ~self.mods)
            if came:
                self.say("  mod  %s down" % came)
            if gone:
                self.say("  mod  %s up" % gone)
            self.mods = mods

        for u in sorted(keys - self.held):
            ch = key_text(u, mods)
            self.say("  key  %s down%s" % (key_name(u),
                                           "   -> %r" % ch if ch else ""))
            if ch:
                self.typed.append(ch)
        for u in sorted(self.held - keys):
            self.say("  key  %s up" % key_name(u))
        self.held = keys

    def absolute(self, rep):
        x = rep[1] | (rep[2] << 8)
        y = rep[3] | (rep[4] << 8)
        wheel = rep[5] - 256 if rep[5] > 127 else rep[5]
        btn = rep[0]
        parts = ["at %d,%d" % (x, y)]
        if wheel:
            parts.append("wheel %+d" % wheel)
        if btn != self.buttons:
            for bit, name in ((1, "left"), (2, "right"), (4, "middle")):
                if (btn & bit) and not (self.buttons & bit):
                    parts.append("%s down" % name)
                elif (self.buttons & bit) and not (btn & bit):
                    parts.append("%s up" % name)
            self.buttons = btn
        self.say("  ptr  %s" % ", ".join(parts))

    def frame(self, f):
        kind, length = f[0], f[1]
        rep = f[2:2 + length]
        if kind in self.counts:
            self.counts[kind] += 1
        if kind == KIND_KBD and length >= 8:
            self.keyboard(rep)
        elif kind == KIND_ABS and length >= 6:
            self.absolute(rep)
        else:
            self.say("  ???  kind %d len %d %s" % (kind, length, rep.hex()))

    def text(self):
        return "".join(self.typed)


def connect(host, port, timeout):
    deadline = time.time() + timeout
    while True:
        try:
            s = socket.create_connection((host, port), 2.0)
            s.settimeout(0.5)
            return s
        except OSError:
            if time.time() >= deadline:
                return None
            time.sleep(0.25)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5562)
    ap.add_argument("--wait", type=float, default=20.0,
                    help="seconds to keep trying to connect")
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="stop after this long (0 = until Ctrl+C or EOF)")
    ap.add_argument("--expect", default=None,
                    help="exit 0 only if this text was typed")
    ap.add_argument("--leds", type=lambda v: int(v, 0), default=None,
                    help="tell the guest the host's lock lamps are this byte")
    ap.add_argument("--quiet", action="store_true",
                    help="only the summary")
    args = ap.parse_args()

    sock = connect(args.host, args.port, args.wait)
    if sock is None:
        print("no USBHIDVIRT on %s:%d (is the guest running, and the driver "
              "installed?)" % (args.host, args.port), file=sys.stderr)
        return 2

    print("cable in: %s:%d" % (args.host, args.port), flush=True)
    if args.leds is not None:
        sock.sendall(bytes([args.leds & 0xFF]))

    w = Watcher(args.quiet)
    buf = b""
    end = time.time() + args.seconds if args.seconds > 0 else None

    try:
        while True:
            if end is not None and time.time() >= end:
                break
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                print("cable out", flush=True)
                break
            buf += chunk
            while len(buf) >= FRAME:
                w.frame(buf[:FRAME])
                buf = buf[FRAME:]
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()

    typed = w.text()
    print("\n%d keyboard, %d pointer; typed %r"
          % (w.counts[KIND_KBD], w.counts[KIND_ABS], typed), flush=True)

    if args.expect is not None:
        if args.expect in typed:
            print("OK: found %r" % args.expect)
            return 0
        print("FAIL: expected %r, got %r" % (args.expect, typed),
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
