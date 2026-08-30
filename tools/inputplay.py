#!/usr/bin/env python3
"""Drive the guest's keyboard and mouse from a script, with nobody at the desk.

kbdvirt.py and mousevirt.py mirror a person's hands: they read the host's real
input devices and need the QEMU window focused.  That is right for playing and
useless for a test, which has to press the same keys the same way while nothing
is watching.  This speaks the same two protocols and takes its input from the
command line.

The drivers must be loaded in the guest first - `drv install a:\\kbdvirt.sys`
and `drv install a:\\mousevirt.sys` - and they listen on TCP 5561 and 5560,
which QEMU forwards by default.

    python tools/inputplay.py --wait 600 "key n" "wait 5000" "key enter"
    python tools/inputplay.py "move 100,60" "click"

Commands:

    wait MS         do nothing for this long
    key NAME        press and release; NAME is a letter, a digit, or one of
                    esc enter space tab up down left right f1..f12
    down NAME       press without releasing
    up NAME         release
    move X,Y        put the pointer here, in the guest's own pixels
    click / rclick  press and release a button where the pointer is
    say TEXT        type it, one character at a time

--wait N holds off until the guest is listening, for up to N seconds: the game
this was written for takes ten minutes to reach its menu, and connecting before
the driver is loaded gets a refusal rather than a queue.

Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
import time

KBD_PORT = 5561
MOUSE_PORT = 5560

# USB HID usage ids, the same table as sdk/include/argon/keys.h.  The guest
# reports keys this way and SDL scancodes are the same numbers, which is why
# the port's translation is nearly the identity.
HID = {
    "a": 0x04, "b": 0x05, "c": 0x06, "d": 0x07, "e": 0x08, "f": 0x09,
    "g": 0x0A, "h": 0x0B, "i": 0x0C, "j": 0x0D, "k": 0x0E, "l": 0x0F,
    "m": 0x10, "n": 0x11, "o": 0x12, "p": 0x13, "q": 0x14, "r": 0x15,
    "s": 0x16, "t": 0x17, "u": 0x18, "v": 0x19, "w": 0x1A, "x": 0x1B,
    "y": 0x1C, "z": 0x1D,
    "1": 0x1E, "2": 0x1F, "3": 0x20, "4": 0x21, "5": 0x22,
    "6": 0x23, "7": 0x24, "8": 0x25, "9": 0x26, "0": 0x27,
    "enter": 0x28, "esc": 0x29, "backspace": 0x2A, "tab": 0x2B, "space": 0x2C,
    "f1": 0x3A, "f2": 0x3B, "f3": 0x3C, "f4": 0x3D, "f5": 0x3E, "f6": 0x3F,
    "f7": 0x40, "f8": 0x41, "f9": 0x42, "f10": 0x43, "f11": 0x44, "f12": 0x45,
    "right": 0x4F, "left": 0x50, "down": 0x51, "up": 0x52,
}

KEY_DOWN, KEY_UP = 1, 2
PTR_ABS, PTR_WHEEL = 1, 3

# Buttons as MOUSEVIRT packs them: bit 0 left, bit 1 right, bit 2 middle.
BTN_LEFT, BTN_RIGHT = 0x1, 0x2


def key_packet(typ: int, hid: int, unicode_: int) -> bytes:
    return struct.pack("<BBHHBB", typ, 0, hid, unicode_, 0, 0)


def mouse_packet(buttons: int, x: int, y: int, wheel: int = 0) -> bytes:
    return struct.pack("<BBhhbB", PTR_ABS, buttons, x, y, wheel, 0)


class Guest:
    """The two sockets, opened when first needed."""

    def __init__(self, wait_s: float):
        self.wait_s = wait_s
        self.kbd = None
        self.mouse = None
        self.x = 0
        self.y = 0
        self.buttons = 0

    def _connect(self, port: int, what: str):
        """A socket the guest is actually on the other end of.

        QEMU's user-mode forwarding accepts the host's connection itself and
        only then looks for a listener inside the guest.  So connect()
        succeeding proves nothing: if the driver is not listening yet, the
        first write is swallowed and the second gets a reset.  That is a
        keyboard which appears to be attached and drops every key, and it is
        worth the two extra packets here never to debug it again.

        The probe is a packet of zeroes.  Both drivers check the type field
        before doing anything with a packet, and zero is not a type either of
        them has, so it is seen and discarded - it cannot press anything.
        """
        deadline = time.monotonic() + self.wait_s
        probe = bytes(8)
        while True:
            s = None
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=2)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                s.sendall(probe)
                time.sleep(0.25)
                s.sendall(probe)
                print("inputplay: %s connected on %d" % (what, port))
                return s
            except OSError:
                if s is not None:
                    s.close()
                if time.monotonic() >= deadline:
                    raise SystemExit(
                        "inputplay: nothing listening on %d after %.0f s. In "
                        "the guest: `drv install a:\\%sVIRT.SYS`.  The driver "
                        "opens its socket once the network has an address, so "
                        "installing it before that waits for the lease."
                        % (port, self.wait_s, what.upper()))
                time.sleep(0.5)

    def keyboard(self):
        if self.kbd is None:
            self.kbd = self._connect(KBD_PORT, "kbd")
        return self.kbd

    def pointer(self):
        if self.mouse is None:
            self.mouse = self._connect(MOUSE_PORT, "mouse")
        return self.mouse

    def send_key(self, name: str, down: bool, up: bool) -> None:
        hid = HID.get(name.lower())
        if hid is None:
            raise SystemExit("inputplay: no key called %r" % name)
        # A letter carries its character as well as its usage id: the game
        # looks at the character for menu shortcuts and at the id for
        # everything else.
        uni = ord(name) if len(name) == 1 else 0
        s = self.keyboard()
        if down:
            s.sendall(key_packet(KEY_DOWN, hid, uni))
        if down and up:
            time.sleep(0.05)
        if up:
            s.sendall(key_packet(KEY_UP, hid, 0))

    def move(self, x: int, y: int) -> None:
        self.x, self.y = x, y
        self.pointer().sendall(mouse_packet(self.buttons, x, y))

    def button(self, mask: int) -> None:
        s = self.pointer()
        self.buttons |= mask
        s.sendall(mouse_packet(self.buttons, self.x, self.y))
        time.sleep(0.12)
        self.buttons &= ~mask
        s.sendall(mouse_packet(self.buttons, self.x, self.y))


def run(guest: Guest, commands: list) -> None:
    for raw in commands:
        parts = raw.split(None, 1)
        if not parts:
            continue
        verb = parts[0].lower()
        arg = parts[1].strip() if len(parts) > 1 else ""

        if verb == "wait":
            time.sleep(int(arg) / 1000.0)
        elif verb == "key":
            guest.send_key(arg, True, True)
        elif verb == "down":
            guest.send_key(arg, True, False)
        elif verb == "up":
            guest.send_key(arg, False, True)
        elif verb == "move":
            x, _, y = arg.partition(",")
            guest.move(int(x), int(y))
        elif verb == "click":
            guest.button(BTN_LEFT)
        elif verb == "rclick":
            guest.button(BTN_RIGHT)
        elif verb == "say":
            for ch in arg:
                guest.send_key(" " if ch == " " else ch, True, True)
                time.sleep(0.03)
        else:
            raise SystemExit("inputplay: no command called %r" % verb)
        print("inputplay: %s" % raw)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("commands", nargs="+", help="what to do, in order")
    ap.add_argument("--wait", type=float, default=60.0,
                    help="seconds to wait for the guest to start listening")
    args = ap.parse_args()

    run(Guest(args.wait), args.commands)
    return 0


if __name__ == "__main__":
    sys.exit(main())
