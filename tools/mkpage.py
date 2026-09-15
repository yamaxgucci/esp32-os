#!/usr/bin/env python3
"""Turn apps/phone/web/index.html into the file PHONE.SYS serves.

The board serves its own client, so the page has to be on the board.  It is
kept as a real .html file - editable, openable in a browser against a board
that is already running, and reviewable as HTML rather than as a wall of hex -
and this gzips it into build/apps/PHONE.GZ, which tools/mksysfs.py puts on C:.

Run by `argon apps` before PHONE.SYS is compiled (the "generate" key in
tools/apps.json), so a page edited and not regenerated cannot ship.  Also fine
to run by hand:

    python tools/mkpage.py

**A file on C:, not an array in the driver, and that is the whole point.**  A
.SYS keeps its data in RAM.  The page as a C array made the driver's data
segment fifty kilobytes, and on the CYD - 208 KB of internal RAM, no PSRAM,
about thirty free with the radio up - that is most of what the board had.
Measured there: thirty kilobytes free at idle, seven once a phone had
connected.  As a file it costs the driver one kilobyte of read buffer and
nothing else; the flash it sits in is the resource this system has plenty of.

Gzipped rather than minified.  A minifier that gets JavaScript subtly wrong
produces a page that fails in a browser nobody here has; gzip cannot change a
byte of it.  The browser decompresses, so the board spends nothing - it serves
the stored bytes with Content-Encoding: gzip and never sees the original.
"View source" on the phone still shows the real page, comments and all,
because that is what arrives.
"""
import gzip
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "apps", "phone", "web", "index.html")
DST = os.path.join(ROOT, "build", "apps", "PHONE.GZ")


def main():
    with open(SRC, "rb") as f:
        page = f.read()

    # CRLF would be served as-is and is harmless in HTML, but it makes the byte
    # count depend on how git checked the file out.
    page = page.replace(b"\r\n", b"\n")
    raw_len = len(page)

    # mtime=0 so the same page produces the same bytes: a generated file that
    # changes on every run is one that rebuilds the image for nothing.
    blob = gzip.compress(page, compresslevel=9, mtime=0)

    old = None
    if os.path.exists(DST):
        with open(DST, "rb") as f:
            old = f.read()
    if old == blob:
        return 0

    os.makedirs(os.path.dirname(DST), exist_ok=True)
    with open(DST, "wb") as f:
        f.write(blob)
    print("mkpage: %s -> %s (%u bytes of HTML, %u gzipped)" %
          (os.path.relpath(SRC, ROOT), os.path.relpath(DST, ROOT), raw_len,
           len(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
