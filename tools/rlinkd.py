#!/usr/bin/env python3
"""ArgonOS - a fake external radio coprocessor over RLINK, for QEMU.

This is the far end of the guest's UART2: the thing EXTRADIO.SYS talks to when a
board has no radio of its own.  It speaks the RLINK protocol (see
apps/common/rlink/ag_rlink.h) and, being a real process on the development
machine, it makes real TCP connections on the guest's behalf - so a `wget`
issued in the guest after `net use extradio` reaches an actual server (the
netfixture, or anything on the host) through here.  That is the whole point: it
exercises the guest's socket path end to end without any radio hardware.

Transport mirrors HostFS: QEMU opens the guest's UART2 as a client socket
(`-serial tcp:127.0.0.1:<port>,reconnect=1`) and this process is the TCP server.

Run standalone:
    python tools/rlinkd.py --port 5559
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
import threading

# --- protocol constants (keep in step with apps/common/rlink/ag_rlink.h) ----
RL_MAGIC = 0x4B4E4C52  # 'RLNK'
RL_PROTO_VERSION = 2  # v2: START carries Wi-Fi credentials (ssid+pass)
RL_HDR_SIZE = 28
RL_MAX_PAYLOAD = 1600

# ops
RL_OP_HELLO = 0
RL_OP_START = 1
RL_OP_READY = 2
RL_OP_IFADDR = 3
RL_OP_RESOLVE = 4
RL_OP_LISTEN = 5
RL_OP_ACCEPT = 6
RL_OP_CONNECT = 7
RL_OP_SEND = 8
RL_OP_CLOSE = 9
RL_OP_NONBLOCK = 10
RL_OP_DATA = 11
RL_OP_EVENT = 12

# flags
RL_F_RESPONSE = 1 << 0
RL_F_EOF = 1 << 1

# events
RL_EV_LINKUP = 1
RL_EV_GOTIP = 2
RL_EV_LINKDOWN = 3

# capabilities
RL_CAP_SOCKETS = 1 << 0

# ag_err_t values used here (sdk/include/argon/abi.h)
AG_ENOENT = 2
AG_EIO = 5
AG_EBADF = 9
AG_EAGAIN = 11
AG_ENFILE = 23
AG_ETIMEDOUT = 60

MAX_CHAN = 8  # EXT_MAX_CHAN in the driver
FAKE_IFADDR = 0x0A00020F  # 10.0.2.15, host order - cosmetic

# <  little-endian, no alignment padding
#   I magic, B op, B flags, H seq, i status, i ch, I a0, I a1, I len
HDR_FMT = "<IBBHiiIII"
assert struct.calcsize(HDR_FMT) == RL_HDR_SIZE


def pack_hdr(op, flags, seq, status, ch, a0, a1, length):
    return struct.pack(
        HDR_FMT, RL_MAGIC, op & 0xFF, flags & 0xFF, seq & 0xFFFF,
        status, ch, a0 & 0xFFFFFFFF, a1 & 0xFFFFFFFF, length & 0xFFFFFFFF
    )


def dotted(host_order):
    return socket.inet_ntoa(struct.pack(">I", host_order & 0xFFFFFFFF))


def to_host_order(ip_str):
    return struct.unpack(">I", socket.inet_aton(ip_str))[0]


class Radio:
    """One guest connection.  Owns the channels and the single writer lock."""

    def __init__(self, conn):
        self.conn = conn
        self.wlock = threading.Lock()   # serialise frames written to the guest
        self.started = False
        self.chan = {}                  # ch -> socket
        self.listeners = {}             # ch -> listening socket
        self.free = list(range(MAX_CHAN))
        self.lock = threading.Lock()    # protects chan/listeners/free

    # --- wire I/O ---------------------------------------------------------
    def send_frame(self, op, flags, seq, status, ch, a0, a1, payload=b""):
        hdr = pack_hdr(op, flags, seq, status, ch, a0, a1, len(payload))
        with self.wlock:
            self.conn.sendall(hdr + payload)

    def reply(self, req_seq, op, status=0, ch=-1, a0=0, a1=0, payload=b""):
        self.send_frame(op, RL_F_RESPONSE, req_seq, status, ch, a0, a1, payload)

    def push_data(self, ch, payload, eof=False):
        flags = RL_F_EOF if eof else 0
        self.send_frame(RL_OP_DATA, flags, 0, 0, ch, 0, 0, payload)

    def push_event(self, ev, a1=0):
        self.send_frame(RL_OP_EVENT, 0, 0, 0, -1, ev, a1)

    def read_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.conn.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    # --- channels ---------------------------------------------------------
    def alloc_ch(self):
        with self.lock:
            return self.free.pop(0) if self.free else None

    def free_ch(self, ch):
        with self.lock:
            self.chan.pop(ch, None)
            self.listeners.pop(ch, None)
            if ch not in self.free and 0 <= ch < MAX_CHAN:
                self.free.append(ch)

    # --- per-connection receive pump: host socket -> DATA frames ----------
    def pump(self, ch, sock):
        try:
            while True:
                data = sock.recv(RL_MAX_PAYLOAD)
                if not data:
                    self.push_data(ch, b"", eof=True)
                    return
                self.push_data(ch, data)
        except OSError:
            try:
                self.push_data(ch, b"", eof=True)
            except OSError:
                pass
        finally:
            with self.lock:
                # leave the socket for CLOSE to reap; stop reading here
                pass

    def start_pump(self, ch, sock):
        t = threading.Thread(target=self.pump, args=(ch, sock), daemon=True)
        t.start()

    # --- request handlers -------------------------------------------------
    def on_hello(self, h):
        self.reply(h["seq"], RL_OP_HELLO, status=0,
                   a0=RL_PROTO_VERSION, a1=RL_CAP_SOCKETS)

    def on_start(self, h, payload):
        # v2: START may carry Wi-Fi credentials (ssid then passphrase, a0 =
        # ssid length).  A real coprocessor joins that network here; we ride
        # the host's, so we only note it.
        if h["len"]:
            sl = min(h["a0"], len(payload))
            ssid = payload[:sl].decode("ascii", "replace")
            print(f"rlinkd: START join '{ssid}' (pass {len(payload) - sl}b)",
                  flush=True)
        self.started = True
        self.reply(h["seq"], RL_OP_START, status=0)
        # A real modem raises "got address" a moment after the link is up.
        self.push_event(RL_EV_GOTIP, FAKE_IFADDR)

    def on_ready(self, h):
        self.reply(h["seq"], RL_OP_READY, status=1 if self.started else 0)

    def on_ifaddr(self, h):
        self.reply(h["seq"], RL_OP_IFADDR, status=0, a0=FAKE_IFADDR)

    def on_resolve(self, h, payload):
        name = payload.decode("ascii", "replace")
        try:
            info = socket.getaddrinfo(name, None, socket.AF_INET,
                                      socket.SOCK_STREAM)
            ip = info[0][4][0]
            self.reply(h["seq"], RL_OP_RESOLVE, status=0, a0=to_host_order(ip))
        except OSError:
            self.reply(h["seq"], RL_OP_RESOLVE, status=-AG_ENOENT)

    def on_connect(self, h):
        addr = dotted(h["a0"])
        port = h["ch"]
        timeout = h["a1"] / 1000.0 if h["a1"] not in (0, 0xFFFFFFFF) else 10.0
        ch = self.alloc_ch()
        if ch is None:
            self.reply(h["seq"], RL_OP_CONNECT, status=-AG_ENFILE)
            return
        try:
            sock = socket.create_connection((addr, port), timeout=timeout)
            sock.settimeout(None)
        except (OSError, ValueError):
            self.free_ch(ch)
            self.reply(h["seq"], RL_OP_CONNECT, status=-AG_EIO)
            return
        with self.lock:
            self.chan[ch] = sock
        self.reply(h["seq"], RL_OP_CONNECT, status=0, ch=ch)
        self.start_pump(ch, sock)

    def on_listen(self, h):
        port = h["a0"]
        ch = self.alloc_ch()
        if ch is None:
            self.reply(h["seq"], RL_OP_LISTEN, status=-AG_ENFILE)
            return
        try:
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind(("0.0.0.0", port))
            srv.listen(4)
        except OSError:
            self.free_ch(ch)
            self.reply(h["seq"], RL_OP_LISTEN, status=-AG_EIO)
            return
        with self.lock:
            self.listeners[ch] = srv
        self.reply(h["seq"], RL_OP_LISTEN, status=0, ch=ch)

    def on_accept(self, h):
        lch = h["ch"]
        timeout = h["a1"] / 1000.0 if h["a1"] not in (0, 0xFFFFFFFF) else None
        with self.lock:
            srv = self.listeners.get(lch)
        if srv is None:
            self.reply(h["seq"], RL_OP_ACCEPT, status=-AG_EBADF)
            return
        srv.settimeout(timeout if h["a1"] != 0 else 0.0)
        try:
            sock, _ = srv.accept()
        except (socket.timeout, BlockingIOError):
            self.reply(h["seq"], RL_OP_ACCEPT, status=-AG_EAGAIN)
            return
        except OSError:
            self.reply(h["seq"], RL_OP_ACCEPT, status=-AG_EIO)
            return
        finally:
            srv.settimeout(None)
        ch = self.alloc_ch()
        if ch is None:
            sock.close()
            self.reply(h["seq"], RL_OP_ACCEPT, status=-AG_ENFILE)
            return
        with self.lock:
            self.chan[ch] = sock
        self.reply(h["seq"], RL_OP_ACCEPT, status=0, ch=ch)
        self.start_pump(ch, sock)

    def on_send(self, h, payload):
        with self.lock:
            sock = self.chan.get(h["ch"])
        if sock is None:
            self.reply(h["seq"], RL_OP_SEND, status=-AG_EBADF)
            return
        try:
            sock.sendall(payload)
            self.reply(h["seq"], RL_OP_SEND, status=len(payload))
        except OSError:
            self.reply(h["seq"], RL_OP_SEND, status=-AG_EIO)

    def on_close(self, h):
        ch = h["ch"]
        with self.lock:
            sock = self.chan.get(ch) or self.listeners.get(ch)
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
        self.free_ch(ch)
        self.reply(h["seq"], RL_OP_CLOSE, status=0)

    def on_nonblock(self, h):
        # The guest tracks O_NONBLOCK itself (recv_now vs recv); nothing to do
        # here beyond acknowledging.
        self.reply(h["seq"], RL_OP_NONBLOCK, status=0)

    # --- main loop --------------------------------------------------------
    def run(self):
        handlers = {
            RL_OP_HELLO: lambda h, p: self.on_hello(h),
            RL_OP_START: lambda h, p: self.on_start(h, p),
            RL_OP_READY: lambda h, p: self.on_ready(h),
            RL_OP_IFADDR: lambda h, p: self.on_ifaddr(h),
            RL_OP_RESOLVE: lambda h, p: self.on_resolve(h, p),
            RL_OP_CONNECT: lambda h, p: self.on_connect(h),
            RL_OP_LISTEN: lambda h, p: self.on_listen(h),
            RL_OP_ACCEPT: lambda h, p: self.on_accept(h),
            RL_OP_SEND: lambda h, p: self.on_send(h, p),
            RL_OP_CLOSE: lambda h, p: self.on_close(h),
            RL_OP_NONBLOCK: lambda h, p: self.on_nonblock(h),
        }
        while True:
            raw = self.read_exact(RL_HDR_SIZE)
            if raw is None:
                return
            fields = struct.unpack(HDR_FMT, raw)
            (magic, op, flags, seq, status, ch, a0, a1, length) = fields
            if magic != RL_MAGIC or length > RL_MAX_PAYLOAD:
                # desync - drop the byte stream is hard here; bail and let QEMU
                # reconnect.  In practice the guest never sends a bad frame.
                print(f"rlinkd: bad frame magic={magic:#x} len={length}",
                      flush=True)
                return
            payload = self.read_exact(length) if length else b""
            if payload is None:
                return
            h = {"op": op, "flags": flags, "seq": seq, "status": status,
                 "ch": ch, "a0": a0, "a1": a1, "len": length}
            fn = handlers.get(op)
            if fn is None:
                print(f"rlinkd: unhandled op {op}", flush=True)
                continue
            fn(h, payload)


def serve(host, port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(1)
    print(f"rlinkd: fake radio on {host}:{port}", flush=True)
    while True:
        conn, addr = srv.accept()
        print(f"rlinkd: guest UART connected from {addr[0]}:{addr[1]}",
              flush=True)
        try:
            Radio(conn).run()
        except OSError as e:
            print(f"rlinkd: link error: {e}", flush=True)
        finally:
            conn.close()
            print("rlinkd: guest disconnected", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5559)
    args = ap.parse_args()
    try:
        serve(args.host, args.port)
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
