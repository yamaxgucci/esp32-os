#!/usr/bin/env python3
"""
The link counters out of a wireblk log: the `wireblk link <wire>:` lines,
joined across the console's wrapping and without the terminal's escapes.

    python tools/linkline.py logs/run.log [field ...]

With field names (frames, naks, to, ...) only those are printed.
"""
import re
import sys


def lines(path):
    raw = open(path, "rb").read().decode("latin-1")
    raw = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "\n", raw).replace("\r", "")
    rows = raw.split("\n")
    out = []
    for i, r in enumerate(rows):
        if "wireblk link " in r:
            out.append(r + "".join(rows[i + 1:i + 3]))
    return list(dict.fromkeys(out))


def main():
    path = sys.argv[1]
    want = sys.argv[2:]
    for l in lines(path):
        if not want:
            print(l)
            continue
        wire = re.search(r"wireblk link (\w+)", l)
        got = []
        for k in want:
            m = re.search(r"\b%s=(\d+)" % re.escape(k), l)
            got.append("%s=%s" % (k, m.group(1) if m else "?"))
        print("%s %s" % (wire.group(1) if wire else "?", " ".join(got)))


if __name__ == "__main__":
    main()
