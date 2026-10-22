#!/usr/bin/env python3
"""Type the commands that start a game into an ArgonOS console over TCP.

`argon run -tcp` puts the console on 127.0.0.1:5556 and holds the machine
until something connects.  Somebody has to be that something, and it also has
to stay connected: a serial port with no peer discards what the guest prints,
so the log of a session driven this way would otherwise be empty.

Everything it types is a line at the shell prompt.  It waits for the prompt
between lines rather than sleeping, because a `drv install` that has not
finished will swallow the next line.
"""

from __future__ import annotations

import socket
import sys
import time

PROMPT = b"A:\>"


def wait_prompt(sock, log, seen_before, timeout=90.0):
    """Read until the prompt has appeared once more than it had."""
    deadline = time.monotonic() + timeout
    buf = b""
    while time.monotonic() < deadline:
        sock.settimeout(1.0)
        try:
            b = sock.recv(65536)
        except socket.timeout:
            continue
        if not b:
            return buf, seen_before
        log.write(b)
        log.flush()
        buf += b
        if buf.count(PROMPT) > seen_before:
            return buf, buf.count(PROMPT)
    return buf, seen_before


def main() -> int:
    host, port = "127.0.0.1", 5556
    logpath = sys.argv[1]
    commands = sys.argv[2:]

    for _ in range(60):
        try:
            sock = socket.create_connection((host, port), timeout=5)
            break
        except OSError:
            time.sleep(1)
    else:
        print("no console on %s:%d" % (host, port))
        return 1

    with open(logpath, "wb") as log:
        seen = 0
        _, seen = wait_prompt(sock, log, seen, timeout=180)
        for cmd in commands:
            print("-> %s" % cmd, flush=True)
            sock.sendall(cmd.encode("ascii") + b"\r")
            _, seen = wait_prompt(sock, log, seen, timeout=180)
        # Stay attached so the guest's output keeps a reader.
        while True:
            sock.settimeout(5.0)
            try:
                b = sock.recv(65536)
            except socket.timeout:
                continue
            if not b:
                return 0
            log.write(b)
            log.flush()


if __name__ == "__main__":
    raise SystemExit(main())
