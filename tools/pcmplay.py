#!/usr/bin/env python3
"""Play a streaming WAV PCM feed from ArgonOS (/dev/pcmvirt via PCMVIRT.SYS).

The guest publishes PCM on TCP :5558 (QEMU hostfwd).  Connect anytime — the
guest does not wait for you.  Typical:

  # guest (once):  drv install t:\\pcmvirt.sys
  # guest:         run … with audio_out=/dev/pcmvirt
  python tools/pcmplay.py
  python tools/pcmplay.py --reconnect          # survive guest restart
  python tools/pcmplay.py --record build/out.wav

A reader thread keeps the TCP window drained so the guest never blocks on send.
Playback uses a short jitter buffer; underruns insert silence (QEMU is often
far below realtime, so gaps are expected unless the emu catches up).
"""

from __future__ import annotations

import argparse
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import wave
from pathlib import Path


def read_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("connection closed")
        buf.extend(chunk)
    return bytes(buf)


def parse_wav_header(hdr: bytes) -> tuple[int, int, int]:
    if len(hdr) < 44 or hdr[0:4] != b"RIFF" or hdr[8:12] != b"WAVE":
        raise ValueError("not a WAV stream")
    channels = struct.unpack_from("<H", hdr, 22)[0]
    rate = struct.unpack_from("<I", hdr, 24)[0]
    bits = struct.unpack_from("<H", hdr, 34)[0]
    if channels < 1 or rate < 1 or bits != 16:
        raise ValueError(f"unsupported WAV: ch={channels} rate={rate} bits={bits}")
    return rate, channels, bits


class PcmRing:
    """Thread-safe byte ring for PCM (int16 interleaved)."""

    def __init__(self, capacity: int) -> None:
        self._buf = bytearray()
        self._cap = capacity
        self._cv = threading.Condition()
        self._closed = False
        self.dropped = 0

    def close(self) -> None:
        with self._cv:
            self._closed = True
            self._cv.notify_all()

    def write(self, data: bytes) -> None:
        if not data:
            return
        with self._cv:
            if self._closed:
                return
            self._buf.extend(data)
            overflow = len(self._buf) - self._cap
            if overflow > 0:
                del self._buf[:overflow]
                self.dropped += overflow
            self._cv.notify_all()

    def read(self, n: int, timeout: float | None = None) -> bytes:
        deadline = None if timeout is None else time.monotonic() + timeout
        with self._cv:
            while len(self._buf) < n and not self._closed:
                if deadline is None:
                    self._cv.wait()
                else:
                    left = deadline - time.monotonic()
                    if left <= 0:
                        break
                    self._cv.wait(timeout=left)
            take = min(n, len(self._buf))
            out = bytes(self._buf[:take])
            del self._buf[:take]
            return out

    def available(self) -> int:
        with self._cv:
            return len(self._buf)


class WavRecorder:
    """Append raw s16 PCM; finalize WAV sizes on close."""

    def __init__(self, path: Path, rate: int, channels: int) -> None:
        self.path = path
        self._wf = wave.open(str(path), "wb")
        self._wf.setnchannels(channels)
        self._wf.setsampwidth(2)
        self._wf.setframerate(rate)
        self._lock = threading.Lock()
        self._bytes = 0

    def write(self, data: bytes) -> None:
        if not data:
            return
        if len(data) & 1:
            data = data[:-1]
        with self._lock:
            self._wf.writeframes(data)
            self._bytes += len(data)

    def close(self) -> None:
        with self._lock:
            self._wf.close()
        print(f"wrote {self.path} ({self._bytes} bytes PCM)", flush=True)


def reader_thread(
    sock: socket.socket, ring: PcmRing, rec: WavRecorder | None = None
) -> None:
    try:
        while True:
            chunk = sock.recv(8192)
            if not chunk:
                break
            if rec is not None:
                rec.write(chunk)
            ring.write(chunk)
    except OSError:
        pass
    finally:
        ring.close()


def play_sounddevice(
    sock: socket.socket, rate: int, channels: int, rec: WavRecorder | None = None
) -> None:
    import sounddevice as sd

    frame_bytes = channels * 2
    # ~1.0 s capacity / ~200 ms prebuffer — fewer live underruns under QEMU.
    ring = PcmRing(capacity=rate * frame_bytes)
    prebuffer = rate * frame_bytes * 20 // 100
    chunk = max(frame_bytes * (rate // 40), frame_bytes * 128)
    underruns = 0
    played = 0
    last_report = time.monotonic()

    t = threading.Thread(
        target=reader_thread, args=(sock, ring, rec), daemon=True
    )
    t.start()

    print(f"buffering ~{prebuffer // frame_bytes} samples...", flush=True)
    while ring.available() < prebuffer and t.is_alive():
        time.sleep(0.01)

    print(
        f"playing {rate} Hz, {channels} ch (sounddevice). Ctrl+C to stop.",
        flush=True,
    )
    silence = bytes(chunk)
    try:
        with sd.RawOutputStream(
            samplerate=rate,
            channels=channels,
            dtype="int16",
            blocksize=0,
        ) as stream:
            while True:
                data = ring.read(chunk, timeout=0.05)
                if not data:
                    if not t.is_alive() and ring.available() == 0:
                        break
                    stream.write(silence)
                    underruns += 1
                    continue
                rem = len(data) % frame_bytes
                if rem:
                    data = data[:-rem]
                if data:
                    stream.write(data)
                    played += len(data)

                now = time.monotonic()
                if now - last_report >= 2.0:
                    print(
                        f"stats: played={played} B  ring_drop={ring.dropped} B  "
                        f"underruns={underruns}  buffered={ring.available()} B",
                        flush=True,
                    )
                    last_report = now
    finally:
        print(
            f"final: played={played} B  ring_drop={ring.dropped} B  "
            f"underruns={underruns}",
            flush=True,
        )


def play_ffplay(
    sock: socket.socket, rate: int, channels: int, rec: WavRecorder | None = None
) -> None:
    ffplay = shutil.which("ffplay")
    if not ffplay:
        raise SystemExit("ffplay not found on PATH")
    hdr = bytearray(44)
    hdr[0:4] = b"RIFF"
    struct.pack_into("<I", hdr, 4, 0x7FFFFFFF)
    hdr[8:12] = b"WAVE"
    hdr[12:16] = b"fmt "
    struct.pack_into("<I", hdr, 16, 16)
    struct.pack_into("<H", hdr, 20, 1)
    struct.pack_into("<H", hdr, 22, channels)
    struct.pack_into("<I", hdr, 24, rate)
    struct.pack_into("<I", hdr, 28, rate * channels * 2)
    struct.pack_into("<H", hdr, 32, channels * 2)
    struct.pack_into("<H", hdr, 34, 16)
    hdr[36:40] = b"data"
    struct.pack_into("<I", hdr, 40, 0x7FFFFFFF)

    cmd = [ffplay, "-nodisp", "-autoexit", "-loglevel", "warning", "-i", "pipe:0"]
    print(f"playing via ffplay ({rate} Hz, {channels} ch). Ctrl+C to stop.")
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    assert proc.stdin is not None
    try:
        proc.stdin.write(hdr)
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            if rec is not None:
                rec.write(chunk)
            proc.stdin.write(chunk)
    finally:
        try:
            proc.stdin.close()
        except OSError:
            pass
        proc.wait(timeout=5)


def save_wav(sock: socket.socket, rate: int, channels: int, path: Path) -> None:
    print(f"recording to {path} (Ctrl+C to stop)...")
    rec = WavRecorder(path, rate, channels)
    got = 0
    t0 = time.monotonic()
    last_report = t0
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            rec.write(chunk)
            got += len(chunk)
            now = time.monotonic()
            if now - last_report >= 2.0:
                elapsed = max(now - t0, 1e-3)
                xrt = (got / (rate * channels * 2)) / elapsed
                print(
                    f"stats: saved={got} B  xrealtime={xrt:.2f}",
                    flush=True,
                )
                last_report = now
    except KeyboardInterrupt:
        print("stopped.")
    finally:
        elapsed = max(time.monotonic() - t0, 1e-3)
        xrt = (got / (rate * channels * 2)) / elapsed if got else 0.0
        print(f"final: saved={got} B  xrealtime={xrt:.2f}", flush=True)
        rec.close()


def connect(host: str, port: int, retries: int) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 256 * 1024)

    last_err: Exception | None = None
    for i in range(max(1, retries)):
        try:
            sock.connect((host, port))
            return sock
        except OSError as e:
            last_err = e
            if i + 1 < retries:
                print(f"waiting for pcmvirt on {host}:{port}...", flush=True)
                time.sleep(1)
    sock.close()
    raise OSError(f"connect failed: {last_err}")


def session(sock: socket.socket, args: argparse.Namespace) -> None:
    rec: WavRecorder | None = None
    try:
        hdr = read_exact(sock, 44)
        rate, channels, _bits = parse_wav_header(hdr)
        print(f"connected: {rate} Hz, {channels} ch", flush=True)
        if args.save:
            save_wav(sock, rate, channels, args.save)
            return
        if args.record:
            args.record.parent.mkdir(parents=True, exist_ok=True)
            rec = WavRecorder(args.record, rate, channels)
            print(f"recording to {args.record}", flush=True)
        if args.ffplay:
            play_ffplay(sock, rate, channels, rec)
        else:
            try:
                play_sounddevice(sock, rate, channels, rec)
            except ImportError:
                print(
                    "sounddevice missing; install with:\n"
                    '  "D:\\Espressif\\tools\\python_env\\idf5.5_py3.12_env\\Scripts\\python.exe"'
                    " -m pip install sounddevice\n"
                    "or use --ffplay",
                    file=sys.stderr,
                )
                raise SystemExit(1)
    finally:
        if rec is not None:
            rec.close()
        sock.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5558)
    ap.add_argument("--ffplay", action="store_true", help="use ffplay instead of sounddevice")
    ap.add_argument(
        "--save",
        type=Path,
        help="write PCM to a WAV file instead of playing",
    )
    ap.add_argument(
        "--record",
        type=Path,
        help="while playing, also write the same PCM to a WAV file",
    )
    ap.add_argument("--retries", type=int, default=60, help="connect attempts (1/s)")
    ap.add_argument(
        "--reconnect",
        action="store_true",
        help="after the stream ends, wait and connect again (guest restart)",
    )
    args = ap.parse_args()
    if args.save and args.record:
        print("use either --save or --record, not both", file=sys.stderr)
        return 2

    try:
        while True:
            try:
                sock = connect(args.host, args.port, args.retries)
            except OSError as e:
                print(str(e), file=sys.stderr)
                print(
                    "Install/load PCMVIRT.SYS in guest, set audio_out=/dev/pcmvirt",
                    file=sys.stderr,
                )
                return 1
            try:
                session(sock, args)
            except KeyboardInterrupt:
                print("\nstopped.")
                return 0
            except Exception as e:
                print(f"error: {e}", file=sys.stderr)
                if str(e) == "connection closed" and not args.reconnect:
                    print(
                        "hint: guest closed the stream (app SETFMT/restart, or "
                        "pcmvirt not ready). Typical fix:\n"
                        "  1) guest: drv install h:\\pcmvirt.sys\n"
                        "  2) guest: run h:\\grain.axe pcmvirt   (or dx7)\n"
                        "  3) host:  python tools/pcmplay.py --reconnect",
                        file=sys.stderr,
                    )
                if not args.reconnect:
                    return 1
            if not args.reconnect:
                return 0
            print("reconnect: waiting for next stream...", flush=True)
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nstopped.")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
