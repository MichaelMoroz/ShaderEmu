# What is beyond the den's two doors: the holodeck, through the left wall, and the corridor
# behind the back wall, which is where a visitor arrives.
import math

from room import (DOOR_HIGH, DOOR_WIDE, DOOR_X, HALF_D, HALF_W, HOLO_HIGH, HOLO_WIDE, HOLO_Z, WALL_THICK,
                  door_casing, door_leaf, frame, wainscot, wall_with_door)
from furniture import pot

# the holodeck: a cube of grid, 8 m a side and 4 high, its door in the middle of its near wall
HOLO_SIDE, HOLO_TALL = 8.0, 4.0
HOLO_X1 = -HALF_W - WALL_THICK
HOLO_X0 = HOLO_X1 - HOLO_SIDE
HOLO_MID = (HOLO_X0 + HOLO_X1) / 2

# the corridor: along the back wall's far side, from the lift at its west end to past the den's door
COR_X0, COR_X1 = -6.0, HALF_W   # its east end is the building's outer wall, as the den's right wall is
COR_Z1 = -HALF_D - WALL_THICK
COR_WIDE, COR_HIGH = 2.4, 2.8
COR_Z0 = COR_Z1 - COR_WIDE
COR_LONG, COR_MID_X, COR_MID_Z = COR_X1 - COR_X0, (COR_X0 + COR_X1) / 2, (COR_Z0 + COR_Z1) / 2
WIN_WIDE, WIN_HIGH, WIN_Y = 1.5, 1.3, 1.75
LAMPS_X = (-3.9, -0.4, 3.1)


def grid_shell(b, mat, inset=0.0):
    """The holodeck's six faces, seen from inside; inset: that much nearer the room's middle."""
    s, t, i = HOLO_SIDE, HOLO_TALL, inset
    b.quad((HOLO_MID, i, HOLO_Z), (s, s), mat, rot=(90, 0, 0))
    b.quad((HOLO_MID, t - i, HOLO_Z), (s, s), mat, rot=(-90, 0, 0))
    with b.at((HOLO_MID, 0, HOLO_Z + s / 2 - i)):
        b.quad((0, t / 2, 0), (s, t), mat)
    with b.at((HOLO_MID, 0, HOLO_Z - s / 2 + i), (0, 180, 0)):
        b.quad((0, t / 2, 0), (s, t), mat)
    with b.at((HOLO_X0 + i, 0, HOLO_Z), (0, -90, 0)):
        b.quad((0, t / 2, 0), (s, t), mat)
    with b.at((HOLO_X1 - i, 0, HOLO_Z), (0, 90, 0)):   # this wall's x runs towards -z
        wall_with_door(b, s, t, -HOLO_WIDE / 2, HOLO_WIDE / 2, HOLO_HIGH, mat)


def holodeck(b):
    b.obj("Holodeck")
    grid_shell(b, "HoloGrid")
    # the arch inside, round the doorway
    w, h = HOLO_WIDE, HOLO_HIGH
    with b.at((HOLO_X1, 0, HOLO_Z), (0, 90, 0)):
        for k in (-1, 1):
            b.box((k * (w / 2 + 0.12), h / 2, -0.06), (0.24, h, 0.12), "RackSteel", bevel=0.014, grain=1)
            b.box((k * (w / 2 + 0.12), 1.3, -0.123), (0.1, 0.5, 0.008), "MetalBlack", bevel=0.003, segs=1)
            b.box((k * (w / 2 + 0.12), 1.3, -0.128), (0.05, 0.02, 0.004), "Amber")
        b.prism((0, h, -0.07), [(-w / 2 - 0.24, 0), (w / 2 + 0.24, 0), (w / 2 + 0.05, 0.36), (-w / 2 - 0.05, 0.36)], 0.14, "RackSteel",
                plane='xy', bevel=0.014)
    # what marks where the program's world may show: the same faces, a hair inside the grid
    b.obj("Holodeck mask")
    grid_shell(b, "HoloMask", 0.002)

    # the thing a visitor carries: the program's camera stands where it is and looks where it points
    b.obj("Holodeck control")
    b.lathe((0, -0.05, 0), [(0, 0), (0.085, 0), (0.1, 0.012), (0.1, 0.03), (0.07, 0.045), (0.03, 0.05), (0, 0.05)], "RackSteel", segs=24)
    b.lathe((0, 0, 0), [(0.03, 0), (0.034, 0.02), (0.028, 0.06), (0, 0.065)], "MetalSteel", segs=16)
    b.prism((0, -0.012, 0.05), [(-0.035, 0), (0.035, 0), (0, 0.075)], 0.008, "Amber", plane='xz')   # an arrow: forwards
    b.cyl((0, 0.068, 0), 0.012, 0.008, "Led", segs=12)


def corridor_door(b, x, name=None):
    """A neighbour's door, shut, in a wall's own space."""
    with b.at((x, 0, 0)):
        door_casing(b)
        door_leaf(b)
        b.box((0, 1.62, -0.05), (0.09, 0.06, 0.004), "Brass", bevel=0.002, segs=1)   # its number
        b.quad((0, 0.008, -0.38), (0.7, 0.42), "RoundRug", rot=(90, 0, 0), uvrect=(0.3, 0.3, 0.7, 0.54))   # a mat


def on(b, asset, x, y, deep, **more):
    """One of Poly Haven's models against a wall in the wall's own space: its back to the wall,
    its foot at y; deep is how far it stands out."""
    b.asset(asset, (x, y, -deep / 2), rot=(0, 180, 0), **more)


def corridor(b):
    b.obj("Corridor")
    b.quad((COR_MID_X, 0, COR_MID_Z), (COR_LONG, COR_WIDE), "Carpet", rot=(90, 0, 0))
    b.quad((COR_MID_X, COR_HIGH, COR_MID_Z), (COR_LONG, COR_WIDE), "Ceiling", rot=(-90, 0, 0))
    north = b.at((COR_MID_X, 0, COR_Z1))                     # x as the world's
    south = b.at((COR_MID_X, 0, COR_Z0), (0, 180, 0))        # x against the world's
    door_at = DOOR_X - COR_MID_X                               # the den's door, in the north wall's space
    with north:
        wall_with_door(b, COR_LONG, COR_HIGH, door_at - DOOR_WIDE / 2, door_at + DOOR_WIDE / 2, DOOR_HIGH, "Wall")
    with b.at((COR_MID_X, 0, COR_Z0), (0, 180, 0)):
        b.quad((0, COR_HIGH / 2, 0), (COR_LONG, COR_HIGH), "Wall")
    with b.at((COR_X1, 0, COR_MID_Z), (0, 90, 0)):   # the outer wall: a window onto the city
        w, y0, y1 = WIN_WIDE / 2, WIN_Y - WIN_HIGH / 2, WIN_Y + WIN_HIGH / 2
        b.quad((0, y0 / 2, 0), (COR_WIDE, y0), "Wall")
        b.quad((0, (y1 + COR_HIGH) / 2, 0), (COR_WIDE, COR_HIGH - y1), "Wall")
        for k in (-1, 1):
            b.quad((k * (COR_WIDE / 2 + w) / 2, WIN_Y, 0), (COR_WIDE / 2 - w, WIN_HIGH), "Wall")
        b.quad((0, y0, 0.1), (WIN_WIDE, 0.2), "Wall", rot=(90, 0, 0))
        b.quad((0, y1, 0.1), (WIN_WIDE, 0.2), "Wall", rot=(-90, 0, 0))
        b.quad((-w, WIN_Y, 0.1), (0.2, WIN_HIGH), "Wall", rot=(0, -90, 0))
        b.quad((w, WIN_Y, 0.1), (0.2, WIN_HIGH), "Wall", rot=(0, 90, 0))
        b.obj("Corridor window")
        casing = [(0, 0), (0.085, 0), (0.085, 0.012), (0.07, 0.022), (0.012, 0.026), (0, 0.016)]
        b.sweep(casing, [(w, y0, 0), (w, y1, 0), (-w, y1, 0), (-w, y0, 0)], "TrimWhite", up=(0, 0, -1))
        sash = [(0, -0.03), (0, 0.03), (-0.05, 0.03), (-0.06, 0.02), (-0.06, -0.03)]
        frame(b, (0, WIN_Y, 0.11), WIN_WIDE, WIN_HIGH, sash, "TrimWhite")
        b.box((0, WIN_Y, 0.11), (0.05, WIN_HIGH - 0.1, 0.05), "TrimWhite", bevel=0.008, grain=1)
        b.box((0, y0 - 0.016, -0.05), (WIN_WIDE + 0.24, 0.04, 0.25), "WoodOak", bevel=0.012, grain=0)
        b.box((0, y0 - 0.066, -0.009), (WIN_WIDE + 0.16, 0.06, 0.018), "WoodOak", bevel=0.005, grain=0)
        b.obj("Window glass")
        b.quad((0, WIN_Y, 0.12), (WIN_WIDE, WIN_HIGH), "Glass")
        b.obj("Corridor")
    with b.at((COR_X0, 0, COR_MID_Z), (0, -90, 0)):
        b.quad((0, COR_HIGH / 2, 0), (COR_WIDE, COR_HIGH), "Wall")

    b.obj("Corridor trim")
    half = COR_LONG / 2
    with b.at((COR_MID_X, 0, COR_Z1)):
        wainscot(b, -half, door_at - DOOR_WIDE / 2 - 0.09)
        wainscot(b, door_at + DOOR_WIDE / 2 + 0.09, half)
        with b.at((door_at, 0, 0)):
            door_casing(b)
    doors = (-3.6, 0.4, 3.9)   # the neighbours', in the south wall's space
    with b.at((COR_MID_X, 0, COR_Z0), (0, 180, 0)):
        edges = [-half] + [x + k * (DOOR_WIDE / 2 + 0.09) for x in doors for k in (-1, 1)] + [half]
        for i in range(0, len(edges), 2):
            wainscot(b, edges[i], edges[i + 1])
    with b.at((COR_X1, 0, COR_MID_Z), (0, 90, 0)):
        wainscot(b, -COR_WIDE / 2, COR_WIDE / 2)
        with b.at((0, 0.44, -0.075)):   # a radiator under the window
            b.box((0, 0, 0.03), (1.2, 0.54, 0.012), "TrimWhite", bevel=0.004)
            for i in range(20):
                b.box((-0.57 + i * 0.06, 0, 0), (0.034, 0.56, 0.05), "TrimWhite", bevel=0.012, grain=1)
            b.box((0, 0.285, 0.005), (1.2, 0.02, 0.07), "TrimWhite", bevel=0.006)
    cove = [(0, 0), (0.05, 0), (0.05, -0.01), (0.03, -0.016), (0.014, -0.034), (0.01, -0.055), (0, -0.055)]
    b.sweep(cove, [(COR_X0, COR_HIGH, COR_Z1), (COR_X1, COR_HIGH, COR_Z1), (COR_X1, COR_HIGH, COR_Z0), (COR_X0, COR_HIGH, COR_Z0)],
            "TrimWhite", closed=True)

    b.obj("Corridor doors")
    with b.at((COR_MID_X, 0, COR_Z0), (0, 180, 0)):
        for x in doors:
            corridor_door(b, x)

    for x in LAMPS_X:   # hanging lamps, drawn up short of a tall visitor's head
        b.asset("modern_ceiling_lamp_01", (x, COR_HIGH - 0.6, COR_MID_Z), size=(None, 0.6, None))
    with b.at((COR_MID_X, 0, COR_Z0), (0, 180, 0)):
        for x in (-1.6, 2.15):
            on(b, "industrial_wall_sconce", x, 1.85, 0.25)

    # the lift, at the west end: where a visitor steps out
    b.obj("Lift")
    with b.at((COR_X0, 0, COR_MID_Z), (0, -90, 0)):
        b.box((0, 1.08, -0.01), (1.36, 2.16, 0.02), "RackSteel")
        for k in (-1, 1):
            b.box((k * 0.3, 1.04, -0.026), (0.59, 2.06, 0.012), "MetalSteel", bevel=0.003, segs=1, grain=1)
            b.box((k * 0.67, 1.08, -0.04), (0.1, 2.16, 0.08), "MetalSteel", bevel=0.008, grain=1)
        b.box((0, 2.2, -0.04), (1.44, 0.12, 0.08), "MetalSteel", bevel=0.008, grain=0)
        b.box((0, 2.36, -0.012), (0.36, 0.1, 0.024), "PlasticDark", bevel=0.004, segs=1)
        b.box((0, 2.36, -0.026), (0.2, 0.05, 0.004), "Amber")                      # the floor it is at
        b.box((0.92, 1.15, -0.008), (0.1, 0.2, 0.016), "MetalSteel", bevel=0.004, segs=1)
        for y, mat in ((1.19, "Amber"), (1.11, "PlasticDark")):
            b.cyl((0.92, y, -0.02), 0.016, 0.008, mat, segs=14, rot=(90, 0, 0))

    b.obj("Corridor things")
    with b.at((COR_MID_X, 0, COR_Z1)):   # the den's wall
        on(b, "painted_wooden_bench", -1.2, 0, 0.5)
        on(b, "rubber_boots", -0.3, 0, 0.3, parts=("rubber_boots_l", "rubber_boots_r"))
        on(b, "korean_fire_extinguisher_01", door_at - 1.0, 0, 0.38)
        on(b, "fire_alarm", door_at - 1.0, 1.35, 0.03)
        on(b, "korean_public_payphone_01", -3.2, 1.0, 0.3)
        on(b, "vintage_suitcase", -4.3, 0, 0.26, parts=("vintage_suitcase_01",))
        on(b, "hanging_picture_frame_02", 1.45, 1.45, 0.04)
        # a notice board over the bench
        with b.at((-1.2, 1.65, 0)):
            b.box((0, 0, -0.008), (1.0, 0.7, 0.016), "Cardboard")
            moulding = [(0, 0), (0.03, 0), (0.03, 0.018), (0.02, 0.026), (0.006, 0.026), (0, 0.016)]
            frame(b, (0, 0, 0), 0.98, 0.68, moulding, "WoodOak")
            for i, (x, y, turn) in enumerate(((-0.3, 0.12, 4), (0.05, 0.15, -6), (0.3, -0.05, 3), (-0.15, -0.14, -3))):
                b.quad((x, y, -0.018), (0.12, 0.12), "Details", rot=(0, 0, turn), decal="note_%d" % (i % 3))
            b.quad((0.28, 0.2, -0.0175), (0.21, 0.26), "Paper", rot=(0, 0, -2))
    with b.at((COR_MID_X, 0, COR_Z0), (0, 180, 0)):   # the neighbours' wall
        on(b, "hanging_picture_frame_01", -1.6, 0.9, 0.02)
        on(b, "side_table_tall_01", 2.15, 0, 0.38)
        on(b, "wicker_basket_01", 2.15, 0.76, 0.34)
        on(b, "ornate_mirror_01", 2.15, 1.1, 0.03)
    b.obj("Pots")
    spot = (COR_X1 - 0.5, 0, COR_Z0 + 0.42)   # by the window
    pot(b, spot, 0.19)
    b.asset("pachira_aquatica_01", (spot[0], 0.33, spot[2]), rot=(0, 110, 0), parts=("_c",))
    b.asset("cardboard_box_01", (COR_X0 + 0.5, 0, COR_Z0 + 0.4), rot=(0, 12, 0), size=(0.5, 0.42, 0.5), limit=5000)
    b.asset("cardboard_box_01", (COR_X0 + 0.52, 0.42, COR_Z0 + 0.42), rot=(0, -20, 0), size=(0.4, 0.3, 0.4), limit=5000)


def build(b):
    holodeck(b)
    corridor(b)
