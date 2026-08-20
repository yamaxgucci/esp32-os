#!/usr/bin/env python3
"""The other end of the network, for testing ArgonOS without the internet.

Two modes.

  python tools/netfixture.py serve --root build/netfix
      Runs an HTTP server (:8000) and an FTP server (:2121) on this PC.  The
      guest reaches them at 10.0.2.2 through QEMU's user-mode NIC, so a `wget`
      or an `ftp` in the emulator talks to files this script wrote - no
      internet, no flakiness, and the awkward cases on purpose:

        /hello.txt      200 with a length, twenty bytes
        /data.bin       200 with a length, 32 KB of a known pattern
        /chunked.txt    200 with no length, chunked in small pieces
        /moved.txt      302 to /hello.txt
        /missing.txt    404
        /slow.txt       200, written a piece at a time with pauses

      The FTP server greets with a *multi-line* 220 and answers PASV with its
      own LAN address, which the guest cannot reach.  Both are deliberate: they
      are what real servers do, and both broke a client that assumed otherwise.

  python tools/netfixture.py probe --port 5558 --compare build/netfix/data.bin
      Waits for `httpd` in the guest to start listening (QEMU forwards 5558),
      then asks it for things and writes PASS/FAIL lines to --out.  Raw sockets
      rather than urllib, because two of the cases are malformed on purpose.

Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
"""

from __future__ import annotations

import argparse
import os
import socket
import socketserver
import sys
import threading
import time
from pathlib import Path

HELLO = b"hello from the host\n"
DATA_LEN = 32 * 1024


def make_fixtures(root: Path) -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / "hello.txt").write_bytes(HELLO)
    (root / "data.bin").write_bytes(bytes(i & 0xFF for i in range(DATA_LEN)))
    (root / "sub").mkdir(exist_ok=True)
    (root / "sub" / "inner.txt").write_bytes(b"inner\n")


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------


class HttpHandler(socketserver.StreamRequestHandler):
    """Hand-rolled: the fixture needs replies that a helpful library refuses."""

    timeout = 30

    def handle(self) -> None:
        head = b""
        while b"\r\n\r\n" not in head and b"\n\n" not in head:
            chunk = self.rfile.read1(512)
            if not chunk:
                return
            head += chunk
            if len(head) > 8192:
                return

        line = head.split(b"\r\n", 1)[0].decode("latin-1", "replace")
        parts = line.split()
        if len(parts) < 2:
            return
        method, target = parts[0], parts[1]
        print(f"http: {method} {target}", flush=True)

        root: Path = self.server.fixture_root  # type: ignore[attr-defined]
        body_only = method == "HEAD"

        if target in ("/hello.txt", "/"):
            self.reply(200, b"text/plain", HELLO, body_only)
        elif target == "/data.bin":
            self.reply(200, b"application/octet-stream",
                       (root / "data.bin").read_bytes(), body_only)
        elif target == "/chunked.txt":
            self.chunked(HELLO * 20, body_only)
        elif target == "/slow.txt":
            self.slow(HELLO * 10, body_only)
        elif target == "/moved.txt":
            self.wfile.write(b"HTTP/1.1 302 Found\r\nLocation: /hello.txt\r\n"
                             b"Content-Length: 0\r\nConnection: close\r\n\r\n")
        elif target == "/relative":
            # A Location without a scheme and without a leading slash.
            self.wfile.write(b"HTTP/1.1 302 Found\r\nLocation: hello.txt\r\n"
                             b"Content-Length: 0\r\nConnection: close\r\n\r\n")
        elif target == "/missing.txt":
            self.reply(404, b"text/plain", b"no\n", body_only)
        else:
            self.reply(404, b"text/plain", b"no such fixture\n", body_only)

    def reply(self, status: int, ctype: bytes, body: bytes, head_only: bool) -> None:
        text = {200: b"OK", 404: b"Not Found"}[status]
        self.wfile.write(b"HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                         b"Content-Length: %d\r\nConnection: close\r\n\r\n"
                         % (status, text, ctype, len(body)))
        if not head_only:
            self.wfile.write(body)

    def chunked(self, body: bytes, head_only: bool) -> None:
        self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                         b"Transfer-Encoding: chunked\r\n"
                         b"Connection: close\r\n\r\n")
        if head_only:
            return
        step = 77  # not a power of two, so chunks straddle the read buffer
        for at in range(0, len(body), step):
            piece = body[at:at + step]
            self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
        self.wfile.write(b"0\r\n\r\n")

    def slow(self, body: bytes, head_only: bool) -> None:
        """No length, and written with gaps: the body ends when the socket does."""
        self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                         b"Connection: close\r\n\r\n")
        if head_only:
            return
        for at in range(0, len(body), 32):
            self.wfile.write(body[at:at + 32])
            self.wfile.flush()
            time.sleep(0.05)


class HttpServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


# ---------------------------------------------------------------------------
# FTP
# ---------------------------------------------------------------------------


class FtpHandler(socketserver.StreamRequestHandler):
    timeout = 60

    def send(self, text: str) -> None:
        print(f"ftp  > {text}", flush=True)
        self.wfile.write((text + "\r\n").encode("utf-8"))

    def handle(self) -> None:
        self.root: Path = self.server.fixture_root  # type: ignore[attr-defined]
        self.cwd = "/"
        self.listener: socket.socket | None = None

        # Multi-line greeting on purpose: a client that reads one line and
        # stops answers every later command with the tail of this one.
        self.send("220-argon test server")
        self.send("220-  this line is here to be skipped")
        self.send("220 ready")

        while True:
            raw = self.rfile.readline()
            if not raw:
                break
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            print(f"ftp  < {line}", flush=True)
            verb, _, arg = line.partition(" ")
            verb = verb.upper()
            arg = arg.strip()

            if verb == "USER":
                self.send("331 password, any password")
            elif verb == "PASS":
                self.send("230 logged in")
            elif verb in ("TYPE", "MODE", "STRU", "NOOP"):
                self.send("200 fine")
            elif verb == "SYST":
                self.send("215 UNIX Type: L8")
            elif verb == "PWD":
                self.send(f'257 "{self.cwd}" is the current directory')
            elif verb == "CWD":
                self.cwd_to(arg)
            elif verb == "CDUP":
                self.cwd_to("..")
            elif verb == "PASV":
                self.pasv()
            elif verb == "LIST":
                self.do_list(arg)
            elif verb == "RETR":
                self.do_retr(arg)
            elif verb == "STOR":
                self.do_stor(arg)
            elif verb == "SIZE":
                self.do_size(arg)
            elif verb == "DELE":
                self.do_dele(arg)
            elif verb == "MKD":
                self.do_mkd(arg)
            elif verb == "RMD":
                self.do_rmd(arg)
            elif verb == "QUIT":
                self.send("221 bye")
                break
            else:
                self.send(f"502 {verb} is not implemented here")

        if self.listener is not None:
            self.listener.close()

    # -- paths --------------------------------------------------------------

    def local(self, arg: str) -> Path | None:
        """A guest path into a real one, refusing anything outside the root."""
        base = self.cwd if arg[:1] != "/" else "/"
        joined = os.path.normpath(os.path.join(base, arg)).replace("\\", "/")
        if not joined.startswith("/"):
            return None
        target = (self.root / joined.lstrip("/")).resolve()
        try:
            target.relative_to(self.root.resolve())
        except ValueError:
            return None
        return target

    def cwd_to(self, arg: str) -> None:
        target = self.local(arg or "/")
        if target is None or not target.is_dir():
            self.send("550 no such directory")
            return
        rel = target.resolve().relative_to(self.root.resolve()).as_posix()
        self.cwd = "/" + ("" if rel == "." else rel)
        self.send(f'250 now in {self.cwd}')

    # -- data connection ---------------------------------------------------

    def pasv(self) -> None:
        if self.listener is not None:
            self.listener.close()
        self.listener = socket.socket()
        self.listener.bind(("0.0.0.0", 0))
        self.listener.listen(1)
        port = self.listener.getsockname()[1]

        # Deliberately this machine's LAN address, which the guest cannot
        # reach: a client that trusts it hangs here, and a great many real
        # servers behind NAT say exactly this.
        try:
            lan = socket.gethostbyname(socket.gethostname())
        except OSError:
            lan = "127.0.0.1"
        octets = lan.split(".")
        if len(octets) != 4:
            octets = ["127", "0", "0", "1"]
        self.send("227 Entering Passive Mode (%s,%d,%d)"
                  % (",".join(octets), port >> 8, port & 0xFF))

    def data_accept(self) -> socket.socket | None:
        if self.listener is None:
            self.send("425 use PASV first")
            return None
        self.listener.settimeout(30)
        try:
            conn, _ = self.listener.accept()
        except socket.timeout:
            self.send("425 nobody connected")
            return None
        finally:
            self.listener.close()
            self.listener = None
        return conn

    # -- transfers ---------------------------------------------------------

    def do_list(self, arg: str) -> None:
        target = self.local(arg) if arg else self.local(".")
        if target is None or not target.exists():
            self.send("550 no such path")
            return
        self.send("150 here it comes")
        conn = self.data_accept()
        if conn is None:
            return
        entries = sorted(target.iterdir()) if target.is_dir() else [target]
        for entry in entries:
            kind = "d" if entry.is_dir() else "-"
            size = 0 if entry.is_dir() else entry.stat().st_size
            conn.sendall(("%srw-r--r-- 1 argon argon %8d Jan  1 00:00 %s\r\n"
                          % (kind, size, entry.name)).encode("utf-8"))
        conn.close()
        self.send("226 that was the listing")

    def do_retr(self, arg: str) -> None:
        target = self.local(arg)
        if target is None or not target.is_file():
            self.send("550 no such file")
            return
        self.send("150 sending")
        conn = self.data_accept()
        if conn is None:
            return
        conn.sendall(target.read_bytes())
        conn.close()
        self.send("226 sent")

    def do_stor(self, arg: str) -> None:
        target = self.local(arg)
        if target is None:
            self.send("550 not a writable name")
            return
        self.send("150 go ahead")
        conn = self.data_accept()
        if conn is None:
            return
        with open(target, "wb") as out:
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                out.write(chunk)
        conn.close()
        self.send("226 stored")

    def do_size(self, arg: str) -> None:
        target = self.local(arg)
        if target is None or not target.is_file():
            self.send("550 no such file")
        else:
            self.send(f"213 {target.stat().st_size}")

    def do_dele(self, arg: str) -> None:
        target = self.local(arg)
        if target is None or not target.is_file():
            self.send("550 no such file")
            return
        target.unlink()
        self.send("250 deleted")

    def do_mkd(self, arg: str) -> None:
        target = self.local(arg)
        if target is None:
            self.send("550 no")
            return
        target.mkdir(exist_ok=True)
        self.send(f'257 "{arg}" created')

    def do_rmd(self, arg: str) -> None:
        target = self.local(arg)
        if target is None or not target.is_dir():
            self.send("550 no such directory")
            return
        try:
            target.rmdir()
        except OSError as exc:
            self.send(f"550 {exc}")
            return
        self.send("250 removed")


class FtpServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


# ---------------------------------------------------------------------------
# The guest's own server, asked questions
# ---------------------------------------------------------------------------


def ask(port: int, request: bytes, timeout: float = 20.0) -> bytes:
    """One request over one connection, the whole reply back."""
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(request)
        out = bytearray()
        while True:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            out += chunk
    return bytes(out)


def split_reply(reply: bytes) -> tuple[bytes, bytes]:
    at = reply.find(b"\r\n\r\n")
    if at < 0:
        return reply, b""
    return reply[:at], reply[at + 4:]


def probe(port: int, compare: Path | None, out_path: Path,
          wait_sec: float) -> int:
    results: list[str] = []

    def note(ok: bool, name: str, detail: str = "") -> None:
        results.append(("PASS " if ok else "FAIL ") + name +
                       (f" - {detail}" if detail else ""))
        print(results[-1], flush=True)

    # The guest takes as long as it takes to boot and reach the command.
    deadline = time.time() + wait_sec
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=2):
                break
        except OSError:
            time.sleep(0.5)
    else:
        note(False, "httpd-listening", f"nothing on :{port} after {wait_sec}s")
        out_path.write_text("\n".join(results) + "\n", encoding="utf-8")
        return 1
    note(True, "httpd-listening")

    # A file, byte for byte.  This is the whole point of the round trip: the
    # guest fetched data.bin over HTTP and is now serving it back.
    if compare is not None:
        want = compare.read_bytes()
        head, body = split_reply(ask(port, b"GET /data.bin HTTP/1.0\r\n"
                                          b"Host: argon\r\n\r\n"))
        ok = head.startswith(b"HTTP/1.1 200")
        note(ok, "httpd-status-200", head.split(b"\r\n")[0].decode("latin-1"))
        note(body == want, "httpd-bytes-match",
             f"{len(body)} bytes, wanted {len(want)}")
        note(b"Content-Length: %d" % len(want) in head, "httpd-content-length")
        note(b"application/octet-stream" in head, "httpd-content-type")

    head, body = split_reply(ask(port, b"GET / HTTP/1.0\r\nHost: argon\r\n\r\n"))
    note(head.startswith(b"HTTP/1.1 200") and b"data.bin" in body,
         "httpd-listing", head.split(b"\r\n")[0].decode("latin-1"))

    head, _ = split_reply(ask(port, b"HEAD /data.bin HTTP/1.0\r\n\r\n"))
    note(head.startswith(b"HTTP/1.1 200") and b"Content-Length:" in head,
         "httpd-head")

    head, _ = split_reply(ask(port, b"GET /nothing.here HTTP/1.0\r\n\r\n"))
    note(head.startswith(b"HTTP/1.1 404"), "httpd-404",
         head.split(b"\r\n")[0].decode("latin-1"))

    # Climbing out of the served directory, spelled two ways.
    head, _ = split_reply(ask(port, b"GET /../sdkconfig HTTP/1.0\r\n\r\n"))
    note(head.startswith(b"HTTP/1.1 403"), "httpd-refuses-dotdot",
         head.split(b"\r\n")[0].decode("latin-1"))
    head, _ = split_reply(ask(port, b"GET /%2e%2e/sdkconfig HTTP/1.0\r\n\r\n"))
    note(head.startswith(b"HTTP/1.1 403"), "httpd-refuses-encoded-dotdot",
         head.split(b"\r\n")[0].decode("latin-1"))

    head, _ = split_reply(ask(port, b"PUT /data.bin HTTP/1.0\r\n\r\n"))
    note(head.startswith(b"HTTP/1.1 405"), "httpd-refuses-put",
         head.split(b"\r\n")[0].decode("latin-1"))

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(results) + "\n", encoding="utf-8")
    return 0 if all(r.startswith("PASS") for r in results) else 1


# ---------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode", choices=("serve", "probe"))
    ap.add_argument("--root", default="build/netfix")
    ap.add_argument("--http-port", type=int, default=8000)
    ap.add_argument("--ftp-port", type=int, default=2121)
    ap.add_argument("--port", type=int, default=5558, help="probe: guest httpd")
    ap.add_argument("--compare", default="")
    ap.add_argument("--out", default="build/httpd_probe.txt")
    ap.add_argument("--wait", type=float, default=180.0)
    args = ap.parse_args()

    if args.mode == "probe":
        return probe(args.port,
                     Path(args.compare) if args.compare else None,
                     Path(args.out), args.wait)

    root = Path(args.root).resolve()
    make_fixtures(root)

    # A port that accepts and then says nothing, ever.  This is how a receive
    # timeout is tested: without one, a client here waits for the heat death of
    # the universe, and that is a defect that only shows up against a server
    # that has gone away rather than one that refuses.
    silent = socket.socket()
    silent.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    silent.bind(("0.0.0.0", args.http_port + 1))
    silent.listen(4)
    held: list[socket.socket] = []

    def hold_silently() -> None:
        while True:
            conn, _ = silent.accept()
            print(f"silent: accepted from {conn.getpeername()}", flush=True)
            held.append(conn)

    threading.Thread(target=hold_silently, daemon=True).start()

    http = HttpServer(("0.0.0.0", args.http_port), HttpHandler)
    http.fixture_root = root  # type: ignore[attr-defined]
    ftp = FtpServer(("0.0.0.0", args.ftp_port), FtpHandler)
    ftp.fixture_root = root  # type: ignore[attr-defined]

    threading.Thread(target=http.serve_forever, daemon=True).start()
    threading.Thread(target=ftp.serve_forever, daemon=True).start()
    print(f"http on :{args.http_port}, silent on :{args.http_port + 1}, "
          f"ftp on :{args.ftp_port}, root {root}",
          flush=True)
    print("the guest reaches this PC at 10.0.2.2", flush=True)

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
