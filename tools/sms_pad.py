"""Shared SMS pad binding helpers for hostfsd (Win32 level keyboard state)."""
from __future__ import annotations

import ctypes
import sys
import threading
import time
from ctypes import wintypes
from pathlib import Path

# HID usage (argon/keys.h) → Win32 VK
HID_TO_VK = {
    0x04: 0x41, 0x05: 0x42, 0x06: 0x43, 0x07: 0x44, 0x08: 0x45, 0x09: 0x46,
    0x0A: 0x47, 0x0B: 0x48, 0x0C: 0x49, 0x0D: 0x4A, 0x0E: 0x4B, 0x0F: 0x4C,
    0x10: 0x4D, 0x11: 0x4E, 0x12: 0x4F, 0x13: 0x50, 0x14: 0x51, 0x15: 0x52,
    0x16: 0x53, 0x17: 0x54, 0x18: 0x55, 0x19: 0x56, 0x1A: 0x57, 0x1B: 0x58,
    0x1C: 0x59, 0x1D: 0x5A,
    0x1E: 0x31, 0x1F: 0x32, 0x20: 0x33, 0x21: 0x34, 0x22: 0x35,
    0x23: 0x36, 0x24: 0x37, 0x25: 0x38, 0x26: 0x39, 0x27: 0x30,
    0x28: 0x0D,  # ENTER
    0x29: 0x1B,  # ESC
    0x2C: 0x20,  # SPACE
    0x2B: 0x09,  # TAB
    0x4F: 0x27,  # RIGHT
    0x50: 0x25,  # LEFT
    0x51: 0x28,  # DOWN
    0x52: 0x26,  # UP
    0x3A: 0x70, 0x3B: 0x71, 0x3C: 0x72, 0x3D: 0x73, 0x3E: 0x74, 0x3F: 0x75,
    0x40: 0x76, 0x41: 0x77, 0x42: 0x78, 0x43: 0x79, 0x44: 0x7A, 0x45: 0x7B,
    0x59: 0x61, 0x5A: 0x62, 0x5B: 0x63, 0x5C: 0x64, 0x5D: 0x65,
    0x5E: 0x66, 0x5F: 0x67, 0x60: 0x68, 0x61: 0x69, 0x62: 0x60,  # KP1..KP0
    0x58: 0x0D,  # KPENTER (same VK as Enter)
    0x57: 0x6B, 0x56: 0x6D, 0x55: 0x6A, 0x54: 0x6F,
}

NAME_TO_HID = {
    "UP": 0x52, "DOWN": 0x51, "LEFT": 0x50, "RIGHT": 0x4F,
    "ENTER": 0x28, "ESC": 0x29, "ESCAPE": 0x29, "SPACE": 0x2C, "TAB": 0x2B,
    "A": 0x04, "B": 0x05, "C": 0x06, "D": 0x07, "E": 0x08, "F": 0x09,
    "G": 0x0A, "H": 0x0B, "I": 0x0C, "J": 0x0D, "K": 0x0E, "L": 0x0F,
    "M": 0x10, "N": 0x11, "O": 0x12, "P": 0x13, "Q": 0x14, "R": 0x15,
    "S": 0x16, "T": 0x17, "U": 0x18, "V": 0x19, "W": 0x1A, "X": 0x1B,
    "Y": 0x1C, "Z": 0x1D,
    "1": 0x1E, "2": 0x1F, "3": 0x20, "4": 0x21, "5": 0x22,
    "6": 0x23, "7": 0x24, "8": 0x25, "9": 0x26, "0": 0x27,
    "F1": 0x3A, "F2": 0x3B, "F3": 0x3C, "F4": 0x3D, "F5": 0x3E, "F6": 0x3F,
    "F7": 0x40, "F8": 0x41, "F9": 0x42, "F10": 0x43, "F11": 0x44, "F12": 0x45,
    "KP0": 0x62, "KP1": 0x59, "KP2": 0x5A, "KP3": 0x5B, "KP4": 0x5C,
    "KP5": 0x5D, "KP6": 0x5E, "KP7": 0x5F, "KP8": 0x60, "KP9": 0x61,
    "KPENTER": 0x58, "KPPLUS": 0x57, "KPMINUS": 0x56, "KPMUL": 0x55,
    "KPDIV": 0x54,
}

# Low byte of each pad (bytes 0 / 1 of the snapshot).
ACT_BIT = {
    "UP": 0x01, "DOWN": 0x02, "LEFT": 0x04, "RIGHT": 0x08,
    "B1": 0x10, "BUTTON1": 0x10, "B2": 0x20, "BUTTON2": 0x20,
    # Aliases used by Mega Drive configs.
    "A": 0x10, "B": 0x20,
}

# High byte of each pad (bytes 3 / 4).  START here is the dedicated button;
# padN.pause still sets the sys pause bit for SMS / old guests.
ACT_BIT_HI = {
    "C": 0x01,
    "START": 0x02,
    "X": 0x04,
    "Y": 0x08,
    "Z": 0x10,
    "MODE": 0x20,
}

ACT_SYS = {
    "PAUSE": 0x01,  # also treated as Start by Mega Drive for 3-byte hosts
    "QUIT": 0x02, "EXIT": 0x02,
}

PAD_VER = 1

DEFAULTS = """\
pad0.up=UP
pad0.down=DOWN
pad0.left=LEFT
pad0.right=RIGHT
pad0.b1=Z
pad0.b2=X
pad0.c=C
pad0.start=ENTER
pad0.pause=ENTER
pad0.quit=ESC
pad1.up=W
pad1.down=S
pad1.left=A
pad1.right=D
pad1.b1=J
pad1.b2=K
pad1.c=L
pad1.start=P
pad1.pause=P
pad1.quit=Q
"""

WH_KEYBOARD_LL = 13
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_SYSKEYDOWN = 0x0104
WM_SYSKEYUP = 0x0105
LLKHF_UP = 0x80


class KBDLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [
        ("vkCode", wintypes.DWORD),
        ("scanCode", wintypes.DWORD),
        ("flags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.c_size_t),
    ]


LowLevelKeyboardProc = ctypes.WINFUNCTYPE(
    ctypes.c_long, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM
)


def load_binds(
    path: Path | None,
) -> tuple[
    list[tuple[int, int, int]],
    list[tuple[int, int, int]],
    list[tuple[int, int]],
]:
    """Return (lo_binds, hi_binds, sys_binds).

    lo/hi entries are (pad, vk, bit); sys entries are (vk, bit).  An old
    sms.cfg without c/start/x/y/z/mode still loads; those bits stay zero.
    """
    text = DEFAULTS
    if path is not None and path.is_file():
        text = path.read_text(encoding="utf-8", errors="replace")
    pad_binds: list[tuple[int, int, int]] = []
    hi_binds: list[tuple[int, int, int]] = []
    sys_binds: list[tuple[int, int]] = []
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or "=" not in line:
            continue
        lhs, rhs = line.split("=", 1)
        lhs, rhs = lhs.strip().lower(), rhs.strip().upper()
        if not lhs.startswith("pad") or len(lhs) < 6 or lhs[3] not in "01" or lhs[4] != ".":
            continue
        pad = int(lhs[3])
        act = lhs[5:].upper()
        hid = NAME_TO_HID.get(rhs)
        if hid is None:
            continue
        vk = HID_TO_VK.get(hid)
        if vk is None:
            continue
        if act in ACT_BIT:
            pad_binds.append((pad, vk, ACT_BIT[act]))
        elif act in ACT_BIT_HI:
            hi_binds.append((pad, vk, ACT_BIT_HI[act]))
        elif act in ACT_SYS:
            sys_binds.append((vk, ACT_SYS[act]))
    return pad_binds, hi_binds, sys_binds


class PadState:
    """Live 6-byte pad snapshot: pad0, pad1, sys, pad0hi, pad1hi, ver.

    QEMU/SDL on Windows installs a low-level keyboard hook when the display
    grabs input; GetAsyncKeyState alone often stays zero.  We keep our own
    WH_KEYBOARD_LL state and OR it with GetAsyncKeyState.
    """

    def __init__(self, cfg_path: Path | None) -> None:
        self.pad_binds, self.hi_binds, self.sys_binds = load_binds(cfg_path)
        self.lock = threading.Lock()
        self.bytes = bytes([0, 0, 0, 0, 0, PAD_VER])
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._down: set[int] = set()
        self._hook = None
        self._proc = None
        self._user32 = None
        if sys.platform != "win32":
            print("sms_pad: Win32 keyboard only; pad stays zero", file=sys.stderr)
            return
        self._user32 = ctypes.windll.user32
        self._user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
        self._user32.GetAsyncKeyState.restype = ctypes.c_short
        self._user32.SetWindowsHookExW.argtypes = [
            ctypes.c_int, LowLevelKeyboardProc, wintypes.HINSTANCE, wintypes.DWORD
        ]
        self._user32.SetWindowsHookExW.restype = wintypes.HHOOK
        self._user32.CallNextHookEx.argtypes = [
            wintypes.HHOOK, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM
        ]
        self._user32.CallNextHookEx.restype = ctypes.c_long
        self._user32.UnhookWindowsHookEx.argtypes = [wintypes.HHOOK]
        self._user32.UnhookWindowsHookEx.restype = wintypes.BOOL
        self._thread = threading.Thread(target=self._loop, name="sms-pad",
                                        daemon=True)
        self._thread.start()

    def _hook_cb(self, nCode: int, wParam: int, lParam: int) -> int:
        if nCode >= 0:
            info = ctypes.cast(lParam, ctypes.POINTER(KBDLLHOOKSTRUCT)).contents
            vk = int(info.vkCode)
            up = (int(info.flags) & LLKHF_UP) != 0 or wParam in (WM_KEYUP, WM_SYSKEYUP)
            with self.lock:
                if up:
                    self._down.discard(vk)
                else:
                    self._down.add(vk)
        return int(self._user32.CallNextHookEx(self._hook, nCode, wParam, lParam))

    def _async_down(self, vk: int) -> bool:
        return bool(self._user32.GetAsyncKeyState(vk) & 0x8000)

    def _key_down(self, vk: int) -> bool:
        with self.lock:
            hooked = vk in self._down
        return hooked or self._async_down(vk)

    def _loop(self) -> None:
        self._proc = LowLevelKeyboardProc(self._hook_cb)
        self._hook = self._user32.SetWindowsHookExW(WH_KEYBOARD_LL, self._proc, None, 0)
        if not self._hook:
            print("sms_pad: SetWindowsHookEx failed; using GetAsyncKeyState only",
                  file=sys.stderr, flush=True)

        msg = wintypes.MSG()
        while not self._stop.is_set():
            # LL hooks need a message pump on the installing thread.
            while self._user32.PeekMessageW(ctypes.byref(msg), 0, 0, 0, 1):
                self._user32.TranslateMessage(ctypes.byref(msg))
                self._user32.DispatchMessageW(ctypes.byref(msg))

            p0 = p1 = sysb = p0h = p1h = 0
            for pad, vk, bit in self.pad_binds:
                if self._key_down(vk):
                    if pad == 0:
                        p0 |= bit
                    else:
                        p1 |= bit
            for pad, vk, bit in self.hi_binds:
                if self._key_down(vk):
                    if pad == 0:
                        p0h |= bit
                    else:
                        p1h |= bit
            for vk, bit in self.sys_binds:
                if self._key_down(vk):
                    sysb |= bit
            blob = bytes(
                (
                    p0 & 0xFF,
                    p1 & 0xFF,
                    sysb & 0xFF,
                    p0h & 0xFF,
                    p1h & 0xFF,
                    PAD_VER,
                )
            )
            with self.lock:
                self.bytes = blob
            time.sleep(0.004)

        if self._hook:
            self._user32.UnhookWindowsHookEx(self._hook)
            self._hook = None

    def snapshot(self) -> bytes:
        with self.lock:
            return self.bytes

    def stop(self) -> None:
        self._stop.set()
