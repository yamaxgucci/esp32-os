#!/usr/bin/env python3
"""ArgonOS - a fake ESP-01 on its FACTORY AT firmware, for QEMU.

The far end of the guest's radio UART when ATRADIO.SYS is driving it.  It speaks
the stock AT command set (ATE0, AT+CWMODE, AT+CIPMUX, AT+CWJAP, AT+CIPSTART,
AT+CIPSEND, +IPD, AT+CIPCLOSE) and, being a real process on the development
machine, makes real TCP connections on the guest's behalf - so a `wget` in the
guest after `net use atradio` reaches an actual server (the netfixture) through
here.  It is to ATRADIO what rlinkd.py is to EXTRADIO: the same end-to-end path,
exercised with no radio hardware, but over the modem's own firmware instead of
ours.

Transport mirrors rlinkd/HostFS: QEMU opens the guest's radio UART as a client
socket (`-serial tcp:127.0.0.1:<port>,reconnect=1`) and this process is the TCP
server.

Run standalone:
    python tools/atmodemd.py --port 5562
"""
from __future__ import annotations

import argparse
import socket
import struct
import threading

MAX_LINK = 5          # AT+CIPMUX=1 link ids 0..4 (AG_AT_MAX_LINK)
IPD_CHUNK = 1460      # a real +IPD is capped near the TCP MSS
FAKE_STA_IP = "10.0.2.15"  # what a joined station would report


class Modem:
    """One guest connection: the AT line state machine and the sockets."""

    def __init__(self, conn):
        self.conn = conn
        self.wlock = threading.Lock()      # serialise bytes written to the guest
        self.rxbuf = b""                   # unparsed bytes from the guest
        self.links = {}                    # link -> socket
        self.free = list(range(MAX_LINK))
        self.lock = threading.Lock()       # protects links/free
        # CIPSEND: when >0, the next `send_need` bytes are payload for send_link
        self.send_link = -1
        self.send_need = 0
        self.send_acc = b""

    # --- wire out ---------------------------------------------------------
    def w(self, data: bytes):
        with self.wlock:
            try:
                self.conn.sendall(data)
            except OSError:
                pass

    def reply(self, text: str):
        """A framed AT reply: CRLF, the lines, CRLF - what the guest classifies."""
        self.w(b"\r\n" + text.encode("ascii", "replace") + b"\r\n")

    def push_ipd(self, link: int, payload: bytes):
        # "+IPD,<link>,<len>:" then the raw bytes, unsolicited.
        head = "+IPD,%d,%d:" % (link, len(payload))
        self.w(head.encode("ascii") + payload)

    # --- links ------------------------------------------------------------
    def alloc(self):
        with self.lock:
            return self.free.pop(0) if self.free else None

    def release(self, link):
        with self.lock:
            self.links.pop(link, None)
            if link not in self.free and 0 <= link < MAX_LINK:
                self.free.append(link)

    def pump(self, link, sock):
        """host socket -> +IPD frames, until the far side closes."""
        try:
            while True:
                data = sock.recv(IPD_CHUNK)
                if not data:
                    break
                self.push_ipd(link, data)
        except OSError:
            pass
        finally:
            self.reply("%d,CLOSED" % link)
            self.release(link)

    def start_pump(self, link, sock):
        threading.Thread(target=self.pump, args=(link, sock), daemon=True).start()

    # --- AT commands ------------------------------------------------------
    def handle_line(self, line: str):
        line = line.strip()
        if line == "":
            return
        if line == "AT" or line.startswith("ATE") or \
                line.startswith("AT+CWMODE") or line.startswith("AT+CIPMUX"):
            self.reply("OK")
        elif line.startswith("AT+CWJAP"):
            self.w(b"WIFI CONNECTED\r\nWIFI GOT IP\r\n")
            self.reply("OK")
        elif line.startswith("AT+CIFSR"):
            self.reply('+CIFSR:STAIP,"%s"\r\nOK' % FAKE_STA_IP)
        elif line.startswith("AT+CIPDOMAIN="):
            self.cmd_resolve(line)
        elif line.startswith("AT+CIPSTART="):
            self.cmd_start(line)
        elif line.startswith("AT+CIPSEND="):
            self.cmd_send_setup(line)
        elif line.startswith("AT+CIPCLOSE="):
            self.cmd_close(line)
        else:
            self.reply("OK")   # be lenient with setup chatter

    @staticmethod
    def _quoted(s: str):
        # first double-quoted field
        a = s.find('"')
        if a < 0:
            return None, s
        b = s.find('"', a + 1)
        if b < 0:
            return None, s
        return s[a + 1:b], s[b + 1:]

    def cmd_resolve(self, line):
        host, _ = self._quoted(line)
        if host is None:
            self.reply("ERROR")
            return
        try:
            ip = socket.getaddrinfo(host, None, socket.AF_INET,
                                    socket.SOCK_STREAM)[0][4][0]
            self.reply('+CIPDOMAIN:%s\r\nOK' % ip)
        except OSError:
            self.reply("ERROR")

    def cmd_start(self, line):
        # AT+CIPSTART=<link>,"TCP","<ip>",<port>
        try:
            body = line.split("=", 1)[1]
            link = int(body.split(",", 1)[0])
            ip, rest = self._quoted(body[body.find(",") + 1:])   # skip "TCP"
            ip, rest = self._quoted(rest)                        # the address
            port = int(rest.strip().lstrip(",").split(",")[0])
        except (ValueError, IndexError):
            self.reply("ERROR")
            return
        with self.lock:
            if link in self.links:
                self.reply("ALREADY CONNECTED")
                return
            if link in self.free:
                self.free.remove(link)
        try:
            sock = socket.create_connection((ip, port), timeout=10.0)
            sock.settimeout(None)
        except (OSError, ValueError):
            self.release(link)
            self.reply("ERROR")
            return
        with self.lock:
            self.links[link] = sock
        self.w(("%d,CONNECT\r\n" % link).encode("ascii"))
        self.reply("OK")
        self.start_pump(link, sock)

    def cmd_send_setup(self, line):
        # AT+CIPSEND=<link>,<len> -> prompt, then <len> raw bytes follow
        try:
            body = line.split("=", 1)[1]
            link_s, len_s = body.split(",", 1)
            link = int(link_s)
            need = int(len_s)
        except (ValueError, IndexError):
            self.reply("ERROR")
            return
        with self.lock:
            if link not in self.links:
                self.reply("ERROR")
                return
        self.send_link = link
        self.send_need = need
        self.send_acc = b""
        self.w(b"\r\nOK\r\n> ")   # the "> " prompt, no trailing newline

    def deliver_send(self):
        link = self.send_link
        data = self.send_acc
        self.send_link = -1
        self.send_need = 0
        self.send_acc = b""
        sock = None
        with self.lock:
            sock = self.links.get(link)
        if sock is None:
            self.reply("SEND FAIL")
            return
        try:
            sock.sendall(data)
            self.w(("Recv %d bytes\r\n" % len(data)).encode("ascii"))
            self.reply("SEND OK")
        except OSError:
            self.reply("SEND FAIL")

    def cmd_close(self, line):
        try:
            link = int(line.split("=", 1)[1].strip())
        except (ValueError, IndexError):
            self.reply("ERROR")
            return
        sock = None
        with self.lock:
            sock = self.links.get(link)
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
        self.release(link)
        self.reply("%d,CLOSED\r\nOK" % link)

    # --- receive pump: guest bytes -> commands / send payload -------------
    def feed(self, data: bytes):
        self.rxbuf += data
        while self.rxbuf:
            if self.send_need > 0:
                take = min(self.send_need, len(self.rxbuf))
                self.send_acc += self.rxbuf[:take]
                self.rxbuf = self.rxbuf[take:]
                self.send_need -= take
                if self.send_need == 0:
                    self.deliver_send()
                continue
            nl = self.rxbuf.find(b"\n")
            if nl < 0:
                break
            line = self.rxbuf[:nl].decode("ascii", "replace")
            self.rxbuf = self.rxbuf[nl + 1:]
            self.handle_line(line)

    def run(self):
        try:
            while True:
                data = self.conn.recv(4096)
                if not data:
                    return
                self.feed(data)
        except OSError:
            return
        finally:
            with self.lock:
                socks = list(self.links.values())
            for s in socks:
                try:
                    s.close()
                except OSError:
                    pass


def serve(port: int):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    print("atmodemd: fake AT modem on TCP %d" % port, flush=True)
    while True:
        conn, _ = srv.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        Modem(conn).run()   # QEMU reconnects on guest reboot; serve one at a time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5562)
    args = ap.parse_args()
    try:
        serve(args.port)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
