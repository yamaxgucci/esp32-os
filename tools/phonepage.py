#!/usr/bin/env python3
"""The phone's page, without a phone and without a board.

  python tools/phonepage.py [--port 8765] [--picture | --text | --both]

Serves apps/phone/web/index.html on http://127.0.0.1:8765/ and speaks the
board's own protocol at /ws, so the page can be opened in any browser and made
to show a screen that never existed.

Why it exists: the two-board rig checks everything the board sends and nothing
the page draws - `phonecheck.py` says so itself, and the gap is exactly where
"I have only ever seen text" lives.  PHONECL cannot fill it: a band of pixels
is five kilobytes and that client holds two, so it counts bands and steps over
them.  A browser on this machine can draw them, and a browser on this machine
needs no Wi-Fi, no access point and no second board.

What it sends, and why those:

  text     a screen of characters with every colour pair on it, so a wrong
           attribute nibble is visible rather than merely possible.
  picture  the same shapes GFXPIX draws on the glass - a circle, a diagonal,
           four differently coloured corners.  A circle is the point: a text
           mode cannot draw one, so "graphics" or "characters" is answerable
           from across the room, and the corners each being a different colour
           turns a mirrored or rotated surface into a visibly wrong corner.

Bands go out as RAW ('B'), which is the encoding the board falls back to and
the only one worth writing twice: PACK and IDX are apps/common/pixband's, they
are tested against their own decoder on the host, and a second implementation
of them here would be a second thing to be wrong.

Whatever the page sends back - keys, taps, the hello - is printed, so the
input half can be read too.

Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
"""
import argparse
import base64
import hashlib
import math
import os
import socket
import struct
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(ROOT, "apps", "phone", "web", "index.html")

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
RECORDED = "build/sample.bands"

OP_INFO, OP_ROW, OP_CURSOR, OP_SCROLL = b"M", b"R", b"C", b"S"
OP_RAW = b"B"
IN_HELLO, IN_KEY, IN_PTR, IN_TEXT = 0x48, 0x4B, 0x50, 0x54

COLS, ROWS = 40, 30
CELL_W, CELL_H = 8, 8
SURF_W, SURF_H = 320, 240


# ---- the wire -------------------------------------------------------------

def frame(payload, opcode=0x2):
    """One unmasked WebSocket frame, which is what a server sends."""
    n = len(payload)
    out = bytearray([0x80 | opcode])
    if n < 126:
        out.append(n)
    elif n < 65536:
        out.append(126)
        out += struct.pack(">H", n)
    else:
        out.append(127)
        out += struct.pack(">Q", n)
    out += payload
    return bytes(out)


def read_frame(sock):
    """One frame from the page.  Clients always mask; servers never do."""
    def want(n):
        buf = b""
        while len(buf) < n:
            piece = sock.recv(n - len(buf))
            if not piece:
                return None
            buf += piece
        return buf

    head = want(2)
    if head is None:
        return None
    opcode = head[0] & 0x0F
    masked = (head[1] & 0x80) != 0
    n = head[1] & 0x7F
    if n == 126:
        n = struct.unpack(">H", want(2))[0]
    elif n == 127:
        n = struct.unpack(">Q", want(8))[0]
    key = want(4) if masked else b"\0\0\0\0"
    body = bytearray(want(n) or b"")
    for i in range(len(body)):
        body[i] ^= key[i % 4]
    return opcode, bytes(body)


# ---- what a screen looks like --------------------------------------------

def info():
    return (OP_INFO + struct.pack("<HH", SURF_W, SURF_H) +
            bytes([COLS, ROWS, CELL_W, CELL_H, 0x01]))


def row(y, text, attr=0x07):
    cells = bytearray()
    for x in range(COLS):
        ch = ord(text[x]) if x < len(text) else 32
        cells += bytes([ch & 0xFF, attr])
    return OP_ROW + bytes([y, COLS]) + bytes(cells)


def colour_row(y):
    """Every foreground on every other background, so a swapped nibble shows."""
    cells = bytearray()
    for x in range(COLS):
        attr = ((x // 16) << 4) | (x % 16)
        cells += bytes([ord("#"), attr & 0xFF])
    return OP_ROW + bytes([y, COLS]) + bytes(cells)


def text_screen():
    out = [info()]
    lines = [
        "ArgonOS - the page, with no board behind",
        "",
        "  This screen was made by tools/phonepage",
        "  so the page itself can be looked at.",
        "",
        "  Every colour pair is on the line below:",
    ]
    for i, line in enumerate(lines):
        out.append(row(i, line, 0x07 if i else 0x0F))
    out.append(colour_row(len(lines)))
    out.append(row(ROWS - 1, "C:\\>", 0x07))
    out.append(OP_CURSOR + bytes([4, ROWS - 1, 32, 0x07, 1]))
    return out


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def picture():
    """GFXPIX's shapes: a circle, a diagonal, four coloured corners."""
    px = [0] * (SURF_W * SURF_H)
    cx, cy, rad = SURF_W // 2, SURF_H // 2, min(SURF_W, SURF_H) // 2 - 12
    white = rgb565(230, 230, 230)
    for a in range(0, 3600):
        t = a * math.pi / 1800.0
        x = int(cx + rad * math.cos(t))
        y = int(cy + rad * math.sin(t))
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                xx, yy = x + dx, y + dy
                if 0 <= xx < SURF_W and 0 <= yy < SURF_H:
                    px[yy * SURF_W + xx] = white
    for i in range(min(SURF_W, SURF_H)):
        x = i * SURF_W // min(SURF_W, SURF_H)
        if 0 <= x < SURF_W and 0 <= i < SURF_H:
            px[i * SURF_W + x] = rgb565(80, 160, 255)
    corners = ((0, 0, rgb565(230, 40, 40)), (SURF_W - 40, 0, rgb565(40, 210, 60)),
               (0, SURF_H - 40, rgb565(250, 200, 40)),
               (SURF_W - 40, SURF_H - 40, rgb565(200, 60, 220)))
    for (x0, y0, colour) in corners:
        for y in range(y0, y0 + 40):
            for x in range(x0, x0 + 40):
                px[y * SURF_W + x] = colour
    return px


def picture_bands(px, rows_per_band=8):
    """The same eight-row strips the board sends, as RAW."""
    out = []
    for y in range(0, SURF_H, rows_per_band):
        h = min(rows_per_band, SURF_H - y)
        body = bytearray()
        for yy in range(y, y + h):
            for x in range(SURF_W):
                body += struct.pack("<H", px[yy * SURF_W + x])
        out.append(OP_RAW + struct.pack("<HHHH", 0, y, SURF_W, h) + bytes(body))
    return out


def recorded_bands(path):
    """Bands encoded by the board's own encoder, from build-host/pixband_sample.

    The page has three decoders and this harness could only exercise one of
    them: a board sends palette-and-indices almost every time - measured, 522
    bands of 522 - so the decoder a phone actually uses was the one nobody had
    tried.  Writing the encoder again here to test them would be the mistake
    ag_pixband.h exists to avoid, so the bytes come from the same C the board
    runs and the page is the only thing under test.

    Each record is a 32-bit length and then exactly what goes in a frame.
    """
    out = []
    with open(path, "rb") as f:
        blob = f.read()
    at = 0
    while at + 4 <= len(blob):
        n = struct.unpack_from("<I", blob, at)[0]
        at += 4
        if n == 0 or at + n > len(blob):
            break
        out.append(blob[at:at + n])
        at += n
    return out


def recorded_geometry(bands):
    """How big the picture is, from the bands themselves."""
    w = h = 0
    for b in bands:
        if len(b) < 9:
            continue
        x, y, bw, bh = struct.unpack_from("<HHHH", b, 1)
        w = max(w, x + bw)
        h = max(h, y + bh)
    return w or SURF_W, h or SURF_H


# ---- the server -----------------------------------------------------------

def serve_page(conn, head):
    body = open(PAGE, "rb").read()
    conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                 b"Content-Length: " + str(len(body)).encode() +
                 b"\r\nConnection: close\r\n\r\n" + body)


def serve_ws(conn, head, what):
    key = ""
    for line in head.split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()
    accept = base64.b64encode(
        hashlib.sha1((key + GUID).encode()).digest()).decode()
    conn.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n"
                  % accept).encode())
    print("page connected")

    def send_all():
        if what in ("text", "both"):
            for m in text_screen():
                conn.sendall(frame(m))
        if what in ("picture", "both"):
            conn.sendall(frame(info()))
            for m in picture_bands(picture()):
                conn.sendall(frame(m))
                time.sleep(0.005)
        if what == "recorded":
            bands = recorded_bands(RECORDED)
            w, h = recorded_geometry(bands)
            conn.sendall(frame(OP_INFO + struct.pack("<HH", w, h) +
                               bytes([COLS, ROWS, CELL_W, CELL_H, 0x01])))
            for m in bands:
                conn.sendall(frame(m))
                time.sleep(0.005)
            print("replayed %u recorded bands, %ux%u" % (len(bands), w, h))
        print("sent the %s" % what)

    send_all()
    while True:
        got = read_frame(conn)
        if got is None:
            print("page went away")
            return
        opcode, body = got
        if opcode == 0x8:
            print("page closed the socket")
            return
        if not body:
            continue
        kind = body[0]
        if kind == IN_HELLO:
            print("page said hello")
            send_all()
        elif kind == IN_KEY and len(body) >= 8:
            print("key: kind %u mods %u hid %u unicode %u"
                  % (body[1], body[2], body[3] | (body[4] << 8),
                     body[5] | (body[6] << 8)))
        elif kind == IN_PTR and len(body) >= 7:
            x = struct.unpack("<h", body[3:5])[0]
            y = struct.unpack("<h", body[5:7])[0]
            print("pointer: kind %u buttons %u at %d,%d" % (body[1], body[2], x, y))
        elif kind == IN_TEXT:
            print("text: %r" % body[1:].decode("utf-8", "replace"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--show", choices=("text", "picture", "both", "recorded"),
                    default="picture")
    ap.add_argument("--bands", default="build/sample.bands",
                    help="for --show recorded: a file from "
                         "build-host/pixband_sample, which encodes with the "
                         "same C the board runs")
    args = ap.parse_args()
    global RECORDED
    RECORDED = args.bands

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(8)
    print("http://127.0.0.1:%u/  (showing the %s)" % (args.port, args.show),
          flush=True)

    while True:
        conn, _ = srv.accept()
        head = b""
        while b"\r\n\r\n" not in head:
            piece = conn.recv(4096)
            if not piece:
                break
            head += piece
        if not head:
            conn.close()
            continue
        text = head.decode("latin1")
        first = text.split("\r\n", 1)[0]
        try:
            if "/ws" in first:
                threading.Thread(target=serve_ws,
                                 args=(conn, text, args.show),
                                 daemon=True).start()
                continue
            serve_page(conn, text)
        except Exception as exc:          # a browser closing mid-write
            print("connection: %s" % exc)
        conn.close()


if __name__ == "__main__":
    sys.exit(main())
