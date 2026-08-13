#!/usr/bin/env python3
"""Send host keyboard to ArgonOS /dev/kbd0 (KBDVIRT.SYS).

Guest listens on TCP :5561 (QEMU hostfwd).  8-byte LE packets:

  type(1) mods(1) hid(u16) unicode(u16) repeat(1) pad(1)

GetAsyncKeyState is global, so keys work while the QEMU SDL window is
focused (that window is video-only).  Capture follows that window *or any
child* (SDL often focuses a child hwnd).  Click the QEMU RGB view to play,
click the serial console to type at A:\\>.  Right-Ctrl *toggles* a sticky
override if focus matching fails.  Ctrl+C is not sent to the guest.

Typical:

  python tools/kbdvirt.py --reconnect
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import socket
import struct
import sys
import time

# USB HID keyboard usage IDs (sdk/include/argon/keys.h)
HID_A = 0x04
HID_1 = 0x1E
HID_0 = 0x27
HID_ENTER = 0x28
HID_ESC = 0x29
HID_BACK = 0x2A
HID_TAB = 0x2B
HID_SPACE = 0x2C
HID_RIGHT = 0x4F
HID_LEFT = 0x50
HID_DOWN = 0x51
HID_UP = 0x52
HID_LCTRL = 0xE0
HID_LSHIFT = 0xE1
HID_LALT = 0xE2
HID_RCTRL = 0xE4
HID_RSHIFT = 0xE5
HID_RALT = 0xE6

VK_BACK = 0x08
VK_TAB = 0x09
VK_RETURN = 0x0D
VK_SHIFT = 0x10
VK_CONTROL = 0x11
VK_MENU = 0x12
VK_ESCAPE = 0x1B
VK_SPACE = 0x20
VK_LEFT = 0x25
VK_UP = 0x26
VK_RIGHT = 0x27
VK_DOWN = 0x28
VK_0 = 0x30
VK_1 = 0x31
VK_A = 0x41
VK_LSHIFT = 0xA0
VK_RSHIFT = 0xA1
VK_LCONTROL = 0xA2
VK_RCONTROL = 0xA3
VK_LMENU = 0xA4
VK_RMENU = 0xA5

AG_MOD_SHIFT = 1 << 0
AG_MOD_CTRL = 1 << 1
AG_MOD_ALT = 1 << 2

TYPE_DOWN = 1
TYPE_UP = 2

# VK → HID. RCtrl is the host pause key, not sent to the guest.
VK_TO_HID: dict[int, int] = {
    VK_RETURN: HID_ENTER,
    VK_ESCAPE: HID_ESC,
    VK_BACK: HID_BACK,
    VK_TAB: HID_TAB,
    VK_SPACE: HID_SPACE,
    VK_LEFT: HID_LEFT,
    VK_UP: HID_UP,
    VK_RIGHT: HID_RIGHT,
    VK_DOWN: HID_DOWN,
    VK_LSHIFT: HID_LSHIFT,
    VK_RSHIFT: HID_RSHIFT,
    VK_LCONTROL: HID_LCTRL,
    VK_LMENU: HID_LALT,
    VK_RMENU: HID_RALT,
}
for i in range(26):
    VK_TO_HID[VK_A + i] = HID_A + i
for i in range(9):
    VK_TO_HID[VK_1 + i] = HID_1 + i
VK_TO_HID[VK_0] = HID_0

WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
GA_ROOT = 2
SKIP_CLASSES = {
    "consolewindowclass",
    "cascadia_hosting_window_class",
    "virtualconsoleclass",
}


class RECT(ctypes.Structure):
    _fields_ = [
        ("left", ctypes.c_long),
        ("top", ctypes.c_long),
        ("right", ctypes.c_long),
        ("bottom", ctypes.c_long),
    ]


def hwnd_i(h: object | None) -> int:
    return int(h) if h else 0


def _text(user32: ctypes.WinDLL, hwnd: int, fn) -> str:
    buf = ctypes.create_unicode_buffer(256)
    if fn(hwnd, buf, 256) <= 0:
        return ""
    return buf.value


def window_title(user32: ctypes.WinDLL, hwnd: int) -> str:
    return _text(user32, hwnd, user32.GetWindowTextW)


def window_class(user32: ctypes.WinDLL, hwnd: int) -> str:
    return _text(user32, hwnd, user32.GetClassNameW)


def is_qemu_title(title: str) -> bool:
    low = title.lower()
    return "qemu" in low or "argonos" in low


def find_qemu_hwnd(user32: ctypes.WinDLL) -> int:
    """Prefer the largest non-console window whose title looks like QEMU.

    A PowerShell/Windows Terminal tab named qemu-run must not win over SDL.
    """
    best = 0
    best_area = -1
    holders: list[object] = []

    def _enum(hwnd: int, _lparam: int) -> bool:
        nonlocal best, best_area
        if not user32.IsWindowVisible(hwnd):
            return True
        if window_class(user32, hwnd).lower() in SKIP_CLASSES:
            return True
        if not is_qemu_title(window_title(user32, hwnd)):
            return True
        rc = RECT()
        area = 0
        if user32.GetClientRect(hwnd, ctypes.byref(rc)):
            area = int(rc.right - rc.left) * int(rc.bottom - rc.top)
        if area > best_area:
            best_area = area
            best = hwnd_i(hwnd)
        return True

    cb = WNDENUMPROC(_enum)
    holders.append(cb)
    user32.EnumWindows(cb, 0)
    return best


def focused_in_qemu(user32: ctypes.WinDLL, qemu_hwnd: int, fg: int) -> bool:
    if qemu_hwnd == 0 or fg == 0:
        return False
    if fg == qemu_hwnd:
        return True
    root = hwnd_i(user32.GetAncestor(wt.HWND(fg), GA_ROOT))
    if root == qemu_hwnd:
        return True
    parent = hwnd_i(user32.GetParent(wt.HWND(fg)))
    while parent:
        if parent == qemu_hwnd:
            return True
        parent = hwnd_i(user32.GetParent(wt.HWND(parent)))
    return False


def pack_pkt(typ: int, mods: int, hid: int, unicode: int = 0, repeat: int = 0) -> bytes:
    return struct.pack("<BBHHBB", typ, mods & 0xFF, hid & 0xFFFF, unicode & 0xFFFF,
                       repeat & 0xFF, 0)


def connect(host: str, port: int, timeout: float) -> socket.socket:
    s = socket.create_connection((host, port), timeout=timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def mods_now(user32: ctypes.WinDLL) -> int:
    m = 0
    if user32.GetAsyncKeyState(VK_SHIFT) & 0x8000:
        m |= AG_MOD_SHIFT
    if user32.GetAsyncKeyState(VK_CONTROL) & 0x8000:
        m |= AG_MOD_CTRL
    if user32.GetAsyncKeyState(VK_MENU) & 0x8000:
        m |= AG_MOD_ALT
    return m


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5561)
    ap.add_argument("--reconnect", action="store_true")
    ap.add_argument("--hz", type=float, default=60.0)
    args = ap.parse_args()

    user32 = ctypes.windll.user32
    user32.EnumWindows.argtypes = (WNDENUMPROC, wt.LPARAM)
    user32.EnumWindows.restype = wt.BOOL
    user32.IsWindowVisible.argtypes = (wt.HWND,)
    user32.IsWindowVisible.restype = wt.BOOL
    user32.GetWindowTextW.argtypes = (wt.HWND, wt.LPWSTR, ctypes.c_int)
    user32.GetWindowTextW.restype = ctypes.c_int
    user32.GetForegroundWindow.argtypes = ()
    user32.GetForegroundWindow.restype = wt.HWND
    user32.GetClassNameW.argtypes = (wt.HWND, wt.LPWSTR, ctypes.c_int)
    user32.GetClassNameW.restype = ctypes.c_int
    user32.GetClientRect.argtypes = (wt.HWND, ctypes.POINTER(RECT))
    user32.GetClientRect.restype = wt.BOOL
    user32.GetAncestor.argtypes = (wt.HWND, ctypes.c_uint)
    user32.GetAncestor.restype = wt.HWND
    user32.GetParent.argtypes = (wt.HWND,)
    user32.GetParent.restype = wt.HWND
    period = 1.0 / max(args.hz, 1.0)
    sock: socket.socket | None = None
    prev: dict[int, bool] = {vk: False for vk in VK_TO_HID}
    paused = True
    sticky = False
    rctrl_was = False
    qemu_hwnd = 0
    last_find = 0.0

    print(
        f"kbdvirt → {args.host}:{args.port}  "
        f"(click QEMU RGB to capture; RCtrl toggles sticky; Ctrl+C=quit)",
        flush=True,
    )

    while True:
        now_t = time.monotonic()
        if qemu_hwnd == 0 or (now_t - last_find) >= 1.0:
            qemu_hwnd = find_qemu_hwnd(user32)
            last_find = now_t

        fg = hwnd_i(user32.GetForegroundWindow())
        rctrl = bool(user32.GetAsyncKeyState(VK_RCONTROL) & 0x8000)
        if rctrl and not rctrl_was:
            sticky = not sticky
            print(f"sticky {'ON' if sticky else 'OFF'}", flush=True)
        rctrl_was = rctrl
        qemu_fg = focused_in_qemu(user32, qemu_hwnd, fg)
        want_paused = not (sticky or qemu_fg)
        if want_paused != paused:
            paused = want_paused
            if paused:
                for vk in prev:
                    if prev[vk]:
                        hid = VK_TO_HID[vk]
                        edges_up = pack_pkt(TYPE_UP, 0, hid)
                        if sock is not None:
                            try:
                                sock.send(edges_up)
                            except OSError:
                                pass
                    prev[vk] = False
                print("capture OFF (click QEMU RGB / RCtrl sticky)", flush=True)
            else:
                for vk, hid in VK_TO_HID.items():
                    prev[vk] = bool(user32.GetAsyncKeyState(vk) & 0x8000)
                print("capture ON", flush=True)

        edges: list[tuple[int, int, int]] = []
        for vk, hid in VK_TO_HID.items():
            now = (not paused) and bool(user32.GetAsyncKeyState(vk) & 0x8000)
            was = prev[vk]
            if now and not was:
                m = mods_now(user32)
                if hid == HID_A + (ord("c") - ord("a")) and (m & AG_MOD_CTRL):
                    prev[vk] = now
                    continue
                edges.append((TYPE_DOWN, hid, m))
            elif was and not now:
                edges.append((TYPE_UP, hid, 0))
            prev[vk] = now

        if sock is None:
            try:
                sock = connect(args.host, args.port, timeout=2.0)
                print("connected", flush=True)
            except OSError as e:
                if not args.reconnect:
                    print(f"connect failed: {e}", file=sys.stderr)
                    return 1
                time.sleep(0.75)
                continue

        for typ, hid, mods in edges:
            pkt = pack_pkt(typ, mods, hid)
            try:
                n = sock.send(pkt)
                if n != len(pkt):
                    raise OSError("short send")
            except OSError as e:
                print(f"disconnected: {e}", flush=True)
                try:
                    sock.close()
                except OSError:
                    pass
                sock = None
                for vk in prev:
                    prev[vk] = False
                if not args.reconnect:
                    return 1
                time.sleep(0.75)
                break

        time.sleep(period)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nbye", flush=True)
        raise SystemExit(0)
