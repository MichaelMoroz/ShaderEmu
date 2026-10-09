# What stands in the room. The sofa, tables, stools, plants, laptop, television, boxes and
# some of the books are Poly Haven's models (assets.py); the rest is modelled here.
import math

from bookdesigns import CELLS, CELL_H, CELL_W, DESIGNS, size as book_size
from room import HALF_D, HALF_W, frame, on_wall
from pc import office_chair, station


def book(b, at, cell, deep, rot=(0, 0, 0)):
    """Book `cell` of the atlas standing on `at`, its spine towards -z, at the size it was drawn."""
    thick, tall = book_size(cell)
    w, h = DESIGNS[cell]
    t, d = thick / 2, deep / 2
    u0, u1, v1 = cell / CELLS, (cell + w / CELL_W) / CELLS, h / CELL_H
    plain = ((cell + 0.5 * w / CELL_W) / CELLS, 0.02)   # a spot of bare cloth, for the covers
    round_ = min(0.004, t * 0.4)   # the spine's edges are rounded
    v = [(-t + round_, 0, -d), (t - round_, 0, -d), (t - round_, tall, -d), (-t + round_, tall, -d),
         (-t, 0, -d + round_), (t, 0, -d + round_), (t, tall, -d + round_), (-t, tall, -d + round_),
         (-t, 0, d), (t, 0, d), (t, tall, d), (-t, tall, d)]
    lip = round_ / thick * (u1 - u0)
    spine = [(u0 + lip, 0), (u1 - lip, 0), (u1 - lip, v1), (u0 + lip, v1)]
    flat = [plain] * 4
    b.mesh(v, [(0, 1, 2, 3), (4, 0, 3, 7), (1, 5, 6, 2), (4, 7, 11, 8), (5, 9, 10, 6)],
           [spine, [(u0, 0), (u0 + lip, 0), (u0 + lip, v1), (u0, v1)], [(u1 - lip, 0), (u1, 0), (u1, v1), (u1 - lip, v1)], flat, flat],
           "Books", at, rot, normal='out')
    inset = 0.004
    p = [(-t + 0.003, inset, -d + 0.006), (t - 0.003, inset, -d + 0.006), (t - 0.003, tall - inset, -d + 0.006), (-t + 0.003, tall - inset, -d + 0.006),
         (-t + 0.003, inset, d - inset), (t - 0.003, inset, d - inset), (t - 0.003, tall - inset, d - inset), (-t + 0.003, tall - inset, d - inset)]
    zero = [(0, 0)] * 4
    b.mesh(p, [(3, 2, 6, 7), (0, 1, 5, 4), (4, 5, 6, 7)], [zero, zero, zero], "Paper", at, rot, normal='out')


def mug(b, at, mat="Ceramic", turn=0):
    with b.at(at, (0, turn, 0)):
        b.lathe((0, 0, 0), [(0, 0.004), (0.036, 0.004), (0.04, 0), (0.041, 0.095), (0.037, 0.095), (0.036, 0.008), (0, 0.008)], mat, segs=24)
        b.tube([(0.04, 0.075, 0), (0.062, 0.07, 0), (0.066, 0.045, 0), (0.058, 0.022, 0), (0.04, 0.02, 0)], 0.006, mat, segs=8, smooth=4)


def sofa(b):
    b.asset("sofa_02", (-0.4, 0, -HALF_D + 0.63), size=(2.26, None, None))   # its back clear of the panelling's cap
    b.obj("Cushions")
    with b.at((-0.4, 0, -HALF_D + 0.63)):
        b.box((-0.74, 0.60, 0.0), (0.42, 0.42, 0.13), "FabricCushion", bevel=0.06, segs=5, rot=(-24, 16, 8))
        b.box((0.76, 0.59, 0.0), (0.40, 0.40, 0.13), "FabricCushion", bevel=0.06, segs=5, rot=(-22, -22, -6))


def low_table(b):
    b.obj("Rugs")
    b.quad((-0.4, 0.006, -HALF_D + 1.75), (3.8, 2.2), "Rug", rot=(90, 0, 0), uvrect=(0, 0, 1, 1))
    at = (-0.4, 0, -HALF_D + 1.75)
    b.asset("modern_coffee_table_01", at, rot=(0, 90, 0), size=(None, 0.40, None))
    b.obj("Table things")
    with b.at(at):
        top = 0.401
        mug(b, (0.32, top, 0.08), turn=40)
        y = top
        for cell in (5, 11, 2):   # a stack, the widest lowest
            thick, tall = book_size(cell)
            book(b, (-0.16 - tall / 2 * 0, y + thick / 2, -0.04), cell, 0.19, rot=(0, b.rand(-14, 14), 90))
            y += thick + 0.001
        for i, turn in enumerate((8, -5)):
            b.box((0.02, top + 0.004 + i * 0.008, 0.16), (0.21, 0.007, 0.28), "Paper", rot=(0, turn + 90, 0))
        b.quad((0.02, top + 0.0165, 0.16), (0.21, 0.276), "Details", rot=(90, 85, 0), decal="magazine_1")


# The classroom in the middle of the den: two rows of two tables in the main desk's oak, an office
# chair and a computer of the decade (world/pc.py) at each of eight places, one a visitor. Unity
# puts each owner's picture on its glass (ShaderEmuStations.cs has the same numbers).
ROWS_Z = (1.5, -0.9)
PLACES_X = (-1.5, -0.5, 0.5, 1.5)
TABLE_TOP = 0.765   # as the main desk's
STATION_Z = 0.06    # a place's computer, behind its table's middle


def table(b, x, z):
    """Two metres of the main desk's kind: an oak top on panel ends, a board behind the knees."""
    with b.at((x, 0, z)):
        b.box((0, TABLE_TOP - 0.022, 0), (2.0, 0.044, 0.9), "WoodOak", bevel=0.01, segs=3, grain=0)
        for side in (-1, 1):
            b.box((side * 0.96, (TABLE_TOP - 0.044) / 2, 0), (0.03, TABLE_TOP - 0.044, 0.8), "WoodOak", bevel=0.004, segs=1, grain=1)
            b.box((side * 0.96, 0.012, 0), (0.05, 0.024, 0.84), "PlasticDark", bevel=0.004, segs=1)
        b.box((0, 0.45, 0.38), (1.89, 0.4, 0.022), "WoodOak", grain=0)
        for gx in (-0.5, 0.5):   # grommets for the cables
            b.lathe((gx, TABLE_TOP + 0.0005, 0.36), [(0, 0), (0.028, 0), (0.032, 0.002), (0.032, 0.004), (0.02, 0.005)], "PlasticDark", segs=16)


def classroom(b):
    b.obj("Tables")
    for z in ROWS_Z:
        for x in (-1.0, 1.0):
            table(b, x, z)
    b.obj("Chairs")
    for z in ROWS_Z:
        for x in PLACES_X:
            with b.at((x + b.rand(-0.05, 0.05), 0, z - 0.95 + b.rand(-0.08, 0.05)), (0, b.rand(-25, 25), 0)):
                office_chair(b)
    for row, z in enumerate(ROWS_Z):
        for place, x in enumerate(PLACES_X):
            b.obj("Station %d" % (row * len(PLACES_X) + place))   # one object a place: each has a tube of its own
            with b.at((x, TABLE_TOP, z + STATION_Z)):
                station(b)


def floor_lamp(b):
    b.obj("Floor lamp")
    with b.at((1.15, 0, -HALF_D + 0.4)):
        b.lathe((0, 0, 0), [(0.16, 0), (0.165, 0.012), (0.13, 0.024), (0.03, 0.04), (0.016, 0.07), (0.013, 1.45), (0.02, 1.46), (0.02, 1.5), (0, 1.5)],
                "Brass", segs=28)
        b.lathe((0, 1.46, 0), [(0.20, 0), (0.204, 0), (0.134, 0.30), (0.13, 0.30), (0.20, 0)], "Shade", segs=32, cap=False)
        for i in range(3):
            a = math.radians(i * 120)
            b.tube([(0.02 * math.cos(a), 1.49, 0.02 * math.sin(a)), (0.13 * math.cos(a), 1.75, 0.13 * math.sin(a))], 0.003, "Brass", segs=6)
        b.tube([(0.013, 1.2, 0), (0.03, 1.19, 0), (0.03, 1.08, 0)], 0.002, "Brass", segs=5, smooth=4)


def shelf(b):
    deep, wide, high = 0.32, 2.0, 2.1
    with on_wall(b, "left"):
        with b.at((-2.6, 0, 0)):
            b.obj("Shelf")
            b.box((0, high / 2, -0.008), (wide - 0.03, high - 0.02, 0.012), "WoodOak", grain=1)
            for x in (-wide / 2 + 0.015, wide / 2 - 0.015):
                b.box((x, high / 2, -deep / 2), (0.03, high, deep), "WoodOak", bevel=0.003, segs=1, grain=1)
            b.box((0, high + 0.012, -deep / 2 - 0.01), (wide + 0.04, 0.03, deep + 0.03), "WoodOak", bevel=0.008, grain=0)
            b.box((0, 0.025, -deep / 2 + 0.01), (wide - 0.06, 0.05, deep - 0.03), "WoodDark", grain=0)
            for s in range(6):
                b.box((0, 0.05 + s * 0.40, -deep / 2), (wide - 0.06, 0.03, deep - 0.012), "WoodOak", bevel=0.003, segs=1, grain=0)
            b.box((0, 1.25 + 0.2, -deep / 2), (0.025, 0.37, deep - 0.03), "WoodOak", grain=1)   # a divider on the games' shelf
            for s in range(5):
                y = 0.05 + s * 0.40 + 0.015
                if s == 3:   # games, face out, and a radio of its decade
                    b.obj("Books")
                    for i in range(3):
                        with b.at((-0.78 + i * 0.24, y + 0.123, -0.2), (8, 0, 0)):
                            b.box((0, 0, 0), (0.19, 0.245, 0.045), "Cardboard")
                            b.quad((0, 0, -0.0238), (0.186, 0.24), "Details", decal="game_%d" % ((i + 1) % 5))
                    b.asset("boombox", (0.5, y, -0.17), rot=(0, 180, 0), size=(0.56, None, None))
                    continue
                # an encyclopedia at one end, single books for the rest, a few leaning or lying
                b.obj("Books")
                left = -wide / 2 + 0.05
                right = wide / 2 - 0.05
                if s in (0, 2, 4):
                    set_wide = 0.55
                    start = left if s != 2 else right - set_wide
                    b.asset("book_encyclopedia_set_01", (start + set_wide / 2, y, -deep + 0.03 + 0.08), rot=(0, 180, 0), limit=20000)
                    if s != 2:
                        left += set_wide + 0.01
                    else:
                        right -= set_wide + 0.01
                x = left
                while x < right - 0.07:
                    roll = b.random.random()
                    cell = b.random.randrange(CELLS)
                    thick, tall = book_size(cell)
                    depth = b.rand(0.17, 0.24)
                    if roll < 0.08:           # a gap
                        x += b.rand(0.06, 0.16)
                    elif roll < 0.16 and x + tall + 0.02 < right:   # a few lying flat, one on another
                        yy = y
                        for _ in range(b.random.randint(2, 4)):
                            cell = b.random.randrange(CELLS)
                            thick, tall2 = book_size(cell)
                            book(b, (x + tall2 + b.rand(0, 0.01), yy + thick / 2, -deep + 0.04 + 0.1), cell, 0.19, rot=(0, b.rand(-4, 4), 90))
                            yy += thick + 0.0005
                        x += tall + 0.02
                    else:
                        lean = b.rand(4, 9) if roll > 0.93 else 0.0
                        book(b, (x + thick / 2, y, -deep + 0.03 + b.rand(0, 0.035) + depth / 2), cell, depth, rot=(0, 0, -lean))
                        x += thick + 0.0015 + tall * math.sin(math.radians(lean))
            b.asset("cardboard_box_01", (0.5, high + 0.027, -0.17), rot=(0, 98, 0), size=(0.4, 0.3, 0.5), limit=5000)


def boxes(b):
    at = (-HALF_W + 0.5, 0, -HALF_D + 0.5)
    b.asset("cardboard_box_01", (at[0], 0, at[2]), rot=(0, 102, 0), size=(0.5, 0.5, 0.6), limit=5000)
    b.asset("cardboard_box_01", (at[0] + 0.03, 0.5, at[2] + 0.02), rot=(0, 81, 0), size=(0.4, 0.34, 0.45), limit=5000)
    b.asset("cardboard_box_01", (at[0] + 0.7, 0, at[2] - 0.05), rot=(0, 31, 0), size=(0.42, 0.4, 0.5), limit=5000)
    b.asset("television_02", (at[0] + 0.03, 0.84, at[2] + 0.02), rot=(0, 40, 0))


def posters(b):
    with on_wall(b, "back"):
        b.obj("Posters")
        for i, mat in enumerate(("PosterDie", "PosterWords", "PosterGrid")):
            x = 1.5 - i * 1.1
            b.box((x, 1.95, -0.006), (0.78, 1.02, 0.012), "PlasticDark")
            moulding = [(0, 0), (0.03, 0), (0.03, 0.022), (0.022, 0.03), (0.006, 0.03), (0, 0.018)]
            frame(b, (x, 1.95, 0), 0.75, 0.99, moulding, "MetalBlack")
            b.quad((x, 1.95, -0.014), (0.74, 0.98), mat, uvrect=(0, 0, 1, 1))


def pot(b, at, r=0.2):
    """A clay pot with its saucer and soil; a plant stands in it at r * 1.8 up."""
    with b.at(at):
        b.lathe((0, 0, 0), [(r * 0.62, 0), (r * 0.66, 0.012), (r * 0.95, r * 1.7), (r * 1.06, r * 1.72), (r * 1.06, r * 1.95), (r * 0.94, r * 1.95),
                            (r * 0.9, r * 1.8), (0, r * 1.8)], "Terracotta", segs=32)
        b.lathe((0, 0, 0), [(r * 0.8, 0), (r * 0.9, 0.006), (r * 0.9, 0.02), (r * 0.82, 0.022)], "Terracotta", segs=32)
        b.lathe((0, r * 1.81, 0), [(0, 0.004), (r * 0.88, 0)], "Rubber", segs=20, cap=False)


def plants(b):
    # two money trees in pots of ours, and a smaller plant in its own by the sofa
    b.obj("Pots")
    left, right = (-HALF_W + 0.62, 0, 1.6), (HALF_W - 0.62, 0, 1.2)
    pot(b, left, 0.21)
    pot(b, right, 0.19)
    b.asset("pachira_aquatica_01", (left[0], 0.36, left[2]), rot=(0, 20, 0), parts=("_d",))
    b.asset("pachira_aquatica_01", (right[0], 0.33, right[2]), rot=(0, 250, 0), parts=("_c",))
    b.asset("potted_plant_02", (2.05, 0, -HALF_D + 0.6), rot=(0, 140, 0), parts=("_leaves", "_pot"), scale=1.19)
    b.asset("potted_plant_02", (2.05, 0, -HALF_D + 0.6), rot=(0, 140, 0), parts=("_dirt",), scale=1.19, limit=2500)   # its soil


def build(b):
    sofa(b)
    low_table(b)
    classroom(b)
    floor_lamp(b)
    shelf(b)
    boxes(b)
    posters(b)
    plants(b)
