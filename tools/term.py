#!/usr/bin/env python3
"""A tiny VT100 serial terminal for the ArgonOS console on Windows.

miniterm shows the console's cursor-positioning escapes as garbage because the
classic Windows console does not interpret them.  This turns on
ENABLE_VIRTUAL_TERMINAL_PROCESSING (pitfall 5) so the 40x25 screen renders, and
passes keystrokes straight through so a password can be typed by hand.

    python tools/term.py COM4 [115200]

Quit with Ctrl+]  (the board keeps running).
"""
import sys, threading, ctypes

try:
    import serial
except ImportError:
    sys.exit("term: pyserial missing - run with the IDF python (argon env).")

import msvcrt

def enable_vt():
    k = ctypes.windll.kernel32
    h = k.GetStdHandle(-11)  # STDOUT
    mode = ctypes.c_uint()
    if k.GetConsoleMode(h, ctypes.byref(mode)):
        k.SetConsoleMode(h, mode.value | 0x0004)  # VIRTUAL_TERMINAL_PROCESSING

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
    enable_vt()
    ser = serial.Serial(port, baud, timeout=0.05, xonxoff=True, write_timeout=10)
    # Do not drive the reset lines: the board is already running.
    try:
        ser.rts = False
        ser.dtr = False
    except Exception:
        pass

    sys.stdout.write("\r\n[term] connected to %s at %d - Ctrl+] to quit\r\n" %
                     (port, baud))
    sys.stdout.flush()
    ser.write(b"\r")  # ask for a prompt

    stop = threading.Event()

    def reader():
        while not stop.is_set():
            data = ser.read(4096)
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    try:
        while not stop.is_set():
            ch = msvcrt.getwch()
            if ch == "\x1d":  # Ctrl+]
                break
            if ch in ("\x00", "\xe0"):  # a function/arrow key: read and drop
                msvcrt.getwch()
                continue
            if ch == "\r":
                ser.write(b"\r")
            elif ch == "\x08":  # backspace
                ser.write(b"\x08")
            else:
                ser.write(ch.encode("latin-1", "replace"))
    finally:
        stop.set()
        ser.close()
        sys.stdout.write("\r\n[term] closed\r\n")
        sys.stdout.flush()

if __name__ == "__main__":
    main()
