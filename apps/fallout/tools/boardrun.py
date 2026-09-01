#!/usr/bin/env python3
"""
Run the game on the board, on a chosen map, and bring its log back.

`argon test` measures the emulator and nothing measures the board, so every
hardware number so far has been a person watching a screen and reporting an
impression - "about five frames a second", "the sound was unclear".  Those are
real observations and they found real faults, but they cannot be compared with
last week's, and they cost somebody's evening each.

This is the same shape of harness the emulator has, for the board:

    python apps/fallout/tools/boardrun.py --port COM4 --host 192.168.0.111 \
        --ssid MYNET --pass SECRET --map arvillag.map --seconds 120

What it does, and why it is this many steps.  The board has one serial port and
the game owns the console while it runs, so nothing can be asked of the board
during a run; and the log lives on the card, which only the board can read.  So
each run is three phases with a reset between them, and the reset is what makes
one run independent of the last:

  1. Wi-Fi up, HTTPD.AXE serving the card, so this machine can put files there
     and take them back.  The board is the server and this is the client on
     purpose: the other direction is an inbound connection to a laptop, and a
     firewall refuses it silently.
  2. ddraw.ini written with the map to start on, the old log deleted.
  3. Reset, radio off, the game with -autostart and -log; wait; reset.
  4. Wi-Fi up again, fetch the log.

The Wi-Fi password is passed in and never written down here: flashing C: takes
the board's stored copy with it, so a run after a reflash needs it again.

Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import http.client
import os
import re
import subprocess
import sys
import time
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))))
BOARDTEST = os.path.join(ROOT, "tools", "boardtest.py")

DDRAW = """; Written by boardrun.py.  Edits here are overwritten every run.
[Misc]
SkipOpeningMovies=%d
StartingMap=%s
"""


def shell(port, cmds, timeout=120, reset=True):
    """Send commands to the board and return the raw transcript."""
    argv = [sys.executable, BOARDTEST, "-p", port, "--timeout", str(timeout)]
    if not reset:
        argv.append("--no-reset")
    argv += cmds
    subprocess.run(argv, capture_output=True, timeout=timeout + 60)
    log = os.path.join(ROOT, "build", "board.log")
    with open(log, "rb") as f:
        return f.read().decode("latin-1")


def strip_ansi(s):
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", s).replace("\r", "")


def wifi_up(port, ssid, password, host, tries=3):
    """Bring the radio up and wait for the board to answer HTTP."""
    for attempt in range(tries):
        cmds = ["wifi on"]
        if ssid:
            cmds.append("wifi connect %s %s" % (ssid, password))
        shell(port, cmds, timeout=90)
        time.sleep(9)
        shell(port, ["run /b a:\\httpd.axe 80 a: /w"], timeout=60, reset=False)
        time.sleep(3)
        if http_alive(host):
            return True
        print("  radio attempt %d did not answer" % (attempt + 1), flush=True)
    return False


def http_alive(host):
    try:
        c = http.client.HTTPConnection(host, 80, timeout=8)
        c.request("GET", "/")
        r = c.getresponse()
        r.read()
        c.close()
        return r.status == 200
    except OSError:
        return False


def http_put(host, name, data):
    boundary = uuid.uuid4().hex
    head = ("--%s\r\nContent-Disposition: form-data; name=\"f\"; "
            "filename=\"%s\"\r\nContent-Type: application/octet-stream\r\n\r\n"
            % (boundary, name)).encode()
    tail = ("\r\n--%s--\r\n" % boundary).encode()
    c = http.client.HTTPConnection(host, 80, timeout=180)
    c.request("POST", "/", body=head + data + tail, headers={
        "Content-Type": "multipart/form-data; boundary=" + boundary,
        "Content-Length": str(len(head) + len(data) + len(tail))})
    r = c.getresponse()
    r.read()
    c.close()
    return r.status


def http_delete(host, name):
    body = "delete=1".encode()
    c = http.client.HTTPConnection(host, 80, timeout=60)
    c.request("POST", "/" + name, body=body, headers={
        "Content-Type": "application/x-www-form-urlencoded",
        "Content-Length": str(len(body))})
    r = c.getresponse()
    r.read()
    c.close()
    return r.status


def http_get(host, name):
    c = http.client.HTTPConnection(host, 80, timeout=180)
    c.request("GET", "/" + name)
    r = c.getresponse()
    body = r.read()
    c.close()
    return r.status, body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--host", required=True, help="the board's IP")
    ap.add_argument("--ssid", default="")
    ap.add_argument("--password", default="")
    ap.add_argument("--map", default="", help="StartingMap for -autostart; "
                                              "empty means the game's own")
    ap.add_argument("--movies", action="store_true",
                    help="let the opening movies play")
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--args", default="-memstat 10000 -fps 10000",
                    help="extra arguments for the game")
    ap.add_argument("--no-autostart", action="store_true")
    ap.add_argument("--out", default="", help="where to write the log")
    args = ap.parse_args()

    out = args.out or os.path.join(ROOT, "build", "boardrun.log")

    print("boardrun: %s, %d s, %s" % (args.map or "(menu)", args.seconds,
                                      args.args), flush=True)

    print("1/4 radio and httpd", flush=True)
    if not http_alive(args.host):
        if not wifi_up(args.port, args.ssid, args.password, args.host):
            sys.exit("boardrun: the board never answered on %s" % args.host)

    print("2/4 ddraw.ini and a clean log", flush=True)
    ini = (DDRAW % (0 if args.movies else 1, args.map)).encode()
    if http_put(args.host, "ddraw.ini", ini) not in (200, 201, 303):
        sys.exit("boardrun: could not write ddraw.ini")
    http_delete(args.host, "fo.log")

    print("3/4 the run", flush=True)
    cmd = "run a:\\fallout2.axe -log a:\\fo.log %s" % args.args
    if not args.no_autostart:
        cmd = "run a:\\fallout2.axe -autostart -log a:\\fo.log %s" % args.args
    shell(args.port, ["wifi off", cmd], timeout=args.seconds + 60)
    # boardtest returns as soon as it sees a prompt, and `run` gives one back
    # while the application keeps going in its slot.  So the wait is here.
    time.sleep(args.seconds)

    print("4/4 fetching the log", flush=True)
    if not wifi_up(args.port, args.ssid, args.password, args.host):
        sys.exit("boardrun: the board never came back on the network")
    status, body = http_get(args.host, "fo.log")
    if status != 200:
        sys.exit("boardrun: fo.log came back %d" % status)
    with open(out, "wb") as f:
        f.write(body)
    print("boardrun: %d bytes -> %s" % (len(body), out))


if __name__ == "__main__":
    main()
