#!/usr/bin/env python3
"""Send host mouse position/buttons to ArgonOS /dev/mouse0 (MOUSEVIRT.SYS).

Guest listens on TCP :5560 (QEMU hostfwd).  8-byte LE packets:

  type(1) buttons(1) x(i16) y(i16) wheel(i8) pad(1)

Maps the letterboxed framebuffer inside the QEMU SDL client area onto the
guest size (default 640×400).  Falls back to the primary monitor if no QEMU
window is found.  No WH_MOUSE hooks (those caused Win64 / grab regressions).

TCP: connect timeout is cleared after connect; sends are non-blocking so a
slow guest drops packets instead of flapping the session.

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

WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class RECT(ctypes.Structure):
    _fields_ = [
        ("left", ctypes.c_long),
        ("top", ctypes.c_long),
        ("right", ctypes.c_long),
        ("bottom", ctypes.c_long),
    ]


def _bind_user32() -> ctypes.WinDLL:
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.EnumWindows.argtypes = (WNDENUMPROC, wt.LPARAM)
    user32.EnumWindows.restype = wt.BOOL
    user32.IsWindowVisible.argtypes = (wt.HWND,)
    user32.IsWindowVisible.restype = wt.BOOL
    user32.IsWindow.argtypes = (wt.HWND,)
    user32.IsWindow.restype = wt.BOOL
    user32.GetWindowTextW.argtypes = (wt.HWND, wt.LPWSTR, ctypes.c_int)
    user32.GetWindowTextW.restype = ctypes.c_int
    user32.GetClientRect.argtypes = (wt.HWND, ctypes.POINTER(RECT))
    user32.GetClientRect.restype = wt.BOOL
    user32.ClientToScreen.argtypes = (wt.HWND, ctypes.POINTER(POINT))
    user32.ClientToScreen.restype = wt.BOOL
    user32.GetCursorPos.argtypes = (ctypes.POINTER(POINT),)
    user32.GetCursorPos.restype = wt.BOOL
    user32.GetAsyncKeyState.argtypes = (ctypes.c_int,)
    user32.GetAsyncKeyState.restype = ctypes.c_short
    user32.GetForegroundWindow.argtypes = ()
    user32.GetForegroundWindow.restype = wt.HWND
    user32.GetSystemMetrics.argtypes = (ctypes.c_int,)
    user32.GetSystemMetrics.restype = ctypes.c_int
    try:
        user32.SetProcessDPIAware.argtypes = ()
        user32.SetProcessDPIAware.restype = wt.BOOL
        user32.SetProcessDPIAware()
    except AttributeError:
        pass
    return user32


def pack_pkt(typ: int, buttons: int, x: int, y: int, wheel: int = 0) -> bytes:
    return struct.pack(
        "<BBhhbB", typ & 0xFF, buttons & 0xFF, int(x), int(y), int(wheel), 0
    )


def connect(host: str, port: int, timeout: float) -> socket.socket:
    s = socket.create_connection((host, port), timeout=timeout)
    # create_connection leaves timeout on the socket; that made send raise
    # under QEMU backlog and flap connected/disconnected while moving.
    s.settimeout(None)
    s.setblocking(False)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def find_qemu_hwnd(user32: ctypes.WinDLL) -> int:
    """First visible top-level window whose title mentions QEMU / ArgonOS."""
    found = wt.HWND(0)
    # Keep the callback alive for the duration of EnumWindows.
    holders: list[object] = []

    def _enum(hwnd: int, _lparam: int) -> bool:
        if not user32.IsWindowVisible(hwnd):
            return True
        buf = ctypes.create_unicode_buffer(256)
        if user32.GetWindowTextW(hwnd, buf, 256) <= 0:
            return True
        low = buf.value.lower()
        if "qemu" in low or "argonos" in low:
            found.value = hwnd
            return False
        return True

    cb = WNDENUMPROC(_enum)
    holders.append(cb)
    user32.EnumWindows(cb, 0)
    return int(found.value) if found.value else 0


def client_screen_rect(
    user32: ctypes.WinDLL, hwnd: int
) -> tuple[int, int, int, int] | None:
    """(screen_x, screen_y, width, height) of hwnd client area, or None."""
    if not hwnd or not user32.IsWindow(hwnd):
        return None
    rc = RECT()
    if not user32.GetClientRect(hwnd, ctypes.byref(rc)):
        return None
    w = int(rc.right - rc.left)
    h = int(rc.bottom - rc.top)
    if w < 8 or h < 8:
        return None
    pt = POINT(0, 0)
    if not user32.ClientToScreen(hwnd, ctypes.byref(pt)):
        return None
    return int(pt.x), int(pt.y), w, h


def letterbox(
    client_w: int, client_h: int, guest_w: int, guest_h: int
) -> tuple[int, int, int, int]:
    """Content rect (ox, oy, cw, ch) inside client that preserves guest aspect."""
    if client_w < 1 or client_h < 1 or guest_w < 1 or guest_h < 1:
        return 0, 0, max(client_w, 1), max(client_h, 1)
    scale = min(client_w / guest_w, client_h / guest_h)
    cw = max(int(guest_w * scale), 1)
    ch = max(int(guest_h * scale), 1)
    ox = (client_w - cw) // 2
    oy = (client_h - ch) // 2
    return ox, oy, cw, ch


def map_to_guest(
    screen_x: int,
    screen_y: int,
    origin_x: int,
    origin_y: int,
    content_x: int,
    content_y: int,
    content_w: int,
    content_h: int,
    guest_w: int,
    guest_h: int,
) -> tuple[int, int, bool]:
    """Return (gx, gy, inside).  gx/gy clamped when outside."""
    rel_x = screen_x - origin_x - content_x
    rel_y = screen_y - origin_y - content_y
    inside = 0 <= rel_x < content_w and 0 <= rel_y < content_h
    if content_w < 1:
        content_w = 1
    if content_h < 1:
        content_h = 1
    gx = int(rel_x * guest_w / content_w)
    gy = int(rel_y * guest_h / content_h)
    if gx < 0:
        gx = 0
    if gy < 0:
        gy = 0
    if gx >= guest_w:
        gx = guest_w - 1
    if gy >= guest_h:
        gy = guest_h - 1
    return gx, gy, inside


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

    user32 = _bind_user32()
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetConsoleWindow.argtypes = ()
    kernel32.GetConsoleWindow.restype = wt.HWND
    hwnd_console = kernel32.GetConsoleWindow()

    sw = user32.GetSystemMetrics(0)
    sh = user32.GetSystemMetrics(1)
    if sw < 1:
        sw = 1920
    if sh < 1:
        sh = 1080

    period = 1.0 / max(args.hz, 1.0)
    sock: socket.socket | None = None
    prev_btn = -1
    prev_x = -1
    prev_y = -1
    qemu_hwnd = 0
    last_find = 0.0
    map_mode = "monitor"
    warned_no_qemu = False

    print(
        f"mousevirt → {args.host}:{args.port}  guest {args.width}x{args.height}  "
        f"(RCtrl=pause, Esc=quit)",
        flush=True,
    )

    def console_focused() -> bool:
        if not hwnd_console:
            return True
        return int(user32.GetForegroundWindow()) == int(hwnd_console)

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

        if console_focused() and (user32.GetAsyncKeyState(VK_ESCAPE) & 0x8000):
            print("quit", flush=True)
            break

        paused = bool(user32.GetAsyncKeyState(VK_RCONTROL) & 0x8000)
        if not paused:
            origin_x, origin_y = 0, 0
            content_x, content_y = 0, 0
            content_w, content_h = sw, sh
            require_inside = False

            if not args.fullscreen_map:
                now = time.monotonic()
                if qemu_hwnd == 0 or (now - last_find) >= 2.0:
                    qemu_hwnd = find_qemu_hwnd(user32)
                    last_find = now
                    if qemu_hwnd == 0 and not warned_no_qemu:
                        print(
                            "QEMU window not found — mapping primary monitor "
                            "(start QEMU with a title containing 'qemu')",
                            flush=True,
                        )
                        warned_no_qemu = True
                        map_mode = "monitor"
                    elif qemu_hwnd != 0 and map_mode != "qemu":
                        print("mapping QEMU client (letterboxed)", flush=True)
                        map_mode = "qemu"
                        warned_no_qemu = False

                cr = client_screen_rect(user32, qemu_hwnd) if qemu_hwnd else None
                if cr is not None:
                    origin_x, origin_y, cw, ch = cr
                    content_x, content_y, content_w, content_h = letterbox(
                        cw, ch, args.width, args.height
                    )
                    require_inside = True

            pt = POINT()
            if not user32.GetCursorPos(ctypes.byref(pt)):
                time.sleep(period)
                continue

            gx, gy, inside = map_to_guest(
                int(pt.x),
                int(pt.y),
                origin_x,
                origin_y,
                content_x,
                content_y,
                content_w,
                content_h,
                args.width,
                args.height,
            )
            if require_inside and not inside:
                time.sleep(period)
                continue

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
