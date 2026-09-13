#!/usr/bin/env python3
"""A board-shaped stand-in for PHONE.SYS, so the page can be checked on a PC.

The client half of this link - apps/phone/web/index.html - is the half neither
QEMU nor the board can check.  A guest can prove the driver serves the page and
upgrades the socket; it cannot prove that the page draws CP437 box drawing
correctly, that a palette band comes out as the right picture, or that a tap
lands where a finger was.  Those need a browser, and a browser needs something
to talk to.

So this speaks the board's side of the protocol - the same op bytes, the same
encodings - and prints every message it gets back.  Then:

    python tools/phonefixture.py
    (open http://127.0.0.1:8765/ in a browser)

What it shows: a console screen built out of the characters that actually catch
a mistake (box drawing, the sixteen CGA colours, a blinking caret), and, on
demand, a picture sent in bands through all three encodings.  Press a key or tap
in the browser and it appears here.

Nothing is imported that is not in the standard library: this has to run without
anybody installing a WebSocket package first.

  --port N       listen somewhere else
  --picture      send the picture instead of the console

Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import base64
import hashlib
import os
import socket
import struct
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(ROOT, "apps", "phone", "web", "index.html")

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

OP_INFO, OP_ROW, OP_CURSOR = b"M", b"R", b"C"
OP_RAW, OP_PACK, OP_IDX = b"B", b"P", b"I"

COLS, ROWS = 80, 25
SURF_W, SURF_H = 320, 240


# ---- WebSocket, the server half ----------------------------------------

def ws_frame(payload, opcode=2):
    """One unmasked binary frame, which is what a server always sends."""
    n = len(payload)
    if n < 126:
        head = struct.pack("!BB", 0x80 | opcode, n)
    elif n <= 0xFFFF:
        head = struct.pack("!BBH", 0x80 | opcode, 126, n)
    else:
        head = struct.pack("!BBQ", 0x80 | opcode, 127, n)
    return head + payload


def ws_read(sock, buf):
    """Pull one whole frame out of `buf`, growing it from the socket.

    Returns (opcode, payload, buf) or (None, None, buf) when the peer has gone.
    """
    while True:
        if len(buf) >= 2:
            b0, b1 = buf[0], buf[1]
            masked = b1 & 0x80
            n = b1 & 0x7F
            at = 2
            if n == 126 and len(buf) >= 4:
                n = struct.unpack("!H", buf[2:4])[0]
                at = 4
            elif n == 127 and len(buf) >= 10:
                n = struct.unpack("!Q", buf[2:10])[0]
                at = 10
            elif n >= 126:
                n = None
            if n is not None:
                need = at + (4 if masked else 0) + n
                if len(buf) >= need:
                    mask = buf[at:at + 4] if masked else b"\0\0\0\0"
                    at += 4 if masked else 0
                    body = bytearray(buf[at:at + n])
                    if masked:
                        for i in range(n):
                            body[i] ^= mask[i & 3]
                    return b0 & 0x0F, bytes(body), buf[need:]
        chunk = sock.recv(4096)
        if not chunk:
            return None, None, buf
        buf += chunk


# ---- the encodings, mirroring apps/common/pixband/ag_pixband.c ----------

def pack16(px):
    out = bytearray()
    i = 0
    n = len(px)
    while i < n:
        run = 1
        while run < 129 and i + run < n and px[i + run] == px[i]:
            run += 1
        if run >= 2:
            out.append(0x80 + (run - 2))
            out += struct.pack("<H", px[i])
            i += run
            continue
        lit = 1
        while lit < 128 and i + lit + 1 < n and px[i + lit] != px[i + lit + 1]:
            lit += 1
        if i + lit > n:
            lit = n - i
        out.append(lit - 1)
        for k in range(lit):
            out += struct.pack("<H", px[i + k])
        i += lit
    return bytes(out)


def pack8(b):
    out = bytearray()
    i = 0
    n = len(b)
    while i < n:
        run = 1
        while run < 129 and i + run < n and b[i + run] == b[i]:
            run += 1
        if run >= 3:
            out.append(0x80 + (run - 2))
            out.append(b[i])
            i += run
            continue
        lit = 1
        while (lit < 128 and i + lit + 2 < n and
               not (b[i + lit] == b[i + lit + 1] == b[i + lit + 2])):
            lit += 1
        if i + lit > n:
            lit = n - i
        out.append(lit - 1)
        out += bytes(b[i:i + lit])
        i += lit
    return bytes(out)


def encode_band(px, w, h, force=None):
    """The smallest of the three, or the one named - geometry included."""
    geom = struct.pack("<HHHH", 0, 0, w, h)  # x/y filled in by the caller
    raw = b"".join(struct.pack("<H", c) for c in px)

    cands = [(OP_RAW, raw)]
    cands.append((OP_PACK, pack16(px)))

    pal, idx = [], []
    for c in px:
        if c not in pal:
            if len(pal) == 256:
                pal = None
                break
            pal.append(c)
        idx.append(pal.index(c))
    if pal:
        bpp = 4 if len(pal) <= 16 else 8
        body = bytearray([bpp, len(pal) - 1])
        for c in pal:
            body += struct.pack("<H", c)
        if bpp == 4:
            packed = bytearray()
            for i in range(0, len(idx), 2):
                lo = idx[i]
                hi = idx[i + 1] if i + 1 < len(idx) else 0
                packed.append((hi << 4) | (lo & 0x0F))
        else:
            packed = bytearray(idx)
        body += pack8(bytes(packed))
        cands.append((OP_IDX, bytes(body)))

    if force is not None:
        for op, body in cands:
            if op == force:
                return op, geom, body
    op, body = min(cands, key=lambda c: len(c[1]))
    return op, geom, body


# ---- what the board would be showing -----------------------------------

def console_screen():
    """A screen chosen to catch the mistakes a screen can hide.

    Box drawing, because the file manager is made of it and CP437's bytes are
    not Unicode's; every one of the sixteen colours on every one of the eight
    backgrounds, because an attribute nibble read the wrong way round looks
    plausible; and the block characters, which is where a font substitution
    shows up as gaps.
    """
    rows = [[(0x20, 0x07)] * COLS for _ in range(ROWS)]

    def put(row, col, text, attr=0x07):
        for i, ch in enumerate(text):
            if col + i < COLS:
                rows[row][col + i] = (ch if isinstance(ch, int) else ord(ch),
                                      attr)

    put(0, 0, "ArgonOS  phone link fixture" + " " * 53, 0x1F)

    # A box, in CP437's own bytes.
    tl, tr, bl, br, hz, vt = 0xC9, 0xBB, 0xC8, 0xBC, 0xCD, 0xBA
    put(2, 2, [tl] + [hz] * 30 + [tr], 0x0B)
    for r in range(3, 7):
        put(r, 2, [vt], 0x0B)
        put(r, 33, [vt], 0x0B)
    put(7, 2, [bl] + [hz] * 30 + [br], 0x0B)
    put(3, 4, "single line:", 0x0F)
    put(4, 4, [0xDA] + [0xC4] * 10 + [0xBF], 0x0E)
    put(5, 4, [0xB3] + [0x20] * 10 + [0xB3], 0x0E)
    put(6, 4, [0xC0] + [0xC4] * 10 + [0xD9], 0x0E)

    put(9, 2, "colours:", 0x0F)
    for fg in range(16):
        put(10, 12 + fg * 3, "%X%X" % (fg, fg), fg)
    for bg in range(8):
        put(11, 12 + bg * 3, "  ", (bg << 4) | 0x0F)

    put(13, 2, "blocks:", 0x0F)
    put(13, 12, [0xB0, 0xB1, 0xB2, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF], 0x0A)

    put(15, 2, "greek/maths:", 0x0F)
    put(15, 16, [0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xF0, 0xF1, 0xF2, 0xF3,
                 0xF7, 0xF8, 0xFB, 0xFC], 0x0D)

    put(17, 2, "C:\\>", 0x07)
    put(19, 2, "type here - every key comes back to the terminal that", 0x08)
    put(20, 2, "started this fixture.  tap anywhere for a pointer event.", 0x08)
    return rows


def picture(w, h):
    """Something with flat areas, a gradient and noise, so all three encodings
    get used on one screen and a wrong one is visible rather than merely
    smaller."""
    px = []
    for y in range(h):
        for x in range(w):
            if y < h // 3:
                c = 0x001F if (x // 40) % 2 == 0 else 0xF800   # flat: packbits
            elif y < 2 * h // 3:
                c = ((x * 31 // w) << 11) | ((y * 63 // h) << 5)  # gradient
            else:
                c = ((x * 7 + y * 13) * 2654435761) & 0xFFFF      # noise: raw
            px.append(c)
    return px


# ---- the session --------------------------------------------------------

def serve(conn, args):
    buf = b""
    # The request, to the blank line.
    while b"\r\n\r\n" not in buf:
        chunk = conn.recv(4096)
        if not chunk:
            return
        buf += chunk

    # The head comes off the buffer, not just out of it: whatever the client
    # sent after the blank line is the first WebSocket frame, and leaving the
    # request in front of it makes the frame reader parse HTTP as a frame.
    head, buf = buf.split(b"\r\n\r\n", 1)
    head = head.decode("latin1")
    key = None
    for line in head.split("\r\n")[1:]:
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip().encode()

    if key is None:
        with open(PAGE, "rb") as f:
            page = f.read().replace(b"\r\n", b"\n")
        conn.sendall(
            b"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
            b"Content-Length: %d\r\nConnection: close\r\n\r\n" % len(page)
            + page)
        conn.close()
        print("served the page (%u bytes)" % len(page))
        return

    accept = base64.b64encode(hashlib.sha1(key + GUID).digest()).decode()
    conn.sendall(
        ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
         "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n"
         % accept).encode())
    print("client connected")

    stop = threading.Event()

    def pusher():
        conn.sendall(ws_frame(OP_INFO + struct.pack("<HHBBBBB", SURF_W, SURF_H,
                                                    COLS, ROWS, 8, 16, 1)))
        if args.picture:
            px = picture(SURF_W, SURF_H)
            for y in range(0, SURF_H, 8):
                band = px[y * SURF_W:(y + 8) * SURF_W]
                op, _, body = encode_band(band, SURF_W, 8)
                geom = struct.pack("<HHHH", 0, y, SURF_W, 8)
                conn.sendall(ws_frame(op + geom + body))
            print("picture sent, %ux%u in bands of 8" % (SURF_W, SURF_H))
            return

        rows = console_screen()
        for y, row in enumerate(rows):
            body = bytearray([y, COLS])
            for ch, attr in row:
                body += bytes([ch & 0xFF, attr])
            conn.sendall(ws_frame(OP_ROW + bytes(body)))

        # The caret, blinking, which is also this fixture's heartbeat.
        lit = True
        while not stop.is_set():
            conn.sendall(ws_frame(OP_CURSOR +
                                  bytes([6, 17, 0x20, 0x07, 1 if lit else 0])))
            lit = not lit
            stop.wait(0.53)

    t = threading.Thread(target=pusher, daemon=True)
    t.start()

    try:
        while True:
            op, body, buf = ws_read(conn, buf)
            if op is None or op == 8:
                break
            if op == 9:
                conn.sendall(ws_frame(body, opcode=10))
                continue
            if not body:
                continue
            kind = chr(body[0])
            rest = body[1:]
            if kind == "H":
                print("<- hello (asking for everything)")
            elif kind == "K" and len(rest) >= 7:
                down = "down" if rest[0] == 1 else "up  "
                hid = struct.unpack("<H", rest[2:4])[0]
                uni = struct.unpack("<H", rest[4:6])[0]
                print("<- key  %s mods=%02x hid=%-3u unicode=%-5u %s"
                      % (down, rest[1], hid, uni,
                         repr(chr(uni)) if 32 <= uni < 0x110000 else ""))
            elif kind == "P" and len(rest) >= 7:
                x = struct.unpack("<H", rest[2:4])[0]
                y = struct.unpack("<H", rest[4:6])[0]
                print("<- ptr  kind=%u buttons=%u at %u,%u"
                      % (rest[0], rest[1], x, y))
            elif kind == "T":
                # The bytes as well as the text: a Windows console cannot
                # always print what arrives, and "??????" looks exactly like a
                # protocol that lost the characters when it is only a terminal
                # that cannot show them.
                print("<- text %s = %r"
                      % (rest.hex(), rest.decode("utf-8", "replace")))
            else:
                print("<- %r" % body[:32])
    except (ConnectionError, OSError):
        pass
    finally:
        stop.set()
        conn.close()
        print("client gone")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--picture", action="store_true",
                    help="send a picture in bands instead of the console")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(4)
    print("phonefixture: http://127.0.0.1:%u/  (Ctrl+C to stop)" % args.port)

    try:
        while True:
            conn, _ = srv.accept()
            threading.Thread(target=serve, args=(conn, args),
                             daemon=True).start()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
