"""Binary PPM (P6) -> PNG, optionally cropped.  For looking at gfxdump output.

    python tools/ppm2png.py shot.ppm shot.png             # whole frame
    python tools/ppm2png.py shot.ppm md.png 160 80 320 224  # x y w h

Written out by hand rather than with Pillow because the IDF's Python has no
imaging library and adding one for this would be a poor trade.
"""
import struct
import sys
import zlib


def read_ppm(path):
    data = open(path, "rb").read()
    fields = []
    pos = 0
    while len(fields) < 4:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            while data[pos : pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[pos : pos + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3 : (y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


src, dst = sys.argv[1], sys.argv[2]
w, h, rgb = read_ppm(src)
if len(sys.argv) > 3:
    cx, cy, cw, ch = (int(v) for v in sys.argv[3:7])
    rows = [rgb[((cy + y) * w + cx) * 3 : ((cy + y) * w + cx + cw) * 3] for y in range(ch)]
    w, h, rgb = cw, ch, b"".join(rows)
write_png(dst, w, h, rgb)
print(f"{dst}: {w}x{h}")
