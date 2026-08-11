#!/usr/bin/env python3
"""ArgonOS HostFS helper: serve a host folder to the guest over QEMU UART1 TCP.

Protocol: components/argon_kernel/include/argon/hsfs_proto.h (little-endian).

  python tools/hostfsd.py --root DIR --port 5557
  python tools/hostfsd.py --root DIR --port 5557 --pad-cfg apps/sms/sms.cfg

With --pad-cfg, after the guest PING the helper pushes HSFS_OP_PADPUSH
(~60 Hz) so the guest can cache live Win32 keyboard state.  Guest H:\\sms.pad
then reads that cache (no per-frame RPC).  Push must not start before PING:
early floods break the mount handshake.
"""
from __future__ import annotations

import argparse
import os
import socket
import struct
import sys
import threading
import time
from pathlib import Path, PurePosixPath

_TOOLS = Path(__file__).resolve().parent
if str(_TOOLS) not in sys.path:
    sys.path.insert(0, str(_TOOLS))
from sms_pad import PadState  # noqa: E402

HSFS_MAGIC = 0x53465348
HSFS_MAX_DATA = 4096

OP_PING = 0
OP_STAT = 1
OP_OPENDIR = 2
OP_READDIR = 3
OP_CLOSEDIR = 4
OP_OPEN = 5
OP_READ = 6
OP_CLOSE = 7
OP_PADPUSH = 8
OP_WRITE = 9
OP_UNLINK = 10

MODE_DIR = 1

# Match argon/abi.h AG_O_* (guest open flags on OPEN a0).
AG_O_WRONLY = 1 << 0
AG_O_RDWR = 1 << 1
AG_O_CREATE = 1 << 2
AG_O_TRUNC = 1 << 3
AG_O_APPEND = 1 << 4
AG_O_EXCL = 1 << 5

# Match argon/abi.h style errors (negated).
AG_OK = 0
AG_ENOENT = -2
AG_EIO = -5
AG_EEXIST = -17
AG_EINVAL = -22
AG_ENOTDIR = -20
AG_EISDIR = -21
AG_EROFS = -30
AG_ERANGE = -34

HDR_FMT = "<IHH i IIII"  # magic op seq status a0 a1 path_len data_len
HDR_SIZE = struct.calcsize(HDR_FMT)


def read_exact(conn: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("short read")
        buf.extend(chunk)
    return bytes(buf)


def send_resp(
    conn: socket.socket,
    op: int,
    seq: int,
    status: int,
    a0: int = 0,
    a1: int = 0,
    data: bytes = b"",
    lock: threading.Lock | None = None,
) -> None:
    if len(data) > HSFS_MAX_DATA:
        data = data[:HSFS_MAX_DATA]
    hdr = struct.pack(
        HDR_FMT, HSFS_MAGIC, op, seq, status, a0, a1, 0, len(data)
    )
    payload = hdr + data
    if lock is not None:
        with lock:
            conn.sendall(payload)
    else:
        conn.sendall(payload)


PAD_VPATH = "/sms.pad"


class Session:
    def __init__(self, root: Path, pad: PadState | None = None) -> None:
        self.root = root.resolve()
        self.files: dict[int, object] = {}
        self.dirs: dict[int, list[os.DirEntry]] = {}
        self.next_h = 1
        self.pad = pad
        self.pad_handles: set[int] = set()
        self.send_lock = threading.Lock()
        self._stop = threading.Event()
        self._push_thread: threading.Thread | None = None
        # True while handling a guest RPC (recv request → send reply).
        self._rpc_busy = False
        # Handles opened for write.  PADPUSH must stay off for the whole copy
        # (not only mid-RPC), or WRITE payloads deadlock the shared UART pipe.
        self.writable_handles: set[int] = set()

    def _pad_blocked(self) -> bool:
        return self._rpc_busy or bool(self.writable_handles)

    def start_pad_push(self, conn: socket.socket) -> None:
        if self.pad is None:
            return

        def loop() -> None:
            while not self._stop.is_set():
                # Never push during an RPC or while a writable file is open.
                # PADPUSH + large WRITE payloads deadlock the TCP serial link
                # (host blocked in sendall, guest blocked in uart_write).
                if self._pad_blocked():
                    time.sleep(0.016)
                    continue
                snap = self.pad.snapshot()
                # Prefer the 6-byte form; pad a short snapshot so a mixed
                # host/guest pair still speaks a legal length.
                if len(snap) < 6:
                    snap = (snap + b"\x00\x00\x00\x00\x00\x01")[:6]
                try:
                    with self.send_lock:
                        if self._pad_blocked():
                            pass
                        else:
                            old_to = conn.gettimeout()
                            conn.settimeout(0.05)
                            try:
                                send_resp(
                                    conn,
                                    OP_PADPUSH,
                                    0,
                                    AG_OK,
                                    data=snap,
                                    lock=None,
                                )
                            except (TimeoutError, socket.timeout):
                                pass
                            finally:
                                conn.settimeout(old_to)
                except OSError:
                    return
                time.sleep(0.016)

        self._push_thread = threading.Thread(
            target=loop, name="hsfs-pad-push", daemon=True
        )
        self._push_thread.start()

    def stop_pad_push(self) -> None:
        self._stop.set()
        if self._push_thread is not None:
            self._push_thread.join(timeout=1.0)
            self._push_thread = None

    def is_pad_path(self, path: str) -> bool:
        if self.pad is None:
            return False
        norm = path.replace("\\", "/")
        if not norm.startswith("/"):
            norm = "/" + norm
        return norm.lower() == PAD_VPATH

    def resolve(self, rel: str) -> Path | None:
        # Guest paths are POSIX-ish under mount root ("/", "/foo").
        rel = rel.replace("\\", "/")
        if not rel.startswith("/"):
            rel = "/" + rel
        pure = PurePosixPath(rel)
        parts = [p for p in pure.parts if p not in ("/", ".", "")]
        if any(p == ".." for p in parts):
            return None
        path = self.root.joinpath(*parts)
        try:
            path = path.resolve()
        except OSError:
            return None
        try:
            path.relative_to(self.root)
        except ValueError:
            return None
        return path

    def handle(self, conn: socket.socket) -> None:
        # Do NOT push before the guest PING: QEMU connects UART1 at start and
        # the RX buffer would fill with PADPUSH during boot, so the handshake
        # reads mid-frame and HostFS never mounts.
        try:
            while True:
                try:
                    raw = read_exact(conn, HDR_SIZE)
                except (ConnectionError, OSError):
                    return
                magic, op, seq, _status, a0, a1, path_len, data_len = struct.unpack(
                    HDR_FMT, raw
                )
                if magic != HSFS_MAGIC:
                    return
                if path_len > 512 or data_len > HSFS_MAX_DATA:
                    return
                # Quiesce PADPUSH before reading the rest of the request body
                # (WRITE payloads are up to 4 KiB and share this TCP pipe).
                self._rpc_busy = True
                try:
                    path_b = read_exact(conn, path_len) if path_len else b""
                    # READ: data_len is max bytes wanted; no payload follows.
                    # WRITE: data_len is payload length.
                    payload = b""
                    if data_len and op != OP_READ:
                        payload = read_exact(conn, data_len)
                    path = path_b.decode("utf-8", errors="surrogateescape")
                    try:
                        self.dispatch(
                            conn, op, seq, a0, a1, path, data_len, payload
                        )
                    except Exception as exc:  # noqa: BLE001 — keep link alive
                        send_resp(conn, op, seq, AG_EIO, lock=self.send_lock)
                        print(f"hostfsd: op {op} error: {exc}", file=sys.stderr)
                finally:
                    self._rpc_busy = False
                if (
                    op == OP_PING
                    and self.pad is not None
                    and self._push_thread is None
                    and not self._stop.is_set()
                ):
                    self.start_pad_push(conn)
                    print("hostfsd: PADPUSH started after guest PING", flush=True)
        finally:
            self.stop_pad_push()

    def dispatch(
        self,
        conn: socket.socket,
        op: int,
        seq: int,
        a0: int,
        a1: int,
        path: str,
        data_len: int,
        payload: bytes,
    ) -> None:
        lock = self.send_lock
        if op == OP_PING:
            send_resp(conn, op, seq, AG_OK, a0=1, lock=lock)
            return

        if op == OP_STAT:
            if self.is_pad_path(path):
                send_resp(conn, op, seq, AG_OK, a0=0, a1=6, lock=lock)
                return
            p = self.resolve(path)
            if p is None or not p.exists():
                send_resp(conn, op, seq, AG_ENOENT, lock=lock)
                return
            mode = MODE_DIR if p.is_dir() else 0
            size = 0 if p.is_dir() else p.stat().st_size
            if size > 0xFFFFFFFF:
                size = 0xFFFFFFFF
            send_resp(conn, op, seq, AG_OK, a0=mode, a1=int(size), lock=lock)
            return

        if op == OP_OPENDIR:
            p = self.resolve(path)
            if p is None or not p.exists():
                send_resp(conn, op, seq, AG_ENOENT, lock=lock)
                return
            if not p.is_dir():
                send_resp(conn, op, seq, AG_ENOTDIR, lock=lock)
                return
            entries = sorted(
                list(os.scandir(p)), key=lambda e: e.name.lower()
            )
            h = self.next_h
            self.next_h += 1
            self.dirs[h] = entries
            send_resp(conn, op, seq, AG_OK, a0=h, lock=lock)
            return

        if op == OP_READDIR:
            entries = self.dirs.get(a0)
            if entries is None:
                send_resp(conn, op, seq, AG_EINVAL, lock=lock)
                return
            if not entries:
                send_resp(conn, op, seq, AG_ENOENT, lock=lock)
                return
            ent = entries.pop(0)
            name = ent.name.encode("utf-8", errors="surrogateescape")
            if len(name) > 255:
                name = name[:255]
            mode = MODE_DIR if ent.is_dir(follow_symlinks=False) else 0
            send_resp(conn, op, seq, AG_OK, a0=mode, data=name, lock=lock)
            return

        if op == OP_CLOSEDIR:
            self.dirs.pop(a0, None)
            send_resp(conn, op, seq, AG_OK, lock=lock)
            return

        if op == OP_OPEN:
            flags = int(a0)
            want_write = (flags & (AG_O_WRONLY | AG_O_RDWR | AG_O_CREATE |
                                   AG_O_TRUNC | AG_O_APPEND)) != 0
            if self.is_pad_path(path):
                if want_write:
                    send_resp(conn, op, seq, AG_EROFS, lock=lock)
                    return
                h = self.next_h
                self.next_h += 1
                self.pad_handles.add(h)
                send_resp(conn, op, seq, AG_OK, a0=h, a1=6, lock=lock)
                return
            p = self.resolve(path)
            if p is None:
                send_resp(conn, op, seq, AG_ENOENT, lock=lock)
                return
            if p.exists() and p.is_dir():
                send_resp(conn, op, seq, AG_EISDIR, lock=lock)
                return
            try:
                mode = self.open_mode(flags, p.exists())
            except ValueError:
                send_resp(conn, op, seq, AG_EINVAL, lock=lock)
                return
            if mode is None:
                # EXCL + exists
                send_resp(conn, op, seq, AG_EEXIST, lock=lock)
                return
            try:
                if want_write and not p.parent.exists():
                    send_resp(conn, op, seq, AG_ENOENT, lock=lock)
                    return
                f = open(p, mode)
            except FileNotFoundError:
                send_resp(conn, op, seq, AG_ENOENT, lock=lock)
                return
            except OSError as exc:
                print(f"hostfsd: open {p}: {exc}", file=sys.stderr)
                send_resp(conn, op, seq, AG_EIO, lock=lock)
                return
            h = self.next_h
            self.next_h += 1
            self.files[h] = f
            if want_write:
                self.writable_handles.add(h)
            try:
                size = p.stat().st_size
            except OSError:
                size = 0
            if size > 0xFFFFFFFF:
                size = 0xFFFFFFFF
            send_resp(conn, op, seq, AG_OK, a0=h, a1=int(size), lock=lock)
            return

        if op == OP_READ:
            want = data_len if data_len else HSFS_MAX_DATA
            if want > HSFS_MAX_DATA:
                want = HSFS_MAX_DATA
            if a0 in self.pad_handles:
                if self.pad is None:
                    send_resp(conn, op, seq, AG_EINVAL, lock=lock)
                    return
                blob = self.pad.snapshot()[:want]
                send_resp(
                    conn, op, seq, AG_OK, a0=len(blob), data=blob, lock=lock
                )
                return
            f = self.files.get(a0)
            if f is None:
                send_resp(conn, op, seq, AG_EINVAL, lock=lock)
                return
            f.seek(a1)
            blob = f.read(want)
            send_resp(conn, op, seq, AG_OK, a0=len(blob), data=blob, lock=lock)
            return

        if op == OP_WRITE:
            if a0 in self.pad_handles:
                send_resp(conn, op, seq, AG_EROFS, lock=lock)
                return
            f = self.files.get(a0)
            if f is None:
                send_resp(conn, op, seq, AG_EINVAL, lock=lock)
                return
            if f.writable() is False:
                send_resp(conn, op, seq, AG_EROFS, lock=lock)
                return
            blob = payload
            if len(blob) > HSFS_MAX_DATA:
                blob = blob[:HSFS_MAX_DATA]
            try:
                f.seek(int(a1))
                n = f.write(blob)
                f.flush()
            except OSError as exc:
                print(f"hostfsd: write: {exc}", file=sys.stderr)
                send_resp(conn, op, seq, AG_EIO, lock=lock)
                return
            send_resp(conn, op, seq, AG_OK, a0=int(n), lock=lock)
            return

        if op == OP_CLOSE:
            if a0 in self.pad_handles:
                self.pad_handles.discard(a0)
                send_resp(conn, op, seq, AG_OK, lock=lock)
                return
            f = self.files.pop(a0, None)
            self.writable_handles.discard(a0)
            if f is not None:
                f.close()
            send_resp(conn, op, seq, AG_OK, lock=lock)
            return

        if op == OP_UNLINK:
            if self.is_pad_path(path):
                send_resp(conn, op, seq, AG_EROFS, lock=lock)
                return
            p = self.resolve(path)
            if p is None:
                send_resp(conn, op, seq, AG_ENOENT, lock=lock)
                return
            if not p.exists():
                send_resp(conn, op, seq, AG_ENOENT, lock=lock)
                return
            if p.is_dir():
                send_resp(conn, op, seq, AG_EISDIR, lock=lock)
                return
            try:
                p.unlink()
            except OSError as exc:
                print(f"hostfsd: unlink {p}: {exc}", file=sys.stderr)
                send_resp(conn, op, seq, AG_EIO, lock=lock)
                return
            send_resp(conn, op, seq, AG_OK, lock=lock)
            return

        send_resp(conn, op, seq, AG_EINVAL, lock=lock)

    @staticmethod
    def open_mode(flags: int, exists: bool) -> str | None:
        """Map AG_O_* flags to a Python open mode, or None for EEXIST."""
        want_write = (flags & (AG_O_WRONLY | AG_O_RDWR | AG_O_CREATE |
                               AG_O_TRUNC | AG_O_APPEND)) != 0
        if not want_write:
            return "rb"
        if (flags & AG_O_EXCL) != 0 and exists:
            return None
        if (flags & AG_O_APPEND) != 0:
            if (flags & AG_O_RDWR) != 0:
                return "a+b"
            return "ab"
        if (flags & AG_O_TRUNC) != 0 or (
            (flags & AG_O_CREATE) != 0 and not exists
        ):
            if (flags & AG_O_RDWR) != 0:
                return "w+b"
            return "wb"
        if not exists:
            if (flags & AG_O_CREATE) != 0:
                return "wb" if (flags & AG_O_WRONLY) != 0 else "w+b"
            raise ValueError("missing")
        if (flags & AG_O_RDWR) != 0:
            return "r+b"
        if (flags & AG_O_WRONLY) != 0:
            # Update in place without truncating.
            return "r+b"
        return "rb"


def serve(
    root: Path, host: str, port: int, pad_cfg: Path | None = None
) -> None:
    root = root.resolve()
    if not root.is_dir():
        raise SystemExit(f"hostfsd: not a directory: {root}")

    pad: PadState | None = None
    if pad_cfg is not None:
        cfg_path = pad_cfg if pad_cfg.is_file() else None
        if pad_cfg is not None and not pad_cfg.is_file():
            print(f"hostfsd: pad cfg not found ({pad_cfg}), using defaults",
                  flush=True)
        pad = PadState(cfg_path)
        print(f"hostfsd: PADPUSH ~60 Hz from {cfg_path or 'defaults'}",
              flush=True)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, port))
    sock.listen(1)
    print(f"hostfsd: serving {root} on {host}:{port}", flush=True)

    while True:
        conn, addr = sock.accept()
        print(f"hostfsd: guest {addr[0]}:{addr[1]}", flush=True)
        session = Session(root, pad)
        try:
            session.handle(conn)
        finally:
            for f in session.files.values():
                try:
                    f.close()
                except OSError:
                    pass
            conn.close()
            print("hostfsd: disconnected", flush=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", required=True, type=Path)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5557)
    ap.add_argument(
        "--pad-cfg",
        type=Path,
        default=None,
        help="sms.cfg for live pad push (Win32 GetAsyncKeyState)",
    )
    args = ap.parse_args()
    try:
        serve(args.root, args.host, args.port, pad_cfg=args.pad_cfg)
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
