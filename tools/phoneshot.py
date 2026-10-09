#!/usr/bin/env python3
"""Be the phone for a while, and keep what it would have shown.

  python tools/phoneshot.py 127.0.0.1:8765 --seconds 20 --out shot.png
  python tools/phoneshot.py 192.168.4.1:8765 --pass argon --out shot.png

Connects to a PHONE.SYS the way the page does (GET /ws, 'H', the password
answer if one is asked for), then does what the page does with every band:
puts it onto a picture that keeps whatever it is given.  At the end the
picture is written as a PNG and the traffic is counted - bands by encoding,
bytes, geometry messages - so "did the picture arrive, and how" has an answer
that is not somebody looking at a phone.

The decoder is a third copy of the one in apps/common/pixband (C) and
apps/phone/web/index.html (JavaScript).  host-tests/test_phonelink.c holds the
C pair to bit-exactness; this one is checked by the picture it produces.
"""
import argparse
import base64
import hashlib
import os
import socket
import struct
import sys
import time
import zlib

OP_INFO, OP_ROW, OP_CURSOR, OP_AUTH, OP_SCROLL, OP_QUIT = b"MRCASQ"
OP_RAW, OP_PACK, OP_IDX = b"BPI"


def unpack16(data, n):
    out, at = [], 0
    while at < len(data):
        ctl = data[at]
        at += 1
        if ctl & 0x80:
            run = ctl - 0x80 + 2
            v = data[at] | data[at + 1] << 8
            at += 2
            out.extend([v] * run)
        else:
            lit = ctl + 1
            for k in range(lit):
                out.append(data[at] | data[at + 1] << 8)
                at += 2
    return out if len(out) == n else None


def unpack8(data, n):
    out, at = bytearray(), 0
    while at < len(data):
        ctl = data[at]
        at += 1
        if ctl & 0x80:
            out.extend(bytes([data[at]]) * (ctl - 0x80 + 2))
            at += 1
        else:
            lit = ctl + 1
            out.extend(data[at:at + lit])
            at += lit
    return out if len(out) == n else None


def decode(op, data, n):
    if op == OP_RAW:
        if len(data) != n * 2:
            return None
        return list(struct.unpack("<%dH" % n, data))
    if op == OP_PACK:
        return unpack16(data, n)
    if op == OP_IDX:
        bpp, ncol = data[0], data[1] + 1
        pal = struct.unpack("<%dH" % ncol, data[2:2 + ncol * 2])
        nbytes = (n + 1) // 2 if bpp == 4 else n
        idx = unpack8(data[2 + ncol * 2:], nbytes)
        if idx is None:
            return None
        if bpp == 4:
            return [pal[(idx[i >> 1] >> 4) if i & 1 else (idx[i >> 1] & 15)]
                    for i in range(n)]
        return [pal[i] for i in idx[:n]]
    return None


class Link:
    def __init__(self, host, port, timeout):
        self.s = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
                        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                        "Sec-WebSocket-Version: 13\r\n\r\n" % (host, key)).encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.s.recv(1)
            if not chunk:
                raise ConnectionError("closed during the upgrade")
            head += chunk
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise ConnectionError("no upgrade: %r" % head.split(b"\r\n")[0])
        self.buf = b""

    def send(self, payload):
        mask = os.urandom(4)
        n = len(payload)
        hdr = bytes([0x82])
        if n < 126:
            hdr += bytes([0x80 | n])
        else:
            hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
        body = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.s.sendall(hdr + mask + body)

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk

    def recv(self):
        """One message: (opcode, payload)."""
        self._need(2)
        b0, b1 = self.buf[0], self.buf[1]
        n, at = b1 & 0x7F, 2
        if n == 126:
            self._need(4)
            n, at = struct.unpack(">H", self.buf[2:4])[0], 4
        elif n == 127:
            self._need(10)
            n, at = struct.unpack(">Q", self.buf[2:10])[0], 10
        self._need(at + n)
        payload, self.buf = self.buf[at:at + n], self.buf[at + n:]
        return b0 & 0x0F, payload


def write_png(path, w, h, px):
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        for x in range(w):
            v = px[y * w + x]
            rows += bytes(((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63,
                           (v & 31) * 255 // 31))

    def chunk(kind, data):
        c = kind + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(rows), 6)))
        f.write(chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addr", help="host:port of PHONE.SYS")
    ap.add_argument("--pass", dest="password", default="")
    ap.add_argument("--seconds", type=float, default=15.0)
    ap.add_argument("--out", default="phoneshot.png")
    ap.add_argument("--keys", default="",
                    help="keys to press, as the page would: comma-separated "
                         "names (left,right,up,down,enter,esc) or single "
                         "characters, one every --key-every seconds from "
                         "--key-at")
    ap.add_argument("--key-at", type=float, default=8.0)
    ap.add_argument("--key-every", type=float, default=1.0)
    args = ap.parse_args()

    # HID usages, as the page sends them (apps/phone/web/index.html).
    names = {"right": (0x4F, 0), "left": (0x50, 0), "down": (0x51, 0),
             "up": (0x52, 0), "enter": (0x28, 13), "esc": (0x29, 27)}
    def hid_of(ch):
        if ch.isdigit():
            return 0x27 if ch == "0" else 0x1D + int(ch)
        if ch.isalpha():
            return 0x04 + ord(ch.lower()) - ord("a")
        return 0

    keys = []   # (hid, unicode, mods)
    for k in [k for k in args.keys.split(",") if k]:
        mods = 0
        while "+" in k:
            m, k = k.split("+", 1)
            mods |= {"shift": 1, "ctrl": 2, "alt": 4}.get(m.lower(), 0)
        if k.lower() in names:
            hid, uni = names[k.lower()]
        else:
            hid, uni = hid_of(k[0]), (0 if mods & 6 else ord(k[0]))
        keys.append((hid, uni, mods))

    host, _, port = args.addr.rpartition(":")
    link = Link(host, int(port), 5.0)
    link.send(b"H")

    w = h = 0
    px = []
    covered = set()
    counts = {"B": 0, "P": 0, "I": 0}
    nbytes = {"B": 0, "P": 0, "I": 0}
    infos = bad = rows = 0
    first_band = None
    t0 = time.time()
    link.s.settimeout(0.5)
    next_key = t0 + args.key_at
    while time.time() - t0 < args.seconds:
        if keys and time.time() >= next_key:
            hid, uni, mods = keys.pop(0)
            for kind in (1, 2):   # down, up: 'K' kind mods hid uni repeat
                link.send(b"K" + struct.pack("<BBHHB", kind, mods, hid, uni, 0))
            print("  key hid=0x%02x uni=%u mods=%u at %.1f s"
                  % (hid, uni, mods, time.time() - t0))
            next_key = time.time() + args.key_every
        try:
            kind, msg = link.recv()
        except socket.timeout:
            continue
        except ConnectionError as e:
            print("link: %s" % e)
            break
        if kind == 8:
            print("closed by the board: %r" % msg[2:])
            break
        if kind != 2 or not msg:
            continue
        op, body = msg[0], msg[1:]
        if op == OP_AUTH:
            if not args.password:
                print("the board wants a password (--pass)")
                return 2
            link.send(b"A" + hashlib.sha1(body[:16] + args.password.encode())
                      .digest())
        elif op == OP_INFO:
            nw, nh = struct.unpack("<HH", body[:4])
            infos += 1
            flags = body[8] if len(body) > 8 else 0
            print("  geometry %ux%u at %.1f s: %s" % (
                nw, nh, time.time() - t0,
                ("board says " + ("PICTURE" if flags & 2 else "console"))
                if flags & 4 else "board does not say which screen"))
            if (nw, nh) != (w, h):
                w, h = nw, nh
                px = [0] * (w * h)
                covered = set()
        elif op == OP_ROW:
            rows += 1
        elif op in (OP_RAW, OP_PACK, OP_IDX):
            x, y, bw, bh = struct.unpack("<HHHH", body[:8])
            out = decode(op, body[8:], bw * bh)
            k = chr(op)
            if out is None or x + bw > w or y + bh > h:
                bad += 1
                print("  undecodable %s band %u,%u %ux%u, %u bytes, %s"
                      % (k, x, y, bw, bh, len(body) - 8,
                         "outside %ux%u" % (w, h) if out is not None
                         else "does not decode"))
                continue
            if first_band is None:
                first_band = time.time() - t0
            counts[k] += 1
            nbytes[k] += len(body)
            for r in range(bh):
                px[(y + r) * w + x:(y + r) * w + x + bw] = out[r * bw:(r + 1) * bw]
                covered.add(y + r)

    total = sum(counts.values())
    print("geometry %ux%u (%u info messages), %u text rows" % (w, h, infos, rows))
    print("bands: %u (raw %u, pack %u, idx %u), %u bytes; %u undecodable"
          % (total, counts["B"], counts["P"], counts["I"], sum(nbytes.values()),
             bad))
    if first_band is not None:
        print("first band after %.2f s; rows of the picture touched: %u of %u"
              % (first_band, len(covered), h))
    if w and h and total:
        write_png(args.out, w, h, px)
        print("picture -> %s" % args.out)
    return 0 if total and not bad else 1


if __name__ == "__main__":
    sys.exit(main())
