#!/usr/bin/env python3
"""Send host keyboard note on/off to ArgonOS /dev/midivirt (MIDIVIRT.SYS).

Guest listens on TCP :5559 (QEMU hostfwd).  Same piano map as apps/dx7:

  Z..M  = C4..B4   Q..I (+ black keys) = C5..C6

Uses Win32 GetAsyncKeyState for real press/release (poly).  Typical:

  # guest (once):  drv install h:\\midivirt.sys
  # guest:         run h:\\dx7.axe pcmvirt
  python tools/midikbd.py
  python tools/midikbd.py --reconnect
  python tools/midikbd.py --reconnect --probe   # input RTT (key → DX7)

Esc quits.  Space sends All Notes Off (CC 123).

With --probe, DX7 ACKs each note-on as SysEx F0 7D <note> F7; prints RTT and
RTT/2 (≈ one-way host→DX7). Esc prints min/median/p99 summary.
"""

from __future__ import annotations

import argparse
import ctypes
import select
import socket
import statistics
import sys
import time

# Virtual-key codes (US layout) → MIDI note
# https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes
NOTE_KEYS: dict[int, int] = {
    # Z S X D C V G B H N J M  → 60..71
    0x5A: 60,  # Z
    0x53: 61,  # S
    0x58: 62,  # X
    0x44: 63,  # D
    0x43: 64,  # C
    0x56: 65,  # V
    0x47: 66,  # G
    0x42: 67,  # B
    0x48: 68,  # H
    0x4E: 69,  # N
    0x4A: 70,  # J
    0x4D: 71,  # M
    # Q 2 W 3 E R 5 T 6 Y 7 U I → 72..84
    0x51: 72,  # Q
    0x32: 73,  # 2
    0x57: 74,  # W
    0x33: 75,  # 3
    0x45: 76,  # E
    0x52: 77,  # R
    0x35: 78,  # 5
    0x54: 79,  # T
    0x36: 80,  # 6
    0x59: 81,  # Y
    0x37: 82,  # 7
    0x55: 83,  # U
    0x49: 84,  # I
}

VK_ESCAPE = 0x1B
VK_SPACE = 0x20
NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")

# Guest ACK after note-on is inside DX7 (apps/dx7/dx7.c midivirt_ack_note).
ACK_F0 = 0xF0
ACK_ID = 0x7D
ACK_F7 = 0xF7


def note_name(n: int) -> str:
    return f"{NOTE_NAMES[n % 12]}{n // 12 - 1}"


def midi_on(note: int, vel: int = 100) -> bytes:
    return bytes((0x90, note & 0x7F, vel & 0x7F))


def midi_off(note: int) -> bytes:
    return bytes((0x80, note & 0x7F, 0x00))


def midi_all_off() -> bytes:
    return bytes((0xB0, 0x7B, 0x00))


def percentile(sorted_vals: list[float], p: float) -> float:
    if not sorted_vals:
        return 0.0
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    k = (len(sorted_vals) - 1) * (p / 100.0)
    f = int(k)
    c = min(f + 1, len(sorted_vals) - 1)
    if f == c:
        return sorted_vals[f]
    return sorted_vals[f] + (sorted_vals[c] - sorted_vals[f]) * (k - f)


class AckParser:
    """Parse guest SysEx ACKs: F0 7D <note> F7."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, data: bytes) -> list[int]:
        notes: list[int] = []
        self._buf.extend(data)
        i = 0
        while i + 3 < len(self._buf):
            if self._buf[i] != ACK_F0:
                i += 1
                continue
            if self._buf[i + 1] != ACK_ID:
                i += 1
                continue
            note = self._buf[i + 2]
            end = self._buf[i + 3]
            if end != ACK_F7 or note > 127:
                i += 1
                continue
            notes.append(note)
            i += 4
        if i:
            del self._buf[:i]
        # Cap runaway garbage
        if len(self._buf) > 64:
            del self._buf[:-4]
        return notes


class KeyPoller:
    def __init__(self) -> None:
        self._user32 = ctypes.windll.user32
        self._kernel32 = ctypes.windll.kernel32
        self._prev: dict[int, bool] = {vk: False for vk in NOTE_KEYS}
        self._prev[VK_ESCAPE] = False
        self._prev[VK_SPACE] = False
        # Console HWND: GetAsyncKeyState is global — Esc in QEMU must NOT quit us.
        self._hwnd = self._kernel32.GetConsoleWindow()

    def down(self, vk: int) -> bool:
        # high bit set → key currently down
        return bool(self._user32.GetAsyncKeyState(vk) & 0x8000)

    def console_focused(self) -> bool:
        if not self._hwnd:
            return True
        return self._user32.GetForegroundWindow() == self._hwnd

    def edges(self) -> list[tuple[str, int | None]]:
        """Return list of ('on'|'off'|'panic'|'quit', note_or_None)."""
        out: list[tuple[str, int | None]] = []
        focused = self.console_focused()

        for vk, note in NOTE_KEYS.items():
            now = self.down(vk) if focused else False
            was = self._prev[vk]
            if focused:
                if now and not was:
                    out.append(("on", note))
                elif was and not now:
                    out.append(("off", note))
            elif was:
                # Lost focus while held → note-off so guest is not stuck.
                out.append(("off", note))
                now = False
            self._prev[vk] = now

        esc = self.down(VK_ESCAPE) if focused else False
        if esc and not self._prev[VK_ESCAPE]:
            out.append(("quit", None))
        self._prev[VK_ESCAPE] = esc

        sp = self.down(VK_SPACE) if focused else False
        if sp and not self._prev[VK_SPACE]:
            out.append(("panic", None))
        self._prev[VK_SPACE] = sp
        return out


def connect(host: str, port: int, timeout: float) -> socket.socket:
    s = socket.create_connection((host, port), timeout=timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def drain_acks(
    sock: socket.socket,
    parser: AckParser,
    pending: dict[int, float],
    rtts_ms: list[float],
) -> None:
    while True:
        r, _, _ = select.select([sock], [], [], 0)
        if not r:
            break
        try:
            chunk = sock.recv(256)
        except OSError:
            break
        if not chunk:
            break
        for note in parser.feed(chunk):
            t0 = pending.pop(note, None)
            if t0 is None:
                continue
            rtt_ms = (time.perf_counter() - t0) * 1000.0
            rtts_ms.append(rtt_ms)
            print(
                f"RTT {note_name(note):4} ({note})  "
                f"{rtt_ms:6.1f} ms  one-way≈{rtt_ms / 2.0:5.1f} ms"
            )


def print_probe_summary(rtts_ms: list[float]) -> None:
    if not rtts_ms:
        print("probe: no ACKs (is DX7 running with midivirt? reinstall MIDIVIRT.SYS?)")
        return
    s = sorted(rtts_ms)
    med = statistics.median(s)
    print(
        f"probe summary: n={len(s)}  "
        f"RTT min={s[0]:.1f} med={med:.1f} p95={percentile(s, 95):.1f} "
        f"p99={percentile(s, 99):.1f} max={s[-1]:.1f} ms"
    )
    print(
        f"probe summary: one-way≈RTT/2  "
        f"min={s[0] / 2:.1f} med={med / 2:.1f} "
        f"p95={percentile(s, 95) / 2:.1f} p99={percentile(s, 99) / 2:.1f} "
        f"max={s[-1] / 2:.1f} ms"
    )


def run(host: str, port: int, reconnect: bool, vel: int, probe: bool) -> int:
    if sys.platform != "win32":
        print("midikbd.py currently requires Windows (GetAsyncKeyState)", file=sys.stderr)
        return 2

    poller = KeyPoller()
    sock: socket.socket | None = None
    parser = AckParser()
    pending: dict[int, float] = {}
    rtts_ms: list[float] = []

    print(f"midikbd: piano Z-M / Q-I → {host}:{port}  (Esc quit, Space panic)")
    print("midikbd: keys only while THIS console is focused")
    print("midikbd: Esc in QEMU/DX7 no longer closes this socket")
    if probe:
        print("midikbd: --probe on (measure key → DX7 via ACK SysEx F0 7D note F7)")

    try:
        while True:
            if sock is None:
                try:
                    sock = connect(host, port, 2.0)
                    print("midikbd: connected")
                    parser = AckParser()
                    pending.clear()
                except OSError as e:
                    if not reconnect:
                        print(f"midikbd: connect failed: {e}", file=sys.stderr)
                        return 1
                    time.sleep(0.5)
                    continue

            if probe and sock is not None:
                try:
                    drain_acks(sock, parser, pending, rtts_ms)
                except OSError as e:
                    print(f"midikbd: recv failed: {e}")
                    try:
                        sock.close()
                    except OSError:
                        pass
                    sock = None
                    if not reconnect:
                        return 1
                    continue

            for kind, note in poller.edges():
                try:
                    if kind == "quit":
                        if sock:
                            sock.sendall(midi_all_off())
                        print("midikbd: quit")
                        if probe:
                            print_probe_summary(rtts_ms)
                        return 0
                    if kind == "panic":
                        sock.sendall(midi_all_off())
                        print("midikbd: ALL NOTES OFF")
                        continue
                    assert note is not None
                    if kind == "on":
                        if probe:
                            pending[note] = time.perf_counter()
                        sock.sendall(midi_on(note, vel))
                        if not probe:
                            print(f"ON  {note_name(note):4} ({note})")
                    else:
                        sock.sendall(midi_off(note))
                        if not probe:
                            print(f"OFF {note_name(note):4} ({note})")
                except OSError as e:
                    print(f"midikbd: send failed: {e}")
                    try:
                        sock.close()
                    except OSError:
                        pass
                    sock = None
                    if not reconnect:
                        return 1
                    break

            time.sleep(0.008)
    finally:
        if sock is not None:
            try:
                sock.sendall(midi_all_off())
                sock.close()
            except OSError:
                pass
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5559)
    ap.add_argument("--reconnect", action="store_true")
    ap.add_argument("--vel", type=int, default=100)
    ap.add_argument(
        "--probe",
        action="store_true",
        help="measure input RTT (key edge → DX7 ACK); print summary on Esc",
    )
    args = ap.parse_args()
    return run(
        args.host,
        args.port,
        args.reconnect,
        max(1, min(127, args.vel)),
        args.probe,
    )


if __name__ == "__main__":
    raise SystemExit(main())
