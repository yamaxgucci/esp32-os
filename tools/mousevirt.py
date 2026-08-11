#!/usr/bin/env python3
"""Send host mouse position/buttons to ArgonOS /dev/mouse0 (MOUSEVIRT.SYS).

Guest listens on TCP :5560 (QEMU hostfwd).  8-byte LE packets:

  type(1) buttons(1) x(i16) y(i16) wheel(i8) pad(1)

Maps the primary monitor to the guest framebuffer (default 640x400).
Hold Right-Ctrl to pause sending (so you can use the host freely).
Esc in this console quits.

Typical:

  # guest (once):  drv install h:\\mousevirt.sys
  # guest:         run h:\\grain.axe pcmvirt
  python tools/mousevirt.py --reconnect
"""

from __future__ import annotations

import argparse
import ctypes
import socket
import struct
import sys
import time

VK_ESCAPE = 0x1B
VK_RBUTTON = 0x02
VK_MBUTTON = 0x04
VK_LBUTTON = 0x01
VK_RCONTROL = 0xA3


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def pack_pkt(typ: int, buttons: int, x: int, y: int, wheel: int = 0) -> bytes:
    return struct.pack("<BBhhbB", typ & 0xFF, buttons & 0xFF, int(x), int(y), int(wheel), 0)


def connect(host: str, port: int, timeout: float) -> socket.socket:
    s = socket.create_connection((host, port), timeout=timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5560)
    ap.add_argument("--width", type=int, default=640, help="guest FB width")
    ap.add_argument("--height", type=int, default=400, help="guest FB height")
    ap.add_argument("--reconnect", action="store_true")
    ap.add_argument("--hz", type=float, default=60.0)
    args = ap.parse_args()

    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32
    hwnd = kernel32.GetConsoleWindow()

    sw = user32.GetSystemMetrics(0)
    sh = user32.GetSystemMetrics(1)
    if sw < 1:
        sw = 1920
    if sh < 1:
        sh = 1080

    period = 1.0 / max(args.hz, 1.0)
    sock: socket.socket | None = None
    prev_btn = 0
    prev_x = -1
    prev_y = -1

    print(
        f"mousevirt → {args.host}:{args.port}  map {sw}x{sh} → "
        f"{args.width}x{args.height}  (RCtrl=pause, Esc=quit)"
    )

    def focused() -> bool:
        if not hwnd:
            return True
        return user32.GetForegroundWindow() == hwnd

    while True:
        if sock is None:
            try:
                sock = connect(args.host, args.port, 2.0)
                print("connected")
            except OSError as e:
                if not args.reconnect:
                    print(f"connect failed: {e}", file=sys.stderr)
                    return 1
                time.sleep(0.5)
                continue

        if focused() and (user32.GetAsyncKeyState(VK_ESCAPE) & 0x8000):
            print("quit")
            break

        paused = bool(user32.GetAsyncKeyState(VK_RCONTROL) & 0x8000)
        if not paused:
            pt = POINT()
            user32.GetCursorPos(ctypes.byref(pt))
            gx = int(pt.x * args.width / sw)
            gy = int(pt.y * args.height / sh)
            if gx < 0:
                gx = 0
            if gy < 0:
                gy = 0
            if gx >= args.width:
                gx = args.width - 1
            if gy >= args.height:
                gy = args.height - 1

            buttons = 0
            if user32.GetAsyncKeyState(VK_LBUTTON) & 0x8000:
                buttons |= 1
            if user32.GetAsyncKeyState(VK_RBUTTON) & 0x8000:
                buttons |= 2
            if user32.GetAsyncKeyState(VK_MBUTTON) & 0x8000:
                buttons |= 4

            if gx != prev_x or gy != prev_y or buttons != prev_btn:
                pkt = pack_pkt(1, buttons, gx, gy, 0)
                try:
                    sock.sendall(pkt)
                except OSError:
                    print("disconnected")
                    try:
                        sock.close()
                    except OSError:
                        pass
                    sock = None
                    if not args.reconnect:
                        return 1
                    time.sleep(0.3)
                    continue
                prev_x, prev_y, prev_btn = gx, gy, buttons

        time.sleep(period)

    if sock is not None:
        try:
            sock.close()
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
