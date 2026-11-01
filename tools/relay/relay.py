#!/usr/bin/env python3
# Argon KVM relay - multi-board.
#
# A VPS with a public IP that boards dial OUT to, so a board behind home NAT is
# reachable from anywhere with no inbound port at home.  The relay pairs an
# outbound board connection with an inbound phone connection and pipes bytes;
# the whole KVM protocol (HTTP page + WebSocket) runs end-to-end board<->phone.
#
# MANY boards share one relay.  Each board announces an id (a free-form label the
# owner picks at setup, offline).  A phone reaches a board with ?id=<id> (kept in
# a cookie thereafter).  ids are NOT unique and NOT persisted: a board owns its id
# only while it has a live connection here; when it drops, the id is free again.
# Collision: while board A (identified by its MAC) holds id X, another board (a
# different MAC) dialing in as X is refused ("id busy") - A keeps it, B's agent
# tells its user to pick another.  id is only a routing label: ACCESS IS THE
# PASSWORD, checked per-board.  So reaching someone else's board by an id clash
# still cannot control or take it over without that board's own password.
#
# Takeover: a second viewer of a busy board gets a page with a password field.
# The board sends the relay a SHA-256 of its web password on connect; the relay
# checks the takeover against THAT board's hash, then reserves the board for the
# taker (cookie token) so the dropped viewer's auto-reconnect cannot grab it back.
#
# Transition: the board port accepts both the legacy hello ("AGKVM1 <secret>")
# and the new one ("AGKVM1 <secret> <mac> <id> <pwhash>").  A TLS board port is
# added later (new boards dial over TLS); the plaintext port stays until then.
#
# Env:
#   RELAY_SECRET     secret a board must present to register      (required)
#   WEB_PASS_SHA256  fallback web-password hash for legacy boards (optional)
#   PUBLIC_HOST/PORT where phones reach us (behind Caddy: 127.0.0.1:8080)
#   BOARD_PORT       plaintext board dial-in port                 (default 7000)

import asyncio
import collections
import hashlib
import hmac
import os
import secrets
import socket
import struct
import sys
import time
import urllib.parse

SECRET = os.environ.get("RELAY_SECRET", "").encode()
LEGACY_PWHASH = os.environ.get("WEB_PASS_SHA256", "").strip().lower()
PUBLIC_HOST = os.environ.get("PUBLIC_HOST", "127.0.0.1")
PUBLIC_PORT = int(os.environ.get("PUBLIC_PORT", "8080"))
BOARD_PORT = int(os.environ.get("BOARD_PORT", "7000"))         # plaintext (legacy)
BOARD_TLS_PORT = int(os.environ.get("BOARD_TLS_PORT", "7443"))  # TLS (new boards)
BOARD_TLS_CERT = os.environ.get("BOARD_TLS_CERT", "/etc/argon-relay/board.crt")
BOARD_TLS_KEY = os.environ.get("BOARD_TLS_KEY", "/etc/argon-relay/board.key")

HELLO = b"AGKVM1"
AUTH_TIMEOUT = 5.0
PARK_WAIT = 6.0
BUSY_WAIT = 1.0
HEAD_MAX = 16384
HEAD_TIMEOUT = 4.0
RES_WINDOW = 30.0
COOKIE = "agkvm_take"
BOARD_COOKIE = "agkvm_board"
REJECT_BUSY = b"AGKVM1-IDBUSY\n"   # board sees this -> tell its user: id taken


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def derive_id(secret_bytes):
    """A stable short label for a board that didn't announce one (legacy)."""
    return hashlib.sha256(b"id:" + secret_bytes).hexdigest()[:10]


class Board:
    __slots__ = ("id", "owner", "pwhash", "parked", "sessions",
                 "res_token", "res_until")

    def __init__(self, bid, owner, pwhash):
        self.id = bid
        self.owner = owner            # MAC (new) or "secret:<...>" (legacy)
        self.pwhash = pwhash          # sha256 hex of web password, or ""
        self.parked = collections.deque()
        self.sessions = set()
        self.res_token = None
        self.res_until = 0.0

    def live_parked(self):
        while self.parked:
            r, w = self.parked[0]
            if w.is_closing() or r.at_eof():
                self.parked.popleft()
                try:
                    w.close()
                except Exception:
                    pass
                continue
            return True
        return False

    def alive(self):
        return self.live_parked() or bool(self.sessions)

    def reserved(self):
        return self.res_token is not None and time.time() < self.res_until

    def held(self):
        # "owned" for collision/lifecycle: a live connection OR an active
        # reservation (which must survive the kick->redial gap of a takeover).
        return self.alive() or self.reserved()


boards = {}            # id -> Board
waiters = {}           # id -> deque of futures waiting for that board


def prune(board):
    if board is not None and not board.held() and boards.get(board.id) is board:
        del boards[board.id]
        waiters.pop(board.id, None)


def http(status, body, extra=""):
    body_b = body.encode("utf-8")
    return (("HTTP/1.1 %s\r\n"
             "Content-Type: text/html; charset=utf-8\r\n"
             "Content-Length: %d\r\nCache-Control: no-store\r\n"
             "Connection: close\r\n%s\r\n" %
             (status, len(body_b), extra)).encode("ascii") + body_b)


def page(title, heading, text, tail=""):
    return """<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>%s</title>
<style>*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;
align-items:center;justify-content:center;background:#111;color:#ddd;
font:16px system-ui,sans-serif;padding:24px}.card{max-width:440px;text-align:center}
h2{margin:0 0 .4em}p{color:#aaa;line-height:1.5}
input{width:100%%;margin-top:16px;padding:12px;border-radius:8px;border:1px solid #333;
background:#1a1a1a;color:#eee;font-size:16px}
button{width:100%%;margin-top:12px;padding:13px 22px;border:0;border-radius:8px;
background:#2d6cdf;color:#fff;font-weight:600;font-size:16px}
.err{margin-top:14px;color:#e66;font-size:14px}.hint{margin-top:14px;font-size:13px;color:#777}
</style><div class=card><h2>%s</h2><p>%s</p>%s</div>""" % (title, heading, text, tail)


def offline_page(bid):
    who = (" (id: %s)" % bid) if bid else ""
    return http("503 Service Unavailable",
                page("KVM offline", "KVM offline",
                     "Плата%s не на связи. Проверьте, что донгл включён и в сети."
                     % who))


def choose_page():
    return http("404 Not Found",
                page("KVM", "Укажите id платы",
                     "Откройте ссылку вида "
                     "<code>?id=ВАШ_ID</code> — id берётся из вашей программы KVM."))


def busy_page(bid, err=False):
    note = ('<div class=err>Неверный пароль. Возможно, ваш id совпал с чужим — '
            'проверьте сообщение вашей программы KVM.</div>') if err else ""
    form = ('<form method=POST action="/__takeover">'
            '<input type=password name=p autocomplete=current-password '
            'placeholder="Пароль" autofocus>'
            '<button type=submit>Управлять отсюда</button></form>'
            + note +
            '<div class=hint>Текущий сеанс будет отключён.</div>')
    return http("200 OK",
                page("Управление занято", "Управление занято",
                     "Этот KVM сейчас под управлением из другого браузера или "
                     "страницы. Чтобы перехватить, введите пароль страницы.",
                     form))


def no_takeover_page():
    return http("200 OK",
                page("Управление занято", "Управление занято",
                     "Этот KVM занят, а перехват по паролю для него недоступен. "
                     "Подождите, пока освободится."))


def takeover_ok_page(token):
    extra = ("Set-Cookie: %s=%s; Path=/; Max-Age=60; SameSite=Lax\r\n"
             % (COOKIE, token))
    body = page("Перехват управления", "Перехватываю управление…",
                "Секунду — страница откроется сама.",
                '<meta http-equiv="refresh" content="2;url=/">'
                '<p><a style="color:#2d6cdf" href="/">Открыть сейчас</a></p>')
    return http("200 OK", body, extra)


def redirect_root(set_board=None):
    extra = "Location: /\r\n"
    if set_board is not None:
        extra += ("Set-Cookie: %s=%s; Path=/; Max-Age=86400; SameSite=Lax\r\n"
                  % (BOARD_COOKIE, set_board))
    return http("302 Found",
                page("…", "…", "Открываю…",
                     '<p><a style="color:#2d6cdf" href="/">Открыть</a></p>'),
                extra)


# ---- board side ---------------------------------------------------------

async def board_conn(reader, writer):
    peer = writer.get_extra_info("peername")
    try:
        line = await asyncio.wait_for(reader.readline(), timeout=AUTH_TIMEOUT)
    except asyncio.TimeoutError:
        writer.close()
        return
    parts = line.split()
    if len(parts) < 2 or parts[0] != HELLO or parts[1] != SECRET:
        log("board auth FAIL from", peer)
        writer.close()
        return

    # New hello:  AGKVM1 <secret> <id> <pwhash>   (id "-" = let us derive one)
    # Legacy:     AGKVM1 <secret>                 (id derived, env pwhash)
    # The owner (for collision: same board re-dialing vs a different board on the
    # same id) is the SECRET, not the id - different people use different secrets,
    # so two boards that happen to pick the same id still have different owners.
    # Newer boards add a 5th field: a token of their own, made once and kept on
    # their C: - THE owner, since every board on this relay shares the secret.
    if len(parts) >= 5:
        owner = "node:" + parts[4].decode("latin1")
    else:
        owner = "secret:" + parts[1].decode("latin1")
    if len(parts) >= 4:
        bid = parts[2].decode("latin1")
        pwhash = parts[3].decode("latin1").lower()
        if bid in ("", "-"):
            bid = derive_id(parts[1])
    else:
        bid = derive_id(parts[1])
        pwhash = LEGACY_PWHASH

    b = boards.get(bid)
    if b is not None and b.held() and b.owner != owner:
        log("board id COLLISION '%s' from %s (owner %s holds it)" %
            (bid, peer, b.owner))
        try:
            writer.write(REJECT_BUSY)
            await writer.drain()
        except Exception:
            pass
        writer.close()
        return

    if b is None or not b.held():
        b = Board(bid, owner, pwhash)
        boards[bid] = b
    else:
        b.owner = owner
        if pwhash:
            b.pwhash = pwhash

    sock = writer.get_extra_info("socket")
    if sock is not None:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)

    wq = waiters.get(bid)
    while wq:
        fut = wq.popleft()
        if not fut.done():
            fut.set_result((reader, writer))
            log("board '%s' from %s -> waiting phone" % (bid, peer))
            return
    b.parked.append((reader, writer))
    log("board '%s' parked from %s (pool=%d)" % (bid, peer, len(b.parked)))


async def take_board(board, timeout):
    if board.live_parked():
        return board.parked.popleft()
    loop = asyncio.get_event_loop()
    fut = loop.create_future()
    waiters.setdefault(board.id, collections.deque()).append(fut)
    try:
        return await asyncio.wait_for(fut, timeout=timeout)
    except asyncio.TimeoutError:
        wq = waiters.get(board.id)
        if wq and fut in wq:
            try:
                wq.remove(fut)
            except ValueError:
                pass
        return None


def abort_rst(w):
    """Drop a connection with a TCP reset, not a polite close.  A polite TLS close
    sends close_notify and waits up to half a minute for the answer - and the
    board, busy pushing a picture into a socket nobody reads any more, does not
    answer: its write blocks, it never sees the close, and what it had queued
    sits in lwip's memory until new connections cannot even finish a handshake.
    A reset makes the board's write fail at once, so it drops the phone, throws
    the queue away, and dials back within seconds."""
    try:
        sock = w.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                            struct.pack("ii", 1, 0))
    except Exception:
        pass
    try:
        w.transport.abort()
    except Exception:
        try:
            w.close()
        except Exception:
            pass


def kick(board):
    n = 0
    for pw, bw in list(board.sessions):
        abort_rst(bw)          # the board: reset, so it lets go immediately
        try:
            pw.close()         # the old viewer: a normal close is fine
        except Exception:
            pass
        n += 1
    return n


# ---- phone side ---------------------------------------------------------

async def read_request(reader):
    buf = b""
    try:
        while b"\r\n\r\n" not in buf and len(buf) < HEAD_MAX:
            chunk = await asyncio.wait_for(reader.read(4096), timeout=HEAD_TIMEOUT)
            if not chunk:
                break
            buf += chunk
    except asyncio.TimeoutError:
        pass
    method = path = b""
    cookies = {}
    clen = 0
    try:
        head, _, rest = buf.partition(b"\r\n\r\n")
        lines = head.split(b"\r\n")
        p = lines[0].split(b" ")
        if len(p) >= 2:
            method, path = p[0], p[1]
        for ln in lines[1:]:
            k, _, v = ln.partition(b":")
            kl = k.strip().lower()
            v = v.strip()
            if kl == b"content-length":
                try:
                    clen = int(v)
                except ValueError:
                    clen = 0
            elif kl == b"cookie":
                for item in v.split(b";"):
                    ck, _, cv = item.strip().partition(b"=")
                    if ck:
                        cookies[ck.decode("latin1")] = cv.decode("latin1")
    except Exception:
        rest = b""
    body = rest
    try:
        while len(body) < clen and len(buf) < HEAD_MAX:
            chunk = await asyncio.wait_for(reader.read(4096), timeout=HEAD_TIMEOUT)
            if not chunk:
                break
            body += chunk
            buf += chunk
    except asyncio.TimeoutError:
        pass
    body_text = body[:clen].decode("utf-8", "replace") if clen else ""
    return buf, method, path, cookies, body_text


def q_id(path):
    try:
        qs = path.split(b"?", 1)[1].decode("latin1") if b"?" in path else ""
        v = urllib.parse.parse_qs(qs).get("id")
        return v[0] if v else None
    except Exception:
        return None


def pw_ok(board, body_text):
    if not board.pwhash:
        return False
    pw = (urllib.parse.parse_qs(body_text).get("p") or [""])[0]
    if pw == "":
        return False
    h = hashlib.sha256(pw.encode("utf-8")).hexdigest()
    return hmac.compare_digest(h, board.pwhash)


async def pipe(src, dst, rst=False):
    try:
        while True:
            data = await src.read(65536)
            if not data:
                break
            dst.write(data)
            await dst.drain()
    except Exception:
        pass
    finally:
        if rst:
            abort_rst(dst)     # dst is the board: let go of it at once
        else:
            try:
                dst.close()
            except Exception:
                pass


async def send_close(writer, payload):
    try:
        writer.write(payload)
        await writer.drain()
    except Exception:
        pass
    try:
        writer.close()
    except Exception:
        pass


def only_board():
    live = [b for b in boards.values() if b.alive()]
    return live[0] if len(live) == 1 else None


async def phone_conn(preader, pwriter):
    peer = pwriter.get_extra_info("peername")
    raw, method, path, cookies, body_text = await read_request(preader)
    bare = path.split(b"?", 1)[0]

    # ?id=... -> remember in a cookie and bounce to /
    qid = q_id(path)
    if qid is not None:
        await send_close(pwriter, redirect_root(set_board=qid))
        return

    # resolve which board this phone wants
    bid = cookies.get(BOARD_COOKIE)
    board = boards.get(bid) if bid else None
    if (board is None or not board.alive()):
        single = only_board()
        if single is not None and not bid:
            board = single
            bid = single.id

    if bare == b"/__takeover":
        if board is None or not board.alive():
            await send_close(pwriter, offline_page(bid))
            return
        if method == b"POST" and pw_ok(board, body_text):
            token = secrets.token_hex(16)
            board.res_token = token
            board.res_until = time.time() + RES_WINDOW
            n = kick(board)
            log("phone %s -> takeover OK on '%s', dropped %d" % (peer, bid, n))
            await send_close(pwriter, takeover_ok_page(token))
        else:
            if method == b"POST":
                log("phone %s -> takeover DENIED on '%s'" % (peer, bid))
            await send_close(pwriter, busy_page(bid, err=(method == b"POST")))
        return

    if board is None or not board.alive():
        if bid:
            await send_close(pwriter, offline_page(bid))
        else:
            await send_close(pwriter, choose_page())
        return

    has_token = (board.reserved() and cookies.get(COOKIE) == board.res_token)
    if board.reserved() and not has_token:
        log("phone %s -> '%s' reserved for taker, busy" % (peer, bid))
        await send_close(pwriter, busy_page(bid))
        return

    busy = bool(board.sessions) and not has_token
    rw = await take_board(board, BUSY_WAIT if busy else PARK_WAIT)
    if rw is None:
        if board.sessions:
            if board.pwhash:
                await send_close(pwriter, busy_page(bid))
            else:
                await send_close(pwriter, no_takeover_page())
        else:
            await send_close(pwriter, offline_page(bid))
        prune(board)
        return

    if has_token:
        board.res_until = time.time() + RES_WINDOW

    breader, bwriter = rw
    try:
        if has_token:
            # This session already proved the password at the relay (takeover);
            # tell the board so it does not ask a second time.
            bwriter.write(b"AGKVM-AUTH\r\n")
        if raw:
            bwriter.write(raw)
        await bwriter.drain()
    except Exception:
        try:
            bwriter.close()
        except Exception:
            pass
        pwriter.close()
        prune(board)
        return

    pair = (pwriter, bwriter)
    board.sessions.add(pair)
    log("phone %s paired on '%s' (sessions=%d)" % (peer, bid, len(board.sessions)))
    try:
        await asyncio.gather(pipe(preader, bwriter, rst=True),
                             pipe(breader, pwriter))
    finally:
        board.sessions.discard(pair)
        prune(board)
    log("phone %s session ended on '%s'" % (peer, bid))


ADMIN_PORT = int(os.environ.get("ADMIN_PORT", "7999"))   # 127.0.0.1 only


async def admin_conn(reader, writer):
    """Local admin (argon-push): PUSH <id> <app|os> <size> <sha256>\\n + bytes.
    Takes one of that board's parked connections, sends it the update, and hands
    back the board's one-line answer.  Bound to 127.0.0.1: only someone already on
    this server can use it, and a board only accepts it if its owner allowed."""
    try:
        line = (await asyncio.wait_for(reader.readline(), 10)).decode().split()
        if len(line) != 5 or line[0] != "PUSH":
            writer.write(b"ERR usage: PUSH <id> <app|os> <size> <sha256>\n")
            return
        _, bid, kind, size_s, sha = line
        size = int(size_s)
        board = boards.get(bid)
        if board is None or not board.alive():
            writer.write(("ERR board '%s' is not online\n" % bid).encode())
            return
        # Maintenance wins over a viewer: reserve the board (phones get the busy
        # page, so an open tab cannot grab it back), drop whoever is on it, and
        # take the first connection it dials in with.
        board.res_token = "admin-" + secrets.token_hex(8)
        board.res_until = time.time() + 90
        if board.sessions:
            n = kick(board)
            writer.write(("NOTE dropped %d viewer session(s)\n" % n).encode())
            await writer.drain()
        rw = await take_board(board, 60)
        if rw is None:
            writer.write(b"ERR no free connection to the board within 60s\n")
            return
        breader, bwriter = rw
        log("admin: pushing %s (%d bytes) to '%s'" % (kind, size, bid))
        bwriter.write(("AGKVM-UPD %s %d %s\r\n\r\n" % (kind, size, sha)).encode())
        left = size
        while left > 0:
            chunk = await reader.read(min(65536, left))
            if not chunk:
                break
            bwriter.write(chunk)
            await bwriter.drain()
            left -= len(chunk)
        answer = await asyncio.wait_for(breader.readline(), 180)
        log("admin: board '%s' answered %r" % (bid, answer.strip()))
        writer.write(answer or b"ERR board closed without answering\n")
        abort_rst(bwriter)
        # The board is about to restart: let it back in under whatever it now
        # presents (an update may change how it identifies itself).
        board.res_token = None
        board.res_until = 0.0
        prune(board)
    except Exception as e:
        try:
            writer.write(("ERR %r\n" % e).encode())
        except Exception:
            pass
    finally:
        try:
            await writer.drain()
            writer.close()
        except Exception:
            pass


async def main():
    if not SECRET:
        print("RELAY_SECRET not set", file=sys.stderr)
        sys.exit(1)
    bsrv = await asyncio.start_server(board_conn, "0.0.0.0", BOARD_PORT)
    psrv = await asyncio.start_server(phone_conn, PUBLIC_HOST, PUBLIC_PORT)
    asrv = await asyncio.start_server(admin_conn, "127.0.0.1", ADMIN_PORT)
    servers = [bsrv, psrv, asrv]
    log("relay up: phone %s:%d  board :%d" % (PUBLIC_HOST, PUBLIC_PORT, BOARD_PORT))

    # TLS board port: new boards dial this over TLS (the hello, with the password
    # hash, is then encrypted).  Needs the Let's Encrypt cert for the board's
    # host name - the board verifies it against the root bundle.  Off if absent.
    if os.path.exists(BOARD_TLS_CERT) and os.path.exists(BOARD_TLS_KEY):
        import ssl
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(BOARD_TLS_CERT, BOARD_TLS_KEY)
        tsrv = await asyncio.start_server(board_conn, "0.0.0.0",
                                          BOARD_TLS_PORT, ssl=ctx)
        servers.append(tsrv)
        log("board TLS on :%d" % BOARD_TLS_PORT)
    else:
        log("board TLS cert missing (%s) - TLS board port OFF" % BOARD_TLS_CERT)

    async with bsrv, psrv:
        await asyncio.gather(*(s.serve_forever() for s in servers))


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
