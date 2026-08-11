#!/usr/bin/env python3
"""Send host mouse position/buttons to ArgonOS /dev/mouse0 (MOUSEVIRT.SYS).

Guest listens on TCP :5560 (QEMU hostfwd).  8-byte LE packets:

  type(1) buttons(1) x(i16) y(i16) wheel(i8) pad(1)

Maps the letterboxed framebuffer inside the QEMU SDL client area onto the
guest size (default 640×400).

Anti-grab: a WH_MOUSE_LL hook on a **dedicated thread with a message pump**
swallows button/wheel over the QEMU client (not WM_MOUSEMOVE).  Installing the
hook on the sleepy main thread without PeekMessage made the host cursor crawl.
TCP is sent only from the main loop — never from the hook.

Hold Right-Ctrl to pause (clicks reach QEMU).  Esc / Ctrl+C quits.

Typical:

  python tools/mousevirt.py --reconnect
"""

from __future__ import annotations

import argparse
import atexit
import ctypes
import ctypes.wintypes as wt
import socket
import struct
import sys
import threading
import time

VK_ESCAPE = 0x1B
VK_RBUTTON = 0x02
VK_MBUTTON = 0x04
VK_LBUTTON = 0x01
VK_RCONTROL = 0xA3

WH_MOUSE_LL = 14
WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_RBUTTONDOWN = 0x0204
WM_RBUTTONUP = 0x0205
WM_MBUTTONDOWN = 0x0207
WM_MBUTTONUP = 0x0208
WM_MOUSEWHEEL = 0x020A
WM_XBUTTONDOWN = 0x020B
WM_XBUTTONUP = 0x020C
WM_MOUSEHWHEEL = 0x020E
PM_REMOVE = 0x0001

SWALLOW_MSG = frozenset(
    {
        WM_LBUTTONDOWN,
        WM_LBUTTONUP,
        WM_RBUTTONDOWN,
        WM_RBUTTONUP,
        WM_MBUTTONDOWN,
        WM_MBUTTONUP,
        WM_XBUTTONDOWN,
        WM_XBUTTONUP,
        WM_MOUSEWHEEL,
        WM_MOUSEHWHEEL,
    }
)

WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
LowLevelMouseProc = ctypes.WINFUNCTYPE(
    ctypes.c_ssize_t, ctypes.c_int, wt.WPARAM, wt.LPARAM
)


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class RECT(ctypes.Structure):
    _fields_ = [
        ("left", ctypes.c_long),
        ("top", ctypes.c_long),
        ("right", ctypes.c_long),
        ("bottom", ctypes.c_long),
    ]


class MSLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [
        ("pt", POINT),
        ("mouseData", wt.DWORD),
        ("flags", wt.DWORD),
        ("time", wt.DWORD),
        ("dwExtraInfo", ctypes.c_ulonglong),
    ]


class MSG(ctypes.Structure):
    _fields_ = [
        ("hwnd", wt.HWND),
        ("message", wt.UINT),
        ("wParam", wt.WPARAM),
        ("lParam", wt.LPARAM),
        ("time", wt.DWORD),
        ("pt", POINT),
    ]


def hwnd_i(h: object | None) -> int:
    return int(h) if h else 0


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
    user32.WindowFromPoint.argtypes = (POINT,)
    user32.WindowFromPoint.restype = wt.HWND
    user32.GetAncestor.argtypes = (wt.HWND, ctypes.c_uint)
    user32.GetAncestor.restype = wt.HWND
    user32.ClipCursor.argtypes = (ctypes.POINTER(RECT),)
    user32.ClipCursor.restype = wt.BOOL
    user32.SetWindowsHookExW.argtypes = (
        ctypes.c_int,
        LowLevelMouseProc,
        wt.HINSTANCE,
        wt.DWORD,
    )
    user32.SetWindowsHookExW.restype = wt.HHOOK
    user32.CallNextHookEx.argtypes = (
        wt.HHOOK,
        ctypes.c_int,
        wt.WPARAM,
        wt.LPARAM,
    )
    user32.CallNextHookEx.restype = ctypes.c_ssize_t
    user32.UnhookWindowsHookEx.argtypes = (wt.HHOOK,)
    user32.UnhookWindowsHookEx.restype = wt.BOOL
    user32.PeekMessageW.argtypes = (
        ctypes.POINTER(MSG),
        wt.HWND,
        wt.UINT,
        wt.UINT,
        wt.UINT,
    )
    user32.PeekMessageW.restype = wt.BOOL
    user32.TranslateMessage.argtypes = (ctypes.POINTER(MSG),)
    user32.TranslateMessage.restype = wt.BOOL
    user32.DispatchMessageW.argtypes = (ctypes.POINTER(MSG),)
    user32.DispatchMessageW.restype = ctypes.c_ssize_t
    user32.PostThreadMessageW.argtypes = (wt.DWORD, wt.UINT, wt.WPARAM, wt.LPARAM)
    user32.PostThreadMessageW.restype = wt.BOOL
    try:
        user32.SetProcessDPIAware.argtypes = ()
        user32.SetProcessDPIAware.restype = wt.BOOL
        user32.SetProcessDPIAware()
    except AttributeError:
        pass
    return user32


USER32 = _bind_user32()
GA_ROOT = 2
WM_QUIT = 0x0012


class AntiGrab:
    """WH_MOUSE_LL on its own pumped thread (required or the cursor crawls)."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._paused = False
        self._qemu_hwnd = 0
        self._client = (0, 0, 0, 0)  # screen x,y,w,h of full client
        self._hook = None
        self._proc = None
        self._thread: threading.Thread | None = None
        self._tid = 0
        self._ready = threading.Event()
        self._stop = threading.Event()
        self._error: str | None = None

    def set_paused(self, paused: bool) -> None:
        with self._lock:
            self._paused = paused

    def set_target(self, qemu_hwnd: int, client: tuple[int, int, int, int] | None) -> None:
        with self._lock:
            self._qemu_hwnd = qemu_hwnd
            self._client = client if client is not None else (0, 0, 0, 0)

    def _hit(self, x: int, y: int) -> bool:
        with self._lock:
            if self._paused:
                return False
            hwnd = self._qemu_hwnd
            cx, cy, cw, ch = self._client
        if hwnd and cw > 0 and ch > 0:
            if cx <= x < cx + cw and cy <= y < cy + ch:
                return True
        # Fallback: any HWND under the cursor belonging to the QEMU top-level.
        if hwnd:
            pt = POINT(x, y)
            under = hwnd_i(USER32.WindowFromPoint(pt))
            if under:
                root = hwnd_i(USER32.GetAncestor(under, GA_ROOT))
                if root == hwnd or under == hwnd:
                    return True
        return False

    def _hook_proc(self, n_code: int, w_param: int, l_param: int) -> int:
        if n_code >= 0:
            msg = int(w_param)
            if msg != WM_MOUSEMOVE and msg in SWALLOW_MSG:
                info = ctypes.cast(
                    l_param, ctypes.POINTER(MSLLHOOKSTRUCT)
                ).contents
                if self._hit(int(info.pt.x), int(info.pt.y)):
                    return 1
        return int(USER32.CallNextHookEx(self._hook, n_code, w_param, l_param))

    def _thread_main(self) -> None:
        # Hook MUST be installed on this thread, which then pumps messages.
        self._tid = threading.get_native_id()
        self._proc = LowLevelMouseProc(self._hook_proc)
        self._hook = USER32.SetWindowsHookExW(WH_MOUSE_LL, self._proc, None, 0)
        if not self._hook:
            self._error = f"SetWindowsHookExW failed: {ctypes.get_last_error()}"
            self._ready.set()
            return
        self._ready.set()
        msg = MSG()
        while not self._stop.is_set():
            while USER32.PeekMessageW(ctypes.byref(msg), None, 0, 0, PM_REMOVE):
                if msg.message == WM_QUIT:
                    self._stop.set()
                    break
                USER32.TranslateMessage(ctypes.byref(msg))
                USER32.DispatchMessageW(ctypes.byref(msg))
            time.sleep(0.001)
        if self._hook:
            USER32.UnhookWindowsHookEx(self._hook)
            self._hook = None
        self._proc = None

    def start(self) -> None:
        if self._thread is not None:
            return
        self._stop.clear()
        self._ready.clear()
        self._thread = threading.Thread(
            target=self._thread_main, name="mousevirt-hook", daemon=True
        )
        self._thread.start()
        if not self._ready.wait(timeout=3.0):
            raise OSError("anti-grab hook thread did not start")
        if self._error:
            raise OSError(self._error)

    def stop(self) -> None:
        self._stop.set()
        tid = self._tid
        if tid:
            USER32.PostThreadMessageW(tid, WM_QUIT, 0, 0)
        t = self._thread
        if t is not None:
            t.join(timeout=2.0)
        self._thread = None
        self._tid = 0


def pack_pkt(typ: int, buttons: int, x: int, y: int, wheel: int = 0) -> bytes:
    return struct.pack(
        "<BBhhbB", typ & 0xFF, buttons & 0xFF, int(x), int(y), int(wheel), 0
    )


def connect(host: str, port: int, timeout: float) -> socket.socket:
    s = socket.create_connection((host, port), timeout=timeout)
    s.settimeout(None)
    s.setblocking(False)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def find_qemu_hwnd(user32: ctypes.WinDLL) -> int:
    found = wt.HWND(0)
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
    return hwnd_i(found.value)


def client_screen_rect(
    user32: ctypes.WinDLL, hwnd: int
) -> tuple[int, int, int, int] | None:
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
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=400)
    ap.add_argument("--reconnect", action="store_true")
    ap.add_argument("--hz", type=float, default=60.0)
    ap.add_argument(
        "--fullscreen-map",
        action="store_true",
        help="map primary monitor; disables anti-grab filter",
    )
    ap.add_argument(
        "--no-grab-filter",
        action="store_true",
        help="do not install WH_MOUSE_LL",
    )
    args = ap.parse_args()

    user32 = USER32
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
    anti: AntiGrab | None = None

    print(
        f"mousevirt → {args.host}:{args.port}  guest {args.width}x{args.height}  "
        f"(RCtrl=pause, Esc/Ctrl+C=quit)",
        flush=True,
    )

    def console_focused() -> bool:
        cons = hwnd_i(hwnd_console)
        if cons == 0:
            return True
        return hwnd_i(user32.GetForegroundWindow()) == cons

    try:
        if not args.no_grab_filter and not args.fullscreen_map:
            anti = AntiGrab()
            anti.start()
            atexit.register(anti.stop)
            print(
                "anti-grab: pumped WH_MOUSE_LL (buttons/wheel over QEMU client)",
                flush=True,
            )

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
            if anti is not None:
                anti.set_paused(paused)

            if not paused:
                # If SDL already grabbed, drop any ClipCursor confinement.
                user32.ClipCursor(None)

                origin_x, origin_y = 0, 0
                content_x, content_y = 0, 0
                content_w, content_h = sw, sh
                require_inside = False
                client: tuple[int, int, int, int] | None = None

                if not args.fullscreen_map:
                    now = time.monotonic()
                    if qemu_hwnd == 0 or (now - last_find) >= 2.0:
                        qemu_hwnd = find_qemu_hwnd(user32)
                        last_find = now
                        if qemu_hwnd == 0 and not warned_no_qemu:
                            print(
                                "QEMU window not found — mapping primary monitor",
                                flush=True,
                            )
                            warned_no_qemu = True
                            map_mode = "monitor"
                        elif qemu_hwnd != 0 and map_mode != "qemu":
                            print("mapping QEMU client (letterboxed)", flush=True)
                            map_mode = "qemu"
                            warned_no_qemu = False

                    cr = (
                        client_screen_rect(user32, qemu_hwnd)
                        if qemu_hwnd
                        else None
                    )
                    if cr is not None:
                        client = cr
                        origin_x, origin_y, cw, ch = cr
                        content_x, content_y, content_w, content_h = letterbox(
                            cw, ch, args.width, args.height
                        )
                        require_inside = True

                if anti is not None:
                    anti.set_target(qemu_hwnd, client)

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
                        time.sleep(0.75)
                        continue

            time.sleep(period)

    except KeyboardInterrupt:
        print("\nquit", flush=True)
        return 0
    finally:
        if anti is not None:
            anti.stop()
        user32.ClipCursor(None)
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
