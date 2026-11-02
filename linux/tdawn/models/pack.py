"""The models' file for the ROM (`tdawn-models.dat`), in the GPU's packed vertices (docs/machine/gpu.md).

A vertex is one word: x, y and z a byte each, whole numbers 0 to 255 that the draw's matrix scales
and moves (a part's `origin` is where 0, 0, 0 lies and `step` how far a byte is, in pixels about the
part's pivot), and a tag in the top byte: the material in its high four bits, which way the face
looks in its low four (`NORMALS`). The game makes a table of 256 colours a house and a light from
those (the `0x2000` table: GDI and Nod are one model). The texture coordinates, a word a vertex in a
second buffer, are all zero (no textures yet): one such buffer serves every part.

    0   "TDM1"
    4   models (32 bits)          every number little-endian
    8   where the 16 normals are: x, y, z, 0 a signed byte each, of 127
    12  where the coordinates are (zero words, as many as the largest part's vertices)
    16  a model a 32 bytes:   name (8, NUL-padded), kind, parts, cells across, cells down (a byte each),
                              where its first part is (32 bits), bounds (6 x 16 bits, 16ths of a pixel), 4 spare
    ... a part a 32 bytes:    where its vertices are (16-aligned), how many (a multiple of 3), origin x, y, z
                              and step (16.16 pixels), pivot x, y, z (16 bits, 16ths of a pixel), turns (a byte), 1 spare
    ... the vertices
"""
import math
import struct

KINDS = {"unit": 0, "building": 1, "aircraft": 2, "infantry": 3, "terrain": 4}

# up; eight round the model (0 ahead, clockwise from above); four sloped up at 45 degrees; down
NORMALS = [(0.0, 0.0, 1.0)]
NORMALS += [(math.sin(k * math.pi / 4), math.cos(k * math.pi / 4), 0.0) for k in range(8)]
NORMALS += [(math.sin(q * math.pi / 2) * 0.7071, math.cos(q * math.pi / 2) * 0.7071, 0.7071) for q in range(4)]
NORMALS += [(0.0, 0.0, -1.0), (0.0, 0.0, 1.0), (0.0, 0.0, 1.0)]


def normal_index(n):
    """Which of the 16 a face's normal is nearest to."""
    best, which = -2.0, 0
    for i, d in enumerate(NORMALS[:14]):
        dot = n[0] * d[0] + n[1] * d[1] + n[2] * d[2]
        if dot > best + 1e-9:
            best, which = dot, i
    return which


def _align(data, to=16):
    data.extend(b"\0" * (-len(data) % to))


def part_words(part):
    """A part's packed vertices, its origin and its step (pixels)."""
    from mesh import normal
    points = [q for t, _ in part.triangles for q in t]
    if not points:
        return [], (0.0, 0.0, 0.0), 1.0
    low = [min(p[k] for p in points) for k in range(3)]
    high = [max(p[k] for p in points) for k in range(3)]
    step = max(max(high[k] - low[k] for k in range(3)) / 255.0, 1.0 / 64)
    words = []
    for (a, b, c), material in part.triangles:
        tag = material << 4 | normal_index(normal(a, b, c))
        for p in (a, b, c):
            q = [int(round((p[k] - low[k]) / step)) for k in range(3)]
            if not all(0 <= v <= 255 for v in q):
                raise ValueError("a point outside its part's bytes")
            words.append(q[0] | q[1] << 8 | q[2] << 16 | tag << 24)
    return words, tuple(low), step


def fixed(v):
    return int(round(v * 65536))


def pack(models):
    """The file's bytes for a list of mesh.Model."""
    head = bytearray(b"TDM1" + struct.pack("<III", len(models), 0, 0))
    records = bytearray()
    parts_table = bytearray()
    blocks = []
    most = 0
    first_part = 16 + 32 * len(models)
    part_at = first_part
    for m in models:
        if len(m.name) > 8 or len(m.parts) > 255:
            raise ValueError(m.name)
        (lx, ly, lz), (hx, hy, hz) = m.bounds()
        b16 = [int(round(v * 16)) for v in (lx, ly, lz, hx, hy, hz)]
        records += m.name.encode("ascii").ljust(8, b"\0")
        records += struct.pack("<BBBBI6hI", KINDS[m.kind], len(m.parts), m.cells[0], m.cells[1], part_at, *b16, 0)
        part_at += 32 * len(m.parts)
        for p in m.parts:
            words, origin, step = part_words(p)
            most = max(most, len(words))
            blocks.append((len(parts_table), words))
            pivot = [int(round(v * 16)) for v in p.pivot]
            parts_table += struct.pack("<IIiiii3hBB", 0, len(words), fixed(origin[0]), fixed(origin[1]), fixed(origin[2]),
                                       fixed(step), *pivot, 1 if p.turns else 0, 0)
    out = head + records + parts_table
    _align(out)
    normals_at = len(out)
    for n in NORMALS:
        out += struct.pack("<bbbb", *(int(round(v * 127)) for v in n), 0)
    _align(out)
    coords_at = len(out)
    out += b"\0" * (4 * most)
    _align(out)
    for record_at, words in blocks:
        struct.pack_into("<I", out, first_part + record_at, len(out))
        out += struct.pack("<%dI" % len(words), *words)
        _align(out)
    struct.pack_into("<II", out, 8, normals_at, coords_at)
    return bytes(out)


def unpack(data):
    """[{name, kind, cells, bounds, parts: [{count, origin, step, pivot, turns, vertices: [(x, y, z, tag)]}]}]"""
    if data[:4] != b"TDM1":
        raise ValueError("not a models file")
    count, normals_at, coords_at = struct.unpack_from("<III", data, 4)
    kinds = {v: k for k, v in KINDS.items()}
    models = []
    for i in range(count):
        at = 16 + 32 * i
        name = data[at:at + 8].rstrip(b"\0").decode("ascii")
        kind, nparts, cx, cy, first = struct.unpack_from("<BBBBI", data, at + 8)
        bounds = [v / 16 for v in struct.unpack_from("<6h", data, at + 16)]
        parts = []
        for j in range(nparts):
            v_at, n, ox, oy, oz, step, px, py, pz, turns, _ = struct.unpack_from("<IIiiii3hBB", data, first + 32 * j)
            if v_at % 16:
                raise ValueError("vertices not on 16 bytes")
            words = struct.unpack_from("<%dI" % n, data, v_at)
            parts.append({"count": n, "origin": (ox / 65536, oy / 65536, oz / 65536), "step": step / 65536,
                          "pivot": (px / 16, py / 16, pz / 16), "turns": bool(turns),
                          "vertices": [(w & 255, w >> 8 & 255, w >> 16 & 255, w >> 24) for w in words]})
        models.append({"name": name, "kind": kinds[kind], "cells": (cx, cy), "bounds": bounds, "parts": parts})
    normals = [struct.unpack_from("<bbb", data, normals_at + 4 * k) for k in range(16)]
    return models, normals, coords_at
