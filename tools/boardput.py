#!/usr/bin/env python3
"""
Put a file on a running board, over the console it is already talking on.

The card comes out, goes into a laptop, comes back: that is thirty seconds of a
person's attention per attempt, and an attempt is how a driver gets debugged.
The board already has a way in - the shell's `recv <file>`, which reads hex and
needs no protocol at either end - so this drives it.

    python tools/boardput.py -p COM4 build/apps/REMDISP.SYS a:\\remdisp.sys

It is not fast and is not meant to be: hex over 115200 with the console echoing
every line is about 5 KB/s, so a driver is a second and a megabyte is four
minutes.  Use it for the small things that change often - a .SYS, a config, a
test image - and the card reader for the game.

Nothing here resets the board.  What is running keeps running, which is the
point: this is for replacing a driver and rebooting once, not for a harness.
"""
import argparse
import http.client
import os
import re
import sys
import time
import uuid

try:
    import serial
except ImportError:
    sys.exit("boardput: pyserial is missing.  Run this with the IDF python "
             "(argon env), which has it because esptool needs it.")

# The shell's line editor takes AG_LINE_MAX (256) including the newline, and it
# echoes what it is given.  96 bytes of payload is 192 characters of hex, which
# leaves room and keeps each line one comfortable write.
PER_LINE = 96


def drain(ser, seconds=0.3):
    end = time.time() + seconds
    out = bytearray()
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            out += chunk
            end = time.time() + seconds
    return bytes(out)


def over_http(host, port, name, data):
    """
    POST the file to HTTPD.AXE running on the board with /w.

    This exists because the obvious direction does not work: `wget` on the
    board fetching from a server on the laptop is an *inbound* connection to
    the laptop, and Windows Firewall refuses those by default - `no answer`,
    with nothing wrong at either end.  Opening a port needs an administrator,
    which is somebody's attention every time the machine changes.  Turning the
    transfer around costs nothing: the board listens, the laptop connects out,
    and no firewall anywhere has an opinion about that.
    """
    boundary = uuid.uuid4().hex
    head = ("--%s\r\n"
            "Content-Disposition: form-data; name=\"f\"; filename=\"%s\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n"
            % (boundary, name)).encode()
    tail = ("\r\n--%s--\r\n" % boundary).encode()

    conn = http.client.HTTPConnection(host, port, timeout=120)
    t0 = time.time()
    conn.request("POST", "/", body=head + data + tail, headers={
        "Content-Type": "multipart/form-data; boundary=" + boundary,
        "Content-Length": str(len(head) + len(data) + len(tail)),
    })
    resp = conn.getresponse()
    resp.read()
    conn.close()
    el = time.time() - t0
    print("boardput: %d -> %s in %.1f s (%.0f KB/s)"
          % (resp.status, name, el, len(data) / el / 1024.0 if el else 0))
    return 0 if resp.status in (200, 201, 303) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", help="serial port, for the hex path")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--http", metavar="HOST",
                    help="the board's address, with `httpd <port> <dir> /w` "
                         "running on it.  Two hundred times faster than the "
                         "console and the only way that scales to a megabyte")
    ap.add_argument("--http-port", type=int, default=80)
    ap.add_argument("src", help="file on this machine")
    ap.add_argument("dst", nargs="?",
                    help="path on the board, e.g. a:\\remdisp.sys "
                         "(serial); over http the name only, and the "
                         "directory is httpd's own")
    args = ap.parse_args()

    with open(args.src, "rb") as f:
        data = f.read()

    if args.http:
        name = os.path.basename(args.dst or args.src)
        print("boardput: %s -> %s:%d as %s, %d bytes"
              % (args.src, args.http, args.http_port, name, len(data)))
        sys.exit(over_http(args.http, args.http_port, name, data))

    if not args.port or not args.dst:
        sys.exit("boardput: need -p PORT and a destination, or --http HOST")
    print("boardput: %s -> %s, %d bytes" % (args.src, args.dst, len(data)))

    #
    # And try again if it came up short.
    #
    # A byte goes missing every few thousand on this path - once in 6208 on
    # the CYD, twice in 24336 on the S3 - and no amount of pacing has made it
    # never happen, because the console was not built to be a file transport
    # and its input buffer is small.  What matters is that a short transfer is
    # now *known* to be short: recv rewrites the file from the start, so a
    # second attempt costs twenty seconds and leaves either a whole file or an
    # error.  Silently almost-right is the one outcome not on offer.
    #
    for attempt in range(3):
        if attempt:
            print("boardput: attempt %d" % (attempt + 1))
        if over_serial(args.port, args.baud, args.dst, data):
            return
    sys.exit("boardput: three attempts, none of them whole")


def over_serial(port, baud, dst, data):
    # No DTR/RTS handling at all: touching either resets the chip through the
    # auto-reset circuit, and the whole point is to leave the board running.
    ser = serial.Serial(port, baud, timeout=0.05, xonxoff=True)
    try:
        #
        # END first, in case the last run of this was interrupted.  A `recv`
        # that never finished is still sitting there reading lines as hex, and
        # the next attempt's "recv a:\..." goes into the file as data and comes
        # back as `not hex: 'r'` - which reads like a bug in this tool and is
        # not one.  At a prompt END is merely an unknown command.
        #
        ser.write(b"END\r\n")
        drain(ser, 0.4)
        ser.write(b"\r\n")
        drain(ser, 0.4)

        ser.write(("recv %s\r\n" % dst).encode())
        reply = drain(ser, 1.0)
        if b"send hex" not in reply:
            sys.exit("boardput: the board did not start recv:\n%s"
                     % reply.decode("latin-1")[-400:])

        # A short read timeout, because it is also the quiet gap that paces the
        # loop: too long and every line pays it, too short and the echo's own
        # stutter looks like the end of it.
        ser.timeout = 0.01
        sent = 0
        t0 = time.time()
        for i in range(0, len(data), PER_LINE):
            ser.write((data[i:i + PER_LINE].hex() + "\r\n").encode())
            #
            # Wait for the echo of that line before sending the next, and let
            # the echo be the only clock.
            #
            # Two wrong versions came before this one and each was wrong in an
            # interesting way.  The first slept twenty milliseconds after every
            # line: correct, and a quarter of an hour for a megabyte, with the
            # processor idle throughout - the sleep restarted whenever a byte
            # arrived, which for an echoing console is always.  The second
            # dropped the wait entirely, on the theory that XON/XOFF would hold
            # the sender back.  It does not: 24336 bytes went out and 8997
            # arrived, ending in `odd number of hex digits`, because the board
            # simply lost what it had no room for and said nothing.
            #
            # A silent partial write is the worst of the three outcomes - an
            # image that is half itself will load and then fail somewhere with
            # no hint of why - so pacing here is correctness and not politeness.
            # The console echoes what it has taken, so the echo is the board
            # saying "next": exactly as fast as it can go and no faster.
            #
            # Not "wait for a newline": this console is a screen, not a
            # teletype, and what comes back is cursor motion - `ESC [ 25 ; 1 H`
            # where a terminal would have put a line feed.  A version that
            # waited for '\n' waited for ever with the echo arriving the whole
            # time.  So the criterion is the echo going quiet, which needs no
            # agreement about what the bytes mean.
            # Quiet alone is not enough, and the CYD proved it: its console
            # stutters mid-line often enough that a gap in the echo was read as
            # the end of it, and 6208 bytes arrived as 6143.  So the echo has to
            # be at least as long as what was typed - a console echoes what it
            # takes, character for character, plus whatever cursor motion it
            # needs - and only then does quiet mean finished.
            want = len(data[i:i + PER_LINE]) * 2 + 2
            got = 0
            deadline = time.time() + 5.0
            while time.time() < deadline:
                chunk = ser.read(4096)
                if chunk:
                    got += len(chunk)
                elif got >= want:
                    break
            if got == 0:
                sys.exit("boardput: no echo after %d bytes - the board stopped "
                         "reading" % sent)
            sent += min(PER_LINE, len(data) - i)
            if (i // PER_LINE) % 512 == 0:
                el = time.time() - t0
                rate = sent / el / 1024.0 if el > 0 else 0
                print("  %d/%d bytes, %.1f KB/s" % (sent, len(data), rate),
                      flush=True)

        ser.write(b"END\r\n")
        tail = drain(ser, 2.0).decode("latin-1")

        #
        # Check the count the board reports, and fail on a mismatch.
        #
        # The failure mode of this whole path is a file that is *almost* right:
        # an .AXE short by sixty bytes loads, runs, and dies somewhere with no
        # hint that the image was not the one that was built.  The board already
        # says how many bytes it wrote; not looking at it was how two corrupted
        # transfers got as far as being debugged as driver bugs.
        #
        m = re.search(r"(\d+) bytes written", re.sub(r"\x1b\[[0-9;]*[A-Za-z]",
                                                     "", tail))
        if m is None:
            sys.exit("boardput: the board never said how much it wrote:\n%s"
                     % tail[-300:])
        wrote = int(m.group(1))
        if wrote != len(data):
            print("boardput: %d bytes of %d arrived" % (wrote, len(data)))
            return False
        print("boardput: %d bytes written" % wrote)
        return True
    finally:
        ser.close()


if __name__ == "__main__":
    main()
