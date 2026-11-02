"""A terrain template's icons as pixels, for looking at them (and the heights' cliff table).

A template file (`.TEM`) is the game's icon set: a header (`IControl_Type` in tile.h: width and
height of an icon, how many logical icons, then offsets to the icons' pixels and to the map of
logical icons to stored ones, 0xFF for none), then 24 x 24 bytes an icon. A picture of it is
written as a PNG with a theater's palette (`TEMPERAT.PAL`: 6 bits a channel).
"""
import struct
import zlib


def icons(data):
    """(width, height, [pixels of each logical icon, or None])."""
    w, h, count = struct.unpack_from("<hhh", data, 0)
    pixels_at, _, _, _, map_at = struct.unpack_from("<iiiii", data, 12)
    out = []
    for i in range(count):
        stored = data[map_at + i] if map_at else i
        if stored == 0xFF:
            out.append(None)
        else:
            at = pixels_at + stored * w * h
            out.append(data[at:at + w * h])
    return w, h, out


def png(path, width, height, rgb_rows):
    """A PNG of rows of bytes (r, g, b each)."""
    raw = b"".join(b"\0" + bytes(row) for row in rgb_rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def sheet(path, entries, palette, columns=6, scale=2):
    """Each (label, template width, height, icons) laid out in cells of 4 x 4 icons, at `scale`."""
    cell = 4 * 24 + 8
    rows = (len(entries) + columns - 1) // columns
    W, H = columns * cell * scale, rows * cell * scale
    picture = [[40, 40, 40] * W for _ in range(H)]
    for n, (label, tw, th, ics) in enumerate(entries):
        ox, oy = (n % columns) * cell + 4, (n // columns) * cell + 4
        for i, ic in enumerate(ics):
            if ic is None or i >= tw * th:
                continue
            ix, iy = ox + (i % tw) * 24, oy + (i // tw) * 24
            for y in range(24):
                for x in range(24):
                    p = ic[y * 24 + x]
                    c = [min(255, palette[p * 3 + k] * 4) for k in range(3)]
                    for sy in range(scale):
                        row = picture[(iy + y) * scale + sy]
                        for sx in range(scale):
                            at = ((ix + x) * scale + sx) * 3
                            row[at:at + 3] = c
    png(path, W, H, picture)
