"""Models from the packed file drawn in software (z-buffered, flat-lit), to look at them on the host.

It draws what the game will: the bytes as the GPU reads them, each part moved by its origin and
step, a turret turned about its pivot, the house colours from a table, the light from the
north-west above as the game's table has it (`ShaderEmu_GL_Model_Table` in gl.cpp makes the same
from the same numbers). The camera is the tilted view's (linux/tdawn/td3d.h) at 60 degrees.
"""
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mesh  # noqa: E402
import pack  # noqa: E402

HOUSES = {"GDI": ((206, 172, 72), (140, 112, 40)), "Nod": ((170, 40, 32), (96, 92, 88))}
LIGHT = (-0.45, 0.45, 0.77)     # from the north-west, above: x right, y north, z up
AMBIENT, DIFFUSE = 0.45, 0.65


def table(house):
    """The 256 colours of a house's models: material (high four bits) lit by normal (low four)."""
    out = []
    for material in range(16):
        colour = HOUSES[house][0] if material == mesh.HOUSE else HOUSES[house][1] if material == mesh.HOUSE_DARK \
            else mesh.MATERIALS.get(material) or (255, 0, 255)
        for n in pack.NORMALS:
            light = AMBIENT + DIFFUSE * max(0.0, n[0] * LIGHT[0] + n[1] * LIGHT[1] + n[2] * LIGHT[2])
            out.append(tuple(min(255, int(c * light)) for c in colour))
    return out


def points_of(part, facing, turret):
    """A part's triangles in model pixels, turned: facing and turret in the game's 256ths, clockwise."""
    ox, oy, oz = part["origin"]
    s = part["step"]
    px, py, pz = part["pivot"]
    out = []
    a = (turret if part["turns"] else 0) * math.pi / 128
    ca, sa = math.cos(a), math.sin(a)
    for x, y, z, tag in part["vertices"]:
        mx, my, mz = ox + x * s, oy + y * s, oz + z * s
        # (a turret about its pivot, clockwise from above; then the whole by the facing)
        mx, my = mx * ca + my * sa, -mx * sa + my * ca
        out.append((mx + px, my + py, mz + pz, tag))
    b = facing * math.pi / 128
    cb, sb = math.cos(b), math.sin(b)
    return [(x * cb + y * sb, -x * sb + y * cb, z, tag) for x, y, z, tag in out]


def draw(model, facing, house, size=96, zoom=2.0, pitch=60):
    """A picture (rows of r, g, b) of one model from the south, tilted."""
    colours = table(house)
    c, s = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    pixels = [[(48, 64, 40)] * size for _ in range(size)]
    depth = [[1e9] * size for _ in range(size)]
    for part in model["parts"]:
        pts = points_of(part, facing, facing // 2)
        for i in range(0, len(pts), 3):
            tri = []
            for x, y, z, tag in pts[i:i + 3]:
                # orthographic tilt (enough to look at): screen y from y and z, depth along the view
                sx = size / 2 + x * zoom
                sy = size * 0.6 - (y * s + z * c) * zoom
                d = -y * c + z * s
                tri.append((sx, sy, -d))
            colour = colours[pts[i][3]]
            fill(pixels, depth, tri, colour)
    return pixels


def fill(pixels, depth, tri, colour):
    (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = tri
    area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
    if abs(area) < 1e-9:
        return
    size = len(pixels)
    for y in range(max(0, int(min(y0, y1, y2))), min(size, int(max(y0, y1, y2)) + 1)):
        for x in range(max(0, int(min(x0, x1, x2))), min(size, int(max(x0, x1, x2)) + 1)):
            px, py = x + 0.5, y + 0.5
            w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area
            w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area
            w2 = 1 - w0 - w1
            if w0 < 0 or w1 < 0 or w2 < 0:
                continue
            z = w0 * z0 + w1 * z1 + w2 * z2
            if z < depth[y][x]:
                depth[y][x] = z
                pixels[y][x] = colour


def png(path, rows):
    raw = b"".join(b"\0" + bytes(v for p in row for v in p) for row in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", len(rows[0]), len(rows), 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def sheet(data, path, facings=(0, 32, 64, 96, 128, 160, 192, 224)):
    """Every model of a file, a row each, at these facings; GDI's colours, Nod's for the second half."""
    models, _, _, _ = pack.unpack(data)
    rows = []
    for k, m in enumerate(models):
        size = 96 if m["kind"] == "unit" else 160
        zoom = 2.5 if m["kind"] == "unit" else 1.6
        pictures = [draw(m, f, "GDI" if k % 2 == 0 else "Nod", size, zoom) for f in facings]
        for y in range(size):
            row = []
            for p in pictures:
                row += p[y]
            row += [(0, 0, 0)] * (160 * len(facings) - len(row))
            rows.append(row)
    png(path, rows)
