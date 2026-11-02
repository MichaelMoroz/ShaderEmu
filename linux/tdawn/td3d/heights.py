#!/usr/bin/env python3
"""Heights for Tiberian Dawn's maps, made at image build (docs/plans/td3d-plan.md, step 2).

The maps have none. Each is made from the map's own templates (`templates.py`), so nothing is
drawn by hand for a mission:

- Ground is on terraces, a rung apart. Cliff cells (the `SLOPE` templates' rock icons, and a
  shore's rock) are walls; what they cut the map into are regions, each on one terrace. The
  largest region is the main ground; a region beyond a cliff from it is a rung up if it is
  smaller than the one it was reached from, a rung down if larger (plateaus are the smaller
  side in these maps: a guess, said in the plan, to be looked at in captures).
- Water lies under its terrace, a beach and a river a little under it, a cliff's cells between
  the terraces on its two sides; a cliff with the same terrace on both sides (its line has a gap
  somewhere) is a ridge.
- Heights are of the cells' corners, 65 x 65 a map, a byte each, in quarters of a game pixel
  (a cell is 24 pixels, 96 units). Corners that touch neither water nor a cliff are smoothed.

A hill is a look: the game's rules never read these (docs/games/tdawn.md).

The file (`tdawn-heights.dat` in the ROM): "TDH1", a count (32 bits, little-endian), then a
record a map of a name (12 bytes, NUL-padded, the scenario's root as `Scen.ScenarioName` has it)
and where its 65 x 65 bytes begin in the file; the corners by rows, north first.

    python3 heights.py --source TREE/tiberiandawn --mix DEMO.MIX --mix DEMOL.MIX --out tdawn-heights.dat [--pgm DIR]
"""
import argparse
import os
import struct
import sys
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mix as mixfile  # noqa: E402
import templates as tt  # noqa: E402

CELLS = 64
CORNERS = CELLS + 1
UNIT = 4            # height units a game pixel
RUNG = 48           # a terrace's step: 12 pixels, half a cell
BASE = 32           # the lowest terrace's height
WATER_DOWN = 14
BEACH_DOWN = 6
RIVER_DOWN = 8
RIDGE_UP = 24
TOP_LEVEL = 4

OPEN, WATER, BEACH, RIVER, CLIFF = range(5)


def cells_of(binary, temps):
    """A map's .BIN as (template, icon) a cell, and the class of each cell."""
    if len(binary) < CELLS * CELLS * 2:
        raise ValueError("a map's binary of %d bytes" % len(binary))
    classes = []
    for i in range(CELLS * CELLS):
        number, icon = binary[2 * i], binary[2 * i + 1]
        t = temps.get(number if number != tt.NONE else 0) or temps[0]
        land = t.land_of(icon) if number != tt.NONE else t.land
        if land == "ROCK" and t.kind in ("SLOPE", "WATER"):
            classes.append(CLIFF)
        elif land == "WATER":
            classes.append(RIVER if t.kind == "RIVER" else WATER)
        elif land == "BEACH":
            classes.append(BEACH)
        else:
            classes.append(OPEN)
    return classes


def regions_of(classes):
    """Each cell's region (-1 for a cliff's) and the regions' sizes: what the cliffs cut apart."""
    region = [-1] * (CELLS * CELLS)
    sizes = []
    for start in range(CELLS * CELLS):
        if classes[start] == CLIFF or region[start] >= 0:
            continue
        number, size = len(sizes), 0
        region[start] = number
        todo = deque([start])
        while todo:
            at = todo.popleft()
            size += 1
            x, y = at % CELLS, at // CELLS
            for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                if 0 <= nx < CELLS and 0 <= ny < CELLS:
                    n = ny * CELLS + nx
                    if classes[n] != CLIFF and region[n] < 0:
                        region[n] = number
                        todo.append(n)
        sizes.append(size)
    return region, sizes


def cliff_sides(classes, region):
    """For each cliff cell, the regions within two cells of it; and which regions face each other."""
    sides = {}
    touching = {}
    for at in range(CELLS * CELLS):
        if classes[at] != CLIFF:
            continue
        x, y = at % CELLS, at // CELLS
        near = set()
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                nx, ny = x + dx, y + dy
                if 0 <= nx < CELLS and 0 <= ny < CELLS and region[ny * CELLS + nx] >= 0:
                    near.add(region[ny * CELLS + nx])
        sides[at] = near
        for a in near:
            touching.setdefault(a, set()).update(near - {a})
    return sides, touching


def levels_of(sizes, touching):
    """A terrace for every region, from the largest outwards."""
    level = [None] * len(sizes)
    for first in sorted(range(len(sizes)), key=lambda r: -sizes[r]):
        if level[first] is not None:
            continue
        level[first] = 1
        todo = deque([first])
        while todo:
            r = todo.popleft()
            for n in sorted(touching.get(r, ())):
                if level[n] is None:
                    step = 1 if sizes[n] <= sizes[r] else -1
                    level[n] = max(0, min(TOP_LEVEL, level[r] + step))
                    todo.append(n)
    return level


def make(binary, temps):
    """The 65 x 65 corner heights of a map (bytes), and the cells' classes."""
    classes = cells_of(binary, temps)
    region, sizes = regions_of(classes)
    sides, touching = cliff_sides(classes, region)
    level = levels_of(sizes, touching)
    cell = [0] * (CELLS * CELLS)
    for at in range(CELLS * CELLS):
        c = classes[at]
        if c == CLIFF:
            near = [level[r] for r in sides[at]] or [1]
            low, high = min(near), max(near)
            cell[at] = BASE + RUNG * low + (RUNG * (high - low) // 2 if high > low else RIDGE_UP)
            continue
        h = BASE + RUNG * level[region[at]]
        cell[at] = h - (WATER_DOWN if c == WATER else BEACH_DOWN if c == BEACH else RIVER_DOWN if c == RIVER else 0)
    # a corner is the mean of the cells round it; one by water is the lowest of them, so water is flat
    corner = [0] * (CORNERS * CORNERS)
    fixed = [False] * (CORNERS * CORNERS)
    for cy in range(CORNERS):
        for cx in range(CORNERS):
            round_it = [(x, y) for x in (cx - 1, cx) for y in (cy - 1, cy) if 0 <= x < CELLS and 0 <= y < CELLS]
            values = [cell[y * CELLS + x] for x, y in round_it]
            kinds = [classes[y * CELLS + x] for x, y in round_it]
            at = cy * CORNERS + cx
            if WATER in kinds or RIVER in kinds:
                corner[at] = min(values)
                fixed[at] = True
            else:
                corner[at] = (sum(values) + len(values) // 2) // len(values)
                fixed[at] = CLIFF in kinds
    # smoothing: the free corners toward their neighbours, a few times (steps become slopes)
    for _ in range(4):
        before = corner[:]
        for cy in range(CORNERS):
            for cx in range(CORNERS):
                at = cy * CORNERS + cx
                if fixed[at]:
                    continue
                total, count = 2 * before[at], 2
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < CORNERS and 0 <= ny < CORNERS:
                        total += before[ny * CORNERS + nx]
                        count += 1
                corner[at] = (total + count // 2) // count
    return bytes(max(0, min(255, h)) for h in corner), classes


def scenario_names(mixes):
    """The demo's missions whose map is in these mixes: GDI's and Nod's of 1 to 15, A to D."""
    names = []
    for side in "GB":
        for number in range(1, 16):
            for variant in "ABCD":
                root = "SC%s%02dE%s" % (side, number, variant)
                if mixfile.find(mixes, root + ".BIN") is not None:
                    names.append(root)
    return names


def pack(maps):
    """The file: "TDH1", the count, the records, the heights."""
    head = 8 + 16 * len(maps)
    out = bytearray(b"TDH1" + struct.pack("<I", len(maps)))
    at = head
    for name, data in maps:
        if len(name) > 11:
            raise ValueError("a name longer than 11: " + name)
        out += name.encode("ascii").ljust(12, b"\0") + struct.pack("<I", at)
        at += len(data)
    for _, data in maps:
        out += data
    return bytes(out)


def unpack(data):
    """{name: 65 x 65 bytes} from such a file (a test, and a look)."""
    if data[:4] != b"TDH1":
        raise ValueError("not a heights file")
    count = struct.unpack_from("<I", data, 4)[0]
    maps = {}
    for i in range(count):
        name = data[8 + 16 * i:20 + 16 * i].rstrip(b"\0").decode("ascii")
        at = struct.unpack_from("<I", data, 20 + 16 * i)[0]
        maps[name] = data[at:at + CORNERS * CORNERS]
    return maps


def pgm(path, heights):
    """A picture to look at: each corner a pixel, scaled up four times."""
    with open(path, "wb") as f:
        f.write(b"P5 %d %d 255\n" % (CORNERS * 4, CORNERS * 4))
        for y in range(CORNERS * 4):
            row = heights[(y // 4) * CORNERS:(y // 4 + 1) * CORNERS]
            f.write(bytes(v for v in row for _ in range(4)))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--source", required=True, help="the fetched tree's tiberiandawn folder")
    p.add_argument("--mix", action="append", required=True, help="a mix that holds maps (first wins)")
    p.add_argument("--out", required=True)
    p.add_argument("--pgm", help="a folder for a picture of each map's heights")
    a = p.parse_args()
    temps = tt.load(a.source)
    mixes = [mixfile.Mix.open(m) for m in a.mix]
    maps = []
    for root in scenario_names(mixes):
        heights, classes = make(mixfile.find(mixes, root + ".BIN"), temps)
        maps.append((root, heights))
        if a.pgm:
            os.makedirs(a.pgm, exist_ok=True)
            pgm(os.path.join(a.pgm, root + ".pgm"), heights)
        counts = [classes.count(k) for k in range(5)]
        print("%s: open %d, water %d, beach %d, river %d, cliff %d; heights %d to %d"
              % (root, counts[0], counts[1], counts[2], counts[3], counts[4], min(heights), max(heights)))
    with open(a.out, "wb") as f:
        f.write(pack(maps))
    print("%d maps, %d bytes: %s" % (len(maps), os.path.getsize(a.out), a.out))


if __name__ == "__main__":
    main()
