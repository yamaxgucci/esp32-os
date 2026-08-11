#!/usr/bin/env python3
"""Send host mouse position/buttons to ArgonOS /dev/mouse0 (MOUSEVIRT.SYS).

Guest listens on TCP :5560 (QEMU hostfwd).  8-byte LE packets:

  type(1) buttons(1) x(i16) y(i16) wheel(i8) pad(1)

By default maps the **QEMU window client area** to the guest framebuffer
(640×400).  Falls back to the primary monitor if QEMU is not found.
Hold Right-Ctrl to pause.  Esc in this console quits.

Typical:

  # guest (once):  drv install h:\\\\mousevirt.sys
  # guest:         run h:\\\\grain.axe pcmvirt
  python tools/mousevirt.py --reconnect
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import socket
import struct
import sys
import time

VK_ESCAPE = 0x1B
VK_RBUTTON = 0x02
VK_MBUTTON = 0x04
VK_LBUTTON = 0x01
VK_RCONTROL = 0xA3

user_WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class RECT(ctypes.Structure):
    _fields_ = [
        ("left", ctypes.c_long),
        ("top", ctypes.c_long),
        ("right", ctypes.c_long),
        ("bottom", ctypes.c_long),
    ]


def pack_pkt(typ: int, buttons: int, x: int, y: int, wheel: int = 0) -> bytes:
    return struct.pack("<BBhhbB", typ & 0xFF, buttons & 0xFF, int(x), int(y), int(wheel), 0)


def connect(host: str, port: int, timeout: float) -> socket.socket:
    s = socket.create_connection((host, port), timeout=timeout)
    # create_connection leaves timeout on the socket; that made sendall raise
    # under QEMU backlog and flap connected/disconnected while moving.
    s.settimeout(None)
    s.setblocking(False)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def find_qemu_hwnd(user32: ctypes.WinDLL) -> int:
    """Best-effort: first visible top-level window whose title mentions QEMU."""
    found = ctypes.c_void_p(0)

    def _enum(hwnd: int, _lparam: int) -> bool:
        if not user32.IsWindowVisible(hwnd):
            return True
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd, buf, 256)
        title = buf.value
        if not title:
            return True
        low = title.lower()
        if "qemu" in low or "argonos" in low:
            found.value = hwnd
            return False
        return True

    cb = WNDENUMPROC(_enum)
    user32.EnumWindows(cb, 0)
    return int(found.value) if found.value else 0


def map_rect(
    user32: ctypes.WinDLL, hwnd: int
) -> tuple[int, int, int, int] | None:
    """Return (screen_x, screen_y, width, height) of hwnd client area."""
    if not hwnd:
        return None
    rc = RECT()
    if not user32.GetClientRect(hwnd, ctypes.byref(rc)):
        return None
    w = rc.right - rc.left
    h = rc.bottom - rc.top
    if w < 8 or h < 8:
        return None
    pt = POINT(0, 0)
    if not user32.ClientToScreen(hwnd, ctypes.byref(pt)):
        return None
    return int(pt.x), int(pt.y), int(w), int(h)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5560)
    ap.add_argument("--width", type=int, default=640, help="guest FB width")
    ap.add_argument("--height", type=int, default=400, help="guest FB height")
    ap.add_argument("--reconnect", action="store_true")
    ap.add_argument("--hz", type=float, default=60.0)
    ap.add_argument(
        "--fullscreen-map",
        action="store_true",
        help="map primary monitor instead of QEMU client area",
    )
    args = ap.parse_args()

    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32
    hwnd_console = kernel32.GetConsoleWindow()

    user32.GetSystemMetrics.restype = ctypes.c_int
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
    map_label = "primary monitor" if args.fullscreen_map else "QEMU client (fallback: monitor)"
    last_map_log = 0.0
    qemu_hwnd = 0

    print(
        f"mousevirt → {args.host}:{args.port}  guest {args.width}x{args.height}  "
        f"map={map_label}  (RCtrl=pause, Esc=quit)"
    )

    def focused() -> bool:
        if not hwnd_console:
            return True
        return user32.GetForegroundWindow() == hwnd_console

    while True:
        if sock is None:
            try:
                sock = connect(args.host, args.port, 2.0)
                print("connected", flush=True)
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
            origin_x, origin_y, src_w, src_h = 0, 0, sw, sh
            if not args.fullscreen_map:
                now = time.monotonic()
                if qemu_hwnd == 0 or now - last_map_log >= 2.0:
                    qemu_hwnd = find_qemu_hwnd(user32)
                    last_map_log = now
                mr = map_rect(user32, qemu_hwnd)
                if mr is not None:
                    origin_x, origin_y, src_w, src_h = mr

            pt = POINT()
            user32.GetCursorPos(ctypes.byref(pt))
            rel_x = pt.x - origin_x
            rel_y = pt.y - origin_y
            if src_w < 1:
                src_w = 1
            if src_h < 1:
                src_h = 1
            gx = int(rel_x * args.width / src_w)
            gy = int(rel_y * args.height / src_h)
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
                    n = sock.send(pkt)
                    if n == len(pkt):
                        prev_x, prev_y, prev_btn = gx, gy, buttons
                    # partial / would-block: keep socket, retry next tick
                except BlockingIOError:
                    pass
                except OSError as e:
                    print(f"disconnected: {e}", flush=True)
                    try:
                        sock.close()
                    except OSError:
                        pass
                    sock = None
                    if not args.reconnect:
                        return 1
                    time.sleep(0.3)
                    continue

        time.sleep(period)

    if sock is not None:
        try:
            sock.close()
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
