"""Type on this machine's keyboard, into the game on the board.

`inputplay.py` sends a script of keys and exits; this sends whatever is typed,
as it is typed, for as long as it is left running.  It is the difference
between driving the game from a test and playing it - saving, above all, which
on a board with no keyboard attached is otherwise impossible.

    python tools/livekeys.py --host 192.168.0.111

Keys go where the game expects them: F6 quick-saves, F7 quick-loads, Escape
opens the menu.  Ctrl+] quits the bridge without sending anything.

Only KBDVIRT is needed on the board (`drv load a:\\kbdvirt.sys`); it listens on
TCP 5561 and the game sees an ordinary keyboard.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import inputplay  # noqa: E402  - the transport and the HID table both live there

if os.name != "nt":
    raise SystemExit("livekeys: this reads the console raw, so Windows only")

import msvcrt  # noqa: E402

# What a console hands over, as HID names inputplay already knows.
PLAIN = {
    b"\r": "enter",
    b"\n": "enter",
    b"\x1b": "esc",
    b"\x08": "backspace",
    b"\t": "tab",
    b" ": "space",
}

# The two-byte keys: msvcrt returns a 0x00 or 0xE0 lead byte and then this.
EXTENDED = {
    b"H": "up", b"P": "down", b"K": "left", b"M": "right",
    b";": "f1", b"<": "f2", b"=": "f3", b">": "f4", b"?": "f5",
    b"@": "f6", b"A": "f7", b"B": "f8", b"C": "f9", b"D": "f10",
    b"\x85": "f11", b"\x86": "f12",
}

QUIT = b"\x1d"          # Ctrl+] - not a key any game wants


def name_of(ch):
    """The HID name for one keypress, or None if there is no sending it."""
    if ch in (b"\x00", b"\xe0"):
        return EXTENDED.get(msvcrt.getch())
    if ch in PLAIN:
        return PLAIN[ch]
    try:
        text = ch.decode("latin1").lower()
    except Exception:
        return None
    return text if text in inputplay.HID else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.0.111",
                    help="the board's address")
    ap.add_argument("--wait", type=float, default=20.0,
                    help="seconds to wait for the guest to open its socket")
    args = ap.parse_args()

    guest = inputplay.Guest(args.wait, args.host)
    print("livekeys: talking to %s:%d" % (args.host, inputplay.KBD_PORT))
    print("          F6 saves, F7 loads, Esc is the menu; Ctrl+] quits.")
    print("          Keep this window focused - it is the keyboard.")

    sent = 0
    while True:
        ch = msvcrt.getch()
        if ch == QUIT:
            break
        name = name_of(ch)
        if name is None:
            continue
        try:
            guest.send_key(name, True, True)
        except Exception as exc:
            print("\r\nlivekeys: %s" % exc)
            break
        sent += 1
        sys.stdout.write("\r%s (%d sent)          " % (name, sent))
        sys.stdout.flush()
        # A game reads the keyboard once a frame; two presses inside one frame
        # are one press as far as it is concerned.
        time.sleep(0.03)

    print("\r\nlivekeys: %d keys sent" % sent)


main()
