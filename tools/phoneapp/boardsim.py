#!/usr/bin/env python3
"""A pretend ArgonOS board on this PC's Bluetooth, for the phone app.

    python tools/phoneapp/boardsim.py [--password argon] [--piece 20]

It is the GATT side of PHONE.SYS - service 0xfff0, writes on 0xfff2, pieces
of messages as notifications on 0xfff1 - in front of a small fake console, so
the Android app can be tried end to end with no board: search, connect, the
password, the screen, the keys, a second phone taking the screen.  What it
is NOT is the board: the console here understands `help`, `ver`, `cls`,
`echo` and `dir`, and nothing else.

The protocol is apps/phone/phone.c's, and this file follows it rather than
defines it; if the two ever disagree, the board is right.

Needs the `bless` package (a BLE peripheral for Windows/macOS/Linux) and a
Bluetooth adapter that can be a peripheral - most built-in ones can.
"""
import argparse
import asyncio
import hashlib
import logging
import os
import sys

try:
    from bless import (BlessServer, GATTAttributePermissions,
                       GATTCharacteristicProperties)
except ImportError:
    sys.exit("boardsim: needs bless (pip install bless)")

SVC = "0000fff0-0000-1000-8000-00805f9b34fb"
CHR_IN = "0000fff1-0000-1000-8000-00805f9b34fb"   # board to phone, notify
CHR_OUT = "0000fff2-0000-1000-8000-00805f9b34fb"  # phone to board, write

FIRST, LAST = 0x40, 0x80
COLS, ROWS = 40, 16
HID_ENTER, HID_BKSP, HID_ESC = 0x28, 0x2A, 0x29


class Console:
    """Just enough of a console to see the link work."""

    def __init__(self):
        self.cells = [[(" ", 0x07)] * COLS for _ in range(ROWS)]
        self.col = 0
        self.row = 0
        self.line = ""
        self.dirty = set()
        self.scrolled = 0
        self.put("ArgonOS board simulator (tools/phoneapp)\n")
        self.put("Not a board: help, ver, cls, echo, dir.\n\n")
        self.prompt()

    def prompt(self):
        self.put("A:\\> ", 0x0B)

    def newline(self):
        self.col = 0
        self.row += 1
        if self.row >= ROWS:
            self.cells.pop(0)
            self.cells.append([(" ", 0x07)] * COLS)
            self.row = ROWS - 1
            self.scrolled += 1

    def put(self, text, attr=0x07):
        for ch in text:
            if ch == "\n":
                self.newline()
                continue
            self.cells[self.row][self.col] = (ch, attr)
            self.dirty.add(self.row)
            self.col += 1
            if self.col >= COLS:
                self.newline()

    def key(self, uni, hid):
        if hid == HID_ENTER or uni in (10, 13):
            self.put("\n")
            self.run(self.line.strip())
            self.line = ""
            self.prompt()
        elif hid == HID_BKSP or uni == 8:
            if self.line:
                self.line = self.line[:-1]
                self.col -= 1
                self.cells[self.row][self.col] = (" ", 0x07)
                self.dirty.add(self.row)
        elif hid == HID_ESC:
            pass
        elif 32 <= uni < 127:
            self.line += chr(uni)
            self.put(chr(uni))

    def run(self, cmd):
        word, _, rest = cmd.partition(" ")
        word = word.lower()
        if word == "":
            return
        if word == "help":
            self.put("help ver cls echo dir\n")
        elif word == "ver":
            self.put("ArgonOS boardsim 1.0 - protocol of PHONE.SYS 1.1\n")
        elif word == "cls":
            self.cells = [[(" ", 0x07)] * COLS for _ in range(ROWS)]
            self.row = 0
            self.col = 0
            self.dirty = set(range(ROWS))
        elif word == "echo":
            self.put(rest + "\n")
        elif word == "dir":
            for n in ("SYSTEM.CFG", "PHONE.GZ", "DRV\\PHONE.SYS"):
                self.put("  " + n + "\n")
        else:
            self.put("Bad command: " + word + "\n", 0x0C)


class Board:
    def __init__(self, server, password, piece):
        self.server = server
        self.password = password.encode()
        self.piece = piece
        self.con = Console()
        self.client = False
        self.authed = False
        self.nonce = b""
        self.outq = asyncio.Queue()

    # ---- board to phone ----------------------------------------------

    def room(self):
        """Bytes one notification can carry: the session's PDU less three,
        capped by --piece, since Windows does not always say."""
        try:
            pdu = self.server._subscribed_clients[0].session.max_pdu_size
            return max(20, min(self.piece, pdu - 3))
        except Exception:
            return self.piece

    def send(self, msg: bytes):
        self.outq.put_nowait(msg)

    async def sender(self):
        while True:
            msg = await self.outq.get()
            room = self.room() - 1
            at = 0
            while at < len(msg):
                take = msg[at:at + room]
                flags = (FIRST if at == 0 else 0) | (LAST if at + len(take) == len(msg) else 0)
                ch = self.server.get_characteristic(CHR_IN)
                ch.value = bytearray([flags]) + take
                self.server.update_value(SVC, CHR_IN)
                at += len(take)
                await asyncio.sleep(0.004)

    def send_info(self):
        m = bytes([ord("M")]) + (COLS * 8).to_bytes(2, "little") + \
            (ROWS * 16).to_bytes(2, "little") + bytes([COLS, ROWS, 8, 16, 0x01])
        self.send(m)

    def send_row(self, r):
        m = bytearray([ord("R"), r, COLS])
        for ch, attr in self.con.cells[r]:
            m += bytes([ord(ch) & 0xFF, attr])
        self.send(bytes(m))

    def send_cursor(self):
        ch, attr = self.con.cells[self.con.row][self.con.col] if self.con.col < COLS else (" ", 7)
        self.send(bytes([ord("C"), self.con.col, self.con.row, ord(ch) & 0xFF, attr, 1]))

    def send_everything(self):
        self.send_info()
        for r in range(ROWS):
            self.send_row(r)
        self.send_cursor()
        self.con.dirty.clear()
        self.con.scrolled = 0

    def flush(self):
        if not (self.client and (self.authed or not self.password)):
            return
        if self.con.scrolled:
            self.send(bytes([ord("S"), self.con.scrolled]))
            self.con.scrolled = 0
            self.con.dirty = set(range(ROWS))  # simple: after a scroll, every row
        for r in sorted(self.con.dirty):
            self.send_row(r)
        self.con.dirty.clear()
        self.send_cursor()

    # ---- phone to board ----------------------------------------------

    def on_write(self, value: bytes):
        if not value:
            return
        op = value[0]
        body = value[1:]
        if op == ord("H"):
            if not self.client:
                logging.info("client connected (hello)")
            self.client = True
            self.authed = False
            if self.password:
                self.nonce = os.urandom(16)
                self.send(b"A" + self.nonce)
            else:
                self.send_everything()
            return
        if not self.client:
            return
        if self.password and not self.authed:
            if op == ord("A") and len(body) == 20:
                want = hashlib.sha1(self.nonce + self.password).digest()
                if body == want:
                    self.authed = True
                    logging.info("authenticated")
                    self.send_everything()
                else:
                    logging.info("wrong password")
                    self.send(b"Qbadpass")
                    self.client = False
            return
        if op == ord("K") and len(body) >= 7:
            kind, mods = body[0], body[1]
            hid = int.from_bytes(body[2:4], "little")
            uni = int.from_bytes(body[4:6], "little")
            logging.info("key %s mods=%d hid=%d uni=%d", "down" if kind == 1 else "up", mods, hid, uni)
            if kind == 1:
                self.con.key(uni, hid)
        elif op == ord("T"):
            text = body.decode("utf-8", "replace")
            logging.info("text %r", text)
            for c in text:
                self.con.key(ord(c), 0)
        elif op == ord("P") and len(body) >= 7 and body[0] in (1, 2, 4):
            logging.info("ptr kind=%d x=%d y=%d", body[0],
                         int.from_bytes(body[2:4], "little", signed=True),
                         int.from_bytes(body[4:6], "little", signed=True))
        self.flush()


async def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--name", default="argon-sim")
    ap.add_argument("--password", default="", help="ask for this, as [phone] password does")
    ap.add_argument("--piece", type=int, default=20,
                    help="largest notification, flag byte included (default 20, the "
                         "smallest MTU - raise it if the phone negotiates more)")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")

    loop = asyncio.get_running_loop()
    server = BlessServer(name=args.name, loop=loop)
    board = Board(server, args.password, args.piece)

    def on_read(ch, **kw):
        return ch.value

    def on_write(ch, value, **kw):
        loop.call_soon_threadsafe(board.on_write, bytes(value))

    server.read_request_func = on_read
    server.write_request_func = on_write

    await server.add_new_service(SVC)
    await server.add_new_characteristic(
        SVC, CHR_IN,
        GATTCharacteristicProperties.read | GATTCharacteristicProperties.notify,
        bytearray(b"ArgonOS"), GATTAttributePermissions.readable)
    await server.add_new_characteristic(
        SVC, CHR_OUT,
        GATTCharacteristicProperties.write | GATTCharacteristicProperties.write_without_response,
        None, GATTAttributePermissions.writeable)
    await server.start()
    logging.info("advertising service fff0 as %s%s - open the ArgonOS app and search",
                 args.name, " (password set)" if args.password else "")
    await board.sender()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
