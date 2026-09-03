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
    move X,Y        put the pointer here, in the guest's own pixels - the
                    *panel's*, which is not the picture's.  Fallout draws
                    640x480 into a 320x240 panel and the shim doubles what
                    arrives, so "move 500,300" on that board is game 1000,600
                    and lands off the screen; halve the game coordinate first.
                    Nothing clips it for you, and an off-screen click is
                    silent.  "-recinput" makes the game print both numbers,
                    panel and game, for every press - which is the way to
                    settle it on a machine whose panel you do not know
    wheel N         turn the wheel N notches, negative the other way
    home            slam the pointer into the top-left corner, so that the
                    next glide starts from a position that is actually known
    glide X,Y       travel there in small steps, so it can be watched
    click / rclick  press and release a button where the pointer is
    say TEXT        type it, one character at a time

--wait N holds off until the guest is listening, for up to N seconds: the game
this was written for takes ten minutes to reach its menu, and connecting before
the driver is loaded gets a refusal rather than a queue.

On a real board there is a second reason for --wait, and it looked for a whole
session like a broken driver: **the driver only starts listening once something
opens the device.**  ensure_listen needs an address, so it cannot bind at load
time on a board that is still getting one from DHCP, and it is retried from the
open and the read - which nothing calls until an application asks for a
pointer.  So load the drivers, start the application, and only then connect:
before that the port is refused and the board looks dead to this tool.

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

    def __init__(self, wait_s: float, host: str = "127.0.0.1"):
        self.wait_s = wait_s
        # The board answers on its own address; QEMU forwards to localhost.
        self.host = host
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
                s = socket.create_connection((self.host, port), timeout=2)
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

    def home(self, w: int = 639, h: int = 479) -> None:
        """Put the pointer in the top-left corner, and know that it is there.

        This is the difference between a replay that works and one that clicks
        on nothing.  The shim hands the engine *movement*, not position: it
        subtracts the last absolute position from this one and Fallout adds
        that to a cursor it tracks itself.  So a single absolute packet does
        not put the cursor anywhere - it shoves it by the difference, from
        wherever it already was, and where it already was is not knowable from
        out here.

        One jump to the far corner and one back is enough to fix it.  The
        second carries a delta of nearly the whole screen in both axes, the
        engine clamps at the edge, and the cursor is then at 0,0 whatever it
        was doing before.
        """
        self.move(w, h)
        time.sleep(0.08)
        self.move(0, 0)
        time.sleep(0.08)

    def glide(self, x: int, y: int, steps: int = 24) -> None:
        """Travel there in small steps, the way a hand would.

        The deltas add up to the same total as one jump, so this is not needed
        for correctness - it is needed for watching.  A cursor that crosses the
        screen visibly can be followed, and a replay that ends up in the wrong
        place shows *where* it went wrong rather than only that it did.
        """
        x0, y0 = self.x, self.y
        for i in range(1, steps + 1):
            self.move(x0 + (x - x0) * i // steps, y0 + (y - y0) * i // steps)
            time.sleep(0.02)

    def wheel(self, notches: int) -> None:
        """Turn the wheel, one notch at a time.

        Fallout scrolls the map by the wheel, which is the only way to move
        the view without a hand on the mouse: the engine's own edge-scrolling
        wants the pointer held against the border, and a pointer put there by
        a single absolute packet is not held anywhere.  The wheel byte is
        signed and momentary - it is reported with a packet and gone - so each
        notch is its own packet and the count is spread over time rather than
        summed, which is what the engine expects from a real wheel.
        """
        s = self.pointer()
        step = 1 if notches >= 0 else -1
        for _ in range(abs(notches)):
            s.sendall(mouse_packet(self.buttons, self.x, self.y, step))
            time.sleep(0.05)
        s.sendall(mouse_packet(self.buttons, self.x, self.y, 0))

    def button(self, mask: int) -> None:
        s = self.pointer()
        self.buttons |= mask
        s.sendall(mouse_packet(self.buttons, self.x, self.y))
        time.sleep(0.12)
        self.buttons &= ~mask
        s.sendall(mouse_packet(self.buttons, self.x, self.y))


def play(guest: Guest, path: str, speed: float = 1.0) -> None:
    """Send a recorded session back, packet for packet, at its own pace.

    The file is what mousevirt/kbdvirt actually put on the wire, which is the
    only faithful record of a session: the coordinates a person sees on the
    host and the ones the guest's cursor ends up at are not the same numbers,
    and only the wire's half can be sent again.

    The pointer is homed first.  The stream carries absolute positions and the
    shim turns them into movement, so where the cursor ends up depends on
    where it started; homing makes that a known place instead of whatever the
    last run left behind.
    """
    events = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "M" and len(parts) == 7:
                _, ms, typ, btn, x, y, wh = parts
                events.append((int(ms), "M", int(typ), int(btn), int(x),
                               int(y), int(wh)))
            elif parts[0] == "K" and len(parts) == 5:
                _, ms, typ, hid, mods = parts
                events.append((int(ms), "K", int(typ), int(hid), int(mods)))
    events.sort(key=lambda e: e[0])
    if not events:
        raise SystemExit("inputplay: %s holds no events" % path)

    guest.home()
    print("inputplay: replaying %d events from %s" % (len(events), path))
    t0 = time.monotonic()
    base = events[0][0]
    for ev in events:
        due = (ev[0] - base) / 1000.0 / max(speed, 0.01)
        lag = due - (time.monotonic() - t0)
        if lag > 0:
            time.sleep(lag)
        if ev[1] == "M":
            _, _, typ, btn, x, y, wh = ev
            guest.pointer().sendall(
                struct.pack("<BBhhbB", typ, btn, x, y, wh, 0))
            guest.x, guest.y, guest.buttons = x, y, btn
        else:
            _, _, typ, hid, mods = ev
            guest.keyboard().sendall(
                struct.pack("<BBHHBB", typ, mods, hid, 0, 0, 0))
    print("inputplay: replay done")


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
        elif verb == "wheel":
            guest.wheel(int(arg))
        elif verb == "home":
            guest.home()
        elif verb == "glide":
            x, _, y = arg.partition(",")
            guest.glide(int(x), int(y))
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
    ap.add_argument("commands", nargs="*", help="what to do, in order")
    ap.add_argument("--play", default="",
                    help="replay a file recorded by mousevirt/kbdvirt "
                         "--record instead of running commands")
    ap.add_argument("--speed", type=float, default=1.0,
                    help="replay faster (2 = twice) or slower (0.5)")
    ap.add_argument("--host", default="127.0.0.1",
                    help="where the guest is: localhost for QEMU, the "
                         "board's own address for a board")
    ap.add_argument("--wait", type=float, default=60.0,
                    help="seconds to wait for the guest to start listening")
    args = ap.parse_args()

    guest = Guest(args.wait, args.host)
    if args.play:
        play(guest, args.play, args.speed)
    else:
        run(guest, args.commands)
    return 0


if __name__ == "__main__":
    sys.exit(main())
