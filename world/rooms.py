# The three side rooms off the corridor by the lift (docs/world/world.md, "The side rooms"): the
# neighbours' doors stand open now, and behind each is a small, warm room of the decade with a
# sofa, a bookcase across from it and a computer on a desk against the far wall, places 12 to 14
# of the classroom's kind (N13, N14 and N15 on their doors). N15, at the building's corner, has a
# window onto the city. Their floors, ceilings and walls are world/shell.py's (rooms_shell);
# what stands in them is Annex's. Models/rooms.json says where the places, lights and solids are.
import json
import math
import os
import random

from mathutils import Vector

import lib
import pc
import toys
from furniture import PLAYER, TAPE
from room import DOOR_HIGH, DOOR_WIDE, WALL_THICK, door_casing, door_leaf, frame, light_switch, reveal, wall_socket

COR_Z0, COR_HIGH, COR_MID_X = -8.1, 2.8, -0.75   # (world/annex.py's corridor: its south wall's face, height and middle)
HIGH = COR_HIGH
Z1 = COR_Z0 - WALL_THICK   # the rooms' door walls, from inside
DEEP = 3.6
Z0 = Z1 - DEEP             # their far walls
# number, west and east walls' x, the door's x (the corridor's three doors), the couch's side (+1 east), a window in the east wall
ROOMS = [(13, -6.8, -3.1, -4.65, 1, False),
         (14, -2.9, 0.7, -1.15, -1, False),
         (15, 0.9, 4.5, 2.85, 1, True)]
FIRST_PLACE = 12           # the classroom's places are 0 to 11
TABLE_TOP = 0.765          # as the classroom's (pc.TABLE_HIGH): Unity lays every place's keys at one height
DESK = (1.6, 0.9)          # wide, deep
DESK_TOP_T = 0.0196        # its top's thickness: what the clamp lamp's jaw takes (3/4 in, as desks of the decade)
DESK_OFF = 0.30            # from the far wall: the strip and the leads lie behind it
COUCH_OFF = {13: 0.0, 14: 0.0, 15: 0.08}   # the sofa off its wall: in N15 the radiator under the window is behind it, 9 cm from its back (more and its end is at the desk's)
WIN_WIDE, WIN_HIGH, WIN_Y = 1.5, 1.3, 1.75
WIN_Z = Z0 + 1.9           # N15's window, in the east wall


def corridor_doors():
    """The doors' x in the corridor's south wall's own space (world x = COR_MID_X - x)."""
    return [COR_MID_X - door for _, _, _, door, _, _ in ROOMS]


def wall_frame(room, side):
    """A wall of a room as (at, rot, length, its middle's x): x along it, y up, the room towards -z."""
    _, x0, x1, _, _, _ = room
    cx, cz = (x0 + x1) / 2, (Z0 + Z1) / 2
    return {"north": ((cx, 0, Z1), (0, 0, 0), x1 - x0), "south": ((cx, 0, Z0), (0, 180, 0), x1 - x0),
            "west": ((x0, 0, cz), (0, -90, 0), DEEP), "east": ((x1, 0, cz), (0, 90, 0), DEEP)}[side]


def to_wall(room, side, x, z):
    """A room's point (world x, z) along a wall, in that wall's own x."""
    (ax, _, az), rot, _ = wall_frame(room, side)
    return {"north": x - ax, "south": ax - x, "west": z - az, "east": az - z}[side]


# ---- the shell (world/shell.py calls this: one mesh a surface, its lightmap laid out here)
def rooms_shell(b, gap):
    for room in ROOMS:
        number, x0, x1, door, _, window = room
        wide, cx, cz = x1 - x0, (x0 + x1) / 2, (Z0 + Z1) / 2
        b.obj("Room %d floor" % number)
        with b.at((cx, 0, cz), (90, 0, 0)):
            b.sheet((-wide / 2, -DEEP / 2, wide / 2, DEEP / 2), "Parquet", extent=max(wide, DEEP))
        b.obj("Room %d ceiling" % number)
        with b.at((cx, HIGH, cz), (-90, 0, 0)):
            b.sheet((-wide / 2, -DEEP / 2, wide / 2, DEEP / 2), "Ceiling", extent=max(wide, DEEP))
        b.obj("Room %d walls" % number)
        row = HIGH + gap
        extent = 4 * row + (0.2 + gap if window else 0)
        for i, side in enumerate(("north", "east", "south", "west")):
            at, rot, length = wall_frame(room, side)
            holes = []
            if side == "north":
                holes.append((door - cx - DOOR_WIDE / 2, -1, door - cx + DOOR_WIDE / 2, DOOR_HIGH))
            if side == "east" and window:
                w = to_wall(room, "east", x1, WIN_Z)
                holes.append((w - WIN_WIDE / 2, WIN_Y - WIN_HIGH / 2, w + WIN_WIDE / 2, WIN_Y + WIN_HIGH / 2))
            with b.at(at, rot):
                b.sheet((-length / 2, 0, length / 2, HIGH), "Wall", holes, light=(0, i * row), extent=extent, uv_from=(0, HIGH / 2))
                if side == "east" and window:   # the window's reveal, 0.2 deep: its four faces side by side in the last row
                    w = holes[0][0] + WIN_WIDE / 2
                    hw, y0, y1, under = WIN_WIDE / 2, WIN_Y - WIN_HIGH / 2, WIN_Y + WIN_HIGH / 2, 4 * row
                    with b.at((w, y0, 0.1), (90, 0, 0)):
                        b.sheet((-hw, -0.1, hw, 0.1), "Wall", light=(0, under), extent=extent)
                    with b.at((w, y1, 0.1), (-90, 0, 0)):
                        b.sheet((-hw, -0.1, hw, 0.1), "Wall", light=(WIN_WIDE + gap, under), extent=extent)
                    with b.at((w - hw, WIN_Y, 0.1), (0, -90, 0)):
                        b.sheet((-0.1, -WIN_HIGH / 2, 0.1, WIN_HIGH / 2), "Wall", light=(2 * (WIN_WIDE + gap), under), extent=extent)
                    with b.at((w + hw, WIN_Y, 0.1), (0, 90, 0)):
                        b.sheet((-0.1, -WIN_HIGH / 2, 0.1, WIN_HIGH / 2), "Wall", light=(2 * (WIN_WIDE + gap) + 0.2 + gap, under), extent=extent)


# ---- furniture modelled here
def desk(b, at, rng):
    """A pedestal desk in walnut, its top's front edge rounded over, at `at` (the top's middle on
    the floor), its front towards -z of the frame: three drawers under the left of the top with
    turned brass knobs on a recessed plinth, a panel end at the right, a modesty board behind the
    knees. Gives its parts as boxes for leads to fall on, in this frame: the top first."""
    wide, deep = DESK
    t = DESK_TOP_T
    with b.at(at, (0, 180, 0)):   # (turned round: its drawers and knees towards the sitter at +z, its modesty board to the wall)
        b.pose("desk", (wide, TABLE_TOP, deep), at=(0, TABLE_TOP / 2, 0), rot=(0, 180, 0), name="Room desk")   # (its sitter's side is -z here)
        # the top: a slab rounded over at every edge, its corners rounded in plan
        top = TABLE_TOP
        with b.at((0, top, 0)):
            soft_top = toys.rounded(wide, deep, 0.02, n=4)
            with b.at((0, -t, 0)):
                toys.soft(b, soft_top, t, 0.008, "DeskWalnut", under=0.003, tile=1.8)   # (the walnut's own repeat)
        legs = top - t
        # the pedestal: a carcass of four boards on a plinth, three drawers in it
        px, pw = -wide / 2 + 0.23, 0.42
        b.box((px, 0.045, -0.01), (pw - 0.04, 0.09, deep - 0.08), "DeskWalnutSides", bevel=0.004, segs=1, grain=0)
        for sx in (-1, 1):
            b.box((px + sx * (pw / 2 - 0.0095), (legs + 0.09) / 2 + 0.0, 0), (0.019, legs - 0.09 + 0.001, deep - 0.02), "DeskWalnutSides", bevel=0.003, segs=1, grain=1)
        b.box((px, legs / 2 + 0.045, deep / 2 - 0.02), (pw - 0.038, legs - 0.09, 0.012), "DeskWalnutSides", grain=1)
        y = 0.095
        for i, h in enumerate((0.27, 0.19, 0.15)):
            front = (px, y + h / 2, -deep / 2 + 0.012)
            b.box(front, (pw - 0.044, h - 0.006, 0.02), "DeskWalnutSides", bevel=0.004, segs=2, grain=1)
            b.box((px, y + h / 2, front[2] + 0.01 + 0.33), (pw - 0.038, h - 0.04, 0.66), "DeskWalnutSides", grain=1)   # its box behind, running on the sides
            frame(b, (front[0], front[1], front[2] - 0.01), pw - 0.09, h - 0.05, [(0, 0), (0.008, 0), (0.008, 0.004), (0.004, 0.006), (0, 0.006)], "DeskWalnutSides")
            b.lathe((px, y + h / 2 + (0.03 if i == 0 else 0), -deep / 2 + 0.003), [(0.009, 0), (0.006, 0.006), (0.005, 0.014), (0.012, 0.022), (0.013, 0.028),
                                                                                 (0.01, 0.033), (0, 0.034)], "Brass", segs=16, rot=(-90, 0, 0))
            y += h + 0.004
        # the panel end at the right, on a plinth of its own, its front edge moulded
        ex = wide / 2 - 0.04
        b.box((ex, 0.03, 0), (0.05, 0.06, deep - 0.06), "DeskWalnutSides", bevel=0.006, segs=2, grain=0)
        b.box((ex, legs / 2 + 0.03, 0), (0.026, legs - 0.06, deep - 0.08), "DeskWalnutSides", bevel=0.004, segs=1, grain=1)
        with b.at((ex - 0.013, legs / 2 + 0.03, 0), (0, 90, 0)):   # (out of its face towards the knees: turned -90 it lay inside the panel)
            frame(b, (0, 0, 0), deep - 0.22, legs - 0.24, [(0, 0), (0.012, 0), (0.012, 0.004), (0.006, 0.007), (0, 0.007)], "DeskWalnutSides")
        # the board behind the knees, from the pedestal to the end
        kx0, kx1 = px + pw / 2, ex - 0.013
        b.box(((kx0 + kx1) / 2, legs - 0.2, deep / 2 - 0.08), (kx1 - kx0, 0.36, 0.018), "DeskWalnutSides", bevel=0.003, segs=1, grain=0)
        # a rail under the top's front between them
        b.box(((kx0 + kx1) / 2, legs - 0.03, -deep / 2 + 0.04), (kx1 - kx0, 0.06, 0.022), "DeskWalnutSides", bevel=0.004, segs=1, grain=0)
    x, z = at[0], at[2]   # (turned round: x and z of its own frame are -x and -z here)
    return [((x, TABLE_TOP - t / 2, z), (wide, t, deep), 0), ((x - px, legs / 2, z), (pw, legs, deep), 0),
            ((x - ex, legs / 2, z), (0.05, legs, deep), 0), ((x - (kx0 + kx1) / 2, legs - 0.2, z - deep / 2 + 0.08), (kx1 - kx0, 0.36, 0.02), 0)]


def bookcase(b, wide=1.2, high=1.95, deep=0.32, shelves=5):
    """A bookcase of the decade in walnut against a wall (the wall's own space, its middle at x 0):
    two sides on a recessed plinth, a moulded cornice over the top, a panelled back, shelves with
    a lipped front. Gives each shelf's height, from the lowest up, and its inside's half width."""
    t = 0.022
    b.pose("against", (wide, high, deep), at=(0, high / 2, -deep / 2), rot=(0, 180, 0), name="Bookcase", back=0.05, clear=0.6)
    b.box((0, high / 2, -0.007), (wide - 0.02, high - 0.02, 0.01), "WoodDark", grain=1)                         # the back
    for sx in (-1, 1):
        b.box((sx * (wide / 2 - t / 2), (high - t) / 2, -deep / 2), (t, high - t, deep), "WoodDark", bevel=0.003, segs=1, grain=1)   # (under the top: up through it, their tops and its ends flickered)
    b.box((0, 0.04, -deep / 2 + 0.015), (wide - 2 * t, 0.08, deep - 0.05), "WoodDark", bevel=0.004, segs=1, grain=0)   # the plinth, set back
    b.box((0, high - t / 2, -deep / 2), (wide, t, deep), "WoodDark", bevel=0.003, segs=1, grain=0)
    # the cornice: an ogee swept round the top's front and sides
    ogee = [(0, 0), (0.012, 0), (0.012, 0.008), (0.022, 0.014)] + [(0.022 + 0.014 * math.sin(math.radians(a)), 0.014 + 0.02 * (1 - math.cos(math.radians(a))))
                                                                     for a in range(0, 91, 15)] + [(0.036, 0.04), (0.04, 0.04), (0.04, 0.046), (0, 0.046)]
    b.sweep(ogee, [(wide / 2, high, 0), (wide / 2, high, -deep), (-wide / 2, high, -deep), (-wide / 2, high, 0)], "WoodDark", up=(0, 1, 0))
    levels = []
    step = (high - 0.1 - t) / shelves
    for s in range(shelves):
        y = 0.08 + s * step
        b.box((0, y + t / 2, -deep / 2 + 0.005), (wide - 2 * t, t, deep - 0.012), "WoodDark", bevel=0.003, segs=1, grain=0)
        b.box((0, y + t / 2 + 0.002, -deep + 0.012), (wide - 2 * t, t + 0.012, 0.014), "WoodDark", bevel=0.004, segs=2, grain=0)   # the lip
        levels.append(y + t)
    return levels, wide / 2 - t


def floor_lamp(b, at, shade="Shade"):
    """A brass standard lamp with a pleated-looking cone of a shade, its switch on the stem (the den's, wherever)."""
    with b.at(at):
        b.pose("floor", (0.4, 1.76, 0.4), at=(0, 0.88, 0), name="Floor lamp")
        b.lathe((0, 0, 0), [(0.16, 0), (0.165, 0.012), (0.13, 0.024), (0.03, 0.04), (0.016, 0.07), (0.013, 1.45), (0.02, 1.46), (0.02, 1.5), (0, 1.5)],
                "Brass", segs=28)
        b.lathe((0, 1.46, 0), [(0.20, 0), (0.204, 0), (0.134, 0.30), (0.13, 0.30), (0.20, 0)], shade, segs=32, cap=False)
        for i in range(3):
            a = math.radians(i * 120)
            b.tube([(0.02 * math.cos(a), 1.49, 0.02 * math.sin(a)), (0.13 * math.cos(a), 1.75, 0.13 * math.sin(a))], 0.003, "Brass", segs=6)
        b.tube([(0.013, 1.2, 0), (0.03, 1.19, 0), (0.03, 1.08, 0)], 0.002, "Brass", segs=5, bend=0.006)   # (a wire bent round 3 of its thicknesses)


# ---- what hangs on the walls: posters of the decade (world/posters.py fetches them into one atlas)
POSTERS = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "posters.json"))) \
    if os.path.exists(os.path.join(os.path.dirname(os.path.abspath(__file__)), "posters.json")) else {}


def poster(b, name, x, y, high, kind="frame", tilt=0.0):
    """A poster in a wall's own space, its middle at x, y, `high` tall at its own shape: in a thin
    black frame behind glass, or pinned straight to the wall with its corners lifting a little."""
    if name not in POSTERS:
        return
    item = POSTERS[name]
    u0, v0, u1, v1 = item["uv"]
    wide = high * item["aspect"]
    b.pose("hung", (wide + 0.04, high + 0.04, 0.024), at=(x, y, -0.012), rot=(0, 180, 0), name="Poster " + name)
    with b.at((x, y, 0), (0, 0, tilt)):
        if kind == "frame":
            b.box((0, 0, -0.006), (wide + 0.02, high + 0.02, 0.012), "PlasticDark", bevel=0.002, segs=1)
            moulding = [(0, 0), (0.016, 0), (0.016, 0.02), (0.012, 0.024), (0.004, 0.024), (0, 0.016)]
            frame(b, (0, 0, 0), wide + 0.02, high + 0.02, moulding, "MetalBlack")
            b.quad((0, 0, -0.0125), (wide, high), "Posters", uvrect=(u0, v0, u1, v1))
        else:
            # paper on the wall: a sheet bowed a hair off it in the middle, its corners curling out, a pin in each
            n = 6
            vertices, faces, uvs = [], [], []
            for j in range(n + 1):
                for i in range(n + 1):
                    s, t = i / n, j / n
                    lift = 0.0015 * math.sin(math.pi * s) * math.sin(math.pi * t) + 0.006 * (max(0, abs(2 * s - 1) - 0.8) * max(0, abs(2 * t - 1) - 0.8)) * 25
                    vertices.append(((s - 0.5) * wide, (t - 0.5) * high, -0.0012 - lift))
            for j in range(n):
                for i in range(n):
                    a = j * (n + 1) + i
                    faces.append((a, a + 1, a + n + 2, a + n + 1))
                    uvs.append([(u0 + (u1 - u0) * (k % (n + 1)) / n, v0 + (v1 - v0) * (k // (n + 1)) / n) for k in faces[-1]])
            b.mesh(vertices, faces, uvs, "Posters", normal=(0, 0, -1))
            for sx in (-1, 1):
                for sy in (-1, 1):
                    px, py = sx * (wide / 2 - 0.02), sy * (high / 2 - 0.02)
                    b.lathe((px, py, -0.0025), [(0.0045, 0), (0.0042, 0.0012), (0.0022, 0.0022), (0, 0.0024)], "GamepadRed" if sx * sy > 0 else "GamepadBlue",
                            segs=12, rot=(-90, 0, 0))


# ---- a room
def door(b, room):
    """The door from the corridor: a casing on the room's side too, the reveal, the leaf open back against the wall."""
    number, x0, x1, door_x, _, _ = room
    at, rot, _ = wall_frame(room, "north")
    with b.at(at, rot):
        with b.at((door_x - at[0], 0, 0)):
            b.obj("Room doors")
            w, h = DOOR_WIDE, DOOR_HIGH
            door_casing(b)
            reveal(b, -w / 2, w / 2, h, "WoodDark")
            with b.at((-w / 2, 0, -0.01), (0, 100, 0)):
                with b.at((w / 2, 0, 0)):
                    door_leaf(b, both=True)
        b.obj("Room things")
        # the light's switch and a socket by the door
        sx = door_x - at[0] + DOOR_WIDE / 2 + 0.25
        light_switch(b, sx, 1.25)


def window(b, room):
    """N15's window onto the city, as the corridor's: a casing, the sash, a sill, the pane, and a radiator under it."""
    _, x0, x1, _, _, _ = room
    at, rot, _ = wall_frame(room, "east")
    cx = to_wall(room, "east", x1, WIN_Z)
    w, y0, y1 = WIN_WIDE / 2, WIN_Y - WIN_HIGH / 2, WIN_Y + WIN_HIGH / 2
    with b.at(at, rot):
        b.obj("Room window")
        casing = [(0, 0), (0.085, 0), (0.085, 0.012), (0.07, 0.022), (0.012, 0.026), (0, 0.016)]
        b.sweep(casing, [(cx + w, y0, 0), (cx + w, y1, 0), (cx - w, y1, 0), (cx - w, y0, 0)], "TrimWhite", up=(0, 0, -1))
        sash = [(0, -0.03), (0, 0.03), (-0.05, 0.03), (-0.06, 0.02), (-0.06, -0.03)]
        frame(b, (cx, WIN_Y, 0.11), WIN_WIDE, WIN_HIGH, sash, "TrimWhite")
        b.box((cx, WIN_Y, 0.11), (0.05, WIN_HIGH - 0.1, 0.05), "TrimWhite", bevel=0.008, grain=1)
        b.box((cx, y0 - 0.016, -0.05), (WIN_WIDE + 0.24, 0.04, 0.25), "WoodOak", bevel=0.012, grain=0)
        b.box((cx, y0 - 0.066, -0.009), (WIN_WIDE + 0.16, 0.06, 0.018), "WoodOak", bevel=0.005, grain=0)
        b.obj("Window glass room")
        b.quad((cx, WIN_Y, 0.12), (WIN_WIDE, WIN_HIGH), "Glass")
        b.obj("Room things")
        radiator(b, cx, 0.36, 0.44)   # under it, low: the sofa stands before it


def radiator(b, x, y, high, fins=20):
    """A column radiator in a wall's own space, its middle at x, y and 75 mm off the wall: its back
    plate, its fins, the cap along its top, two brackets into the wall, and its pipes, each from its
    foot out past its end and down into the floor through a valve."""
    wide = fins * 0.06
    b.pose("hung", (wide, high + 0.02, 0.08), at=(x, y, -0.075), rot=(0, 180, 0), name="Radiator", back=0.05, clear=0.08)
    with b.at((x, y, -0.075)):
        b.box((0, 0, 0.03), (wide, high, 0.012), "TrimWhite", bevel=0.004)
        for i in range(fins):
            b.box((-wide / 2 + 0.03 + i * 0.06, 0, 0), (0.034, high + 0.02, 0.05), "TrimWhite", bevel=0.012, grain=1)
        b.box((0, high / 2 + 0.015, 0.005), (wide, 0.02, 0.07), "TrimWhite", bevel=0.006)
        for sx in (-1, 1):
            bx = sx * (wide / 2 - 0.12)
            b.lathe((bx, high * 0.3, 0.03), [(0, 0), (0.009, 0), (0.009, 0.04), (0.016, 0.04), (0.016, 0.0448), (0, 0.0448)], "TrimWhite", segs=12, rot=(90, 0, 0))   # a bracket to the wall, and its rose
            px = sx * (wide / 2 + 0.035)
            # (a 15 mm pipe, BS EN 1057, 7.5 mm round; its elbow an end-feed fitting's, a bend of about 20 mm round its middle,
            # a guess from makers' sheets: bent round 12 mm in three points its inside had folded through itself)
            bend, yb = 0.02, -high / 2 + 0.03
            arc = [(px - sx * bend + sx * bend * math.sin(math.pi / 2 * k / 8), yb - bend + bend * math.cos(math.pi / 2 * k / 8), 0) for k in range(9)]
            b.tube([(sx * (wide / 2 - 0.02), yb, 0)] + arc + [(px, -y, 0)], 0.0075, "MetalSteel", segs=10)
            b.lathe((px, -high / 2 - 0.04, 0), [(0, 0), (0.012, 0), (0.012, 0.03), (0.009, 0.034), (0.009, 0.05), (0.015, 0.052), (0.015, 0.062), (0, 0.062)], "Brass", segs=14)   # its valve
            b.lathe((px, -y, 0), [(0, 0), (0.022, 0), (0.022, 0.004), (0, 0.006)], "TrimWhite", segs=14)   # the floor's rose


def trim(b, room):
    """A skirting board round the room (broken by the doorway), a cove at the ceiling, a picture rail."""
    number, x0, x1, door_x, _, _ = room
    b.obj("Room trim")
    skirting = [(0, 0), (0.016, 0), (0.016, 0.07), (0.012, 0.078), (0.008, 0.086), (0.005, 0.09), (0, 0.09)]
    d0, d1 = door_x - DOOR_WIDE / 2 - 0.09, door_x + DOOR_WIDE / 2 + 0.09
    b.sweep(skirting, [(d1, 0, Z1), (x1, 0, Z1), (x1, 0, Z0), (x0, 0, Z0), (x0, 0, Z1), (d0, 0, Z1)], "WoodDark", up=(0, 1, 0))
    cove = [(0, 0), (0.05, 0), (0.05, -0.01), (0.03, -0.016), (0.014, -0.034), (0.01, -0.055), (0, -0.055)]
    b.sweep(cove, [(x0, HIGH, Z1), (x1, HIGH, Z1), (x1, HIGH, Z0), (x0, HIGH, Z0)], "TrimWhite", closed=True)
    rail = [(0, 0), (0.016, 0.004), (0.016, 0.026), (0, 0.032)]
    b.sweep(rail, [(x0, 2.3, Z1), (x1, 2.3, Z1), (x1, 2.3, Z0), (x0, 2.3, Z0)], "TrimWhite", closed=True)


SOFAS = {13: ("Sofa_01", 1.57), 14: ("sofa_02", 1.9), 15: ("sofa_03", 2.1)}
ROOM_POSTERS = {   # a room's: (wall, along it from the wall's middle, height of its middle, height, kind); "desk": over the desk
    13: [("east", 0.0, 1.62, 0.78, "frame"), ("north", -1.2, 1.6, 0.66, "pin"), ("south", "desk", 1.85, 0.6, "pin"), ("west", -1.05, 1.55, 0.7, "frame"),
         ("west", 1.3, 1.6, 0.66, "pin"), ("east", -1.45, 1.62, 0.66, "frame")],
    14: [("west", 0.0, 1.62, 0.78, "frame"), ("south", "desk", 1.85, 0.6, "pin"), ("east", -1.15, 1.58, 0.7, "pin"), ("north", -1.15, 1.6, 0.66, "frame"),
         ("east", 1.3, 1.6, 0.66, "frame")],
    15: [("north", 1.2, 1.62, 0.7, "frame"), ("south", "desk", 1.85, 0.6, "pin"), ("west", -1.05, 1.58, 0.7, "pin"), ("west", 1.3, 1.6, 0.66, "frame"),
         ("north", -1.2, 1.6, 0.66, "pin")],
}
# the den's and the corridor's (world/furniture.py and world/annex.py hang them): (wall, along, height of its middle, height, kind)
DEN_POSTERS = {"wargames_1983": ("right", 3.95, 1.68, 0.84, "frame"), "sneakers_1992": ("right", 4.95, 1.68, 0.84, "frame"),
               "jurassic_park_1993": ("right", -1.0, 1.7, 0.84, "frame"), "intel_pentium_pro_poster": ("left", -4.7, 1.85, 0.7, "frame"),
               "amiga500_ad": ("left", 2.25, 1.7, 0.8, "pin"), "tomb_raider_poster": ("back", -1.75, 1.95, 0.46, "frame")}
CORRIDOR_POSTERS = {"halflife_ad": ("den", -2.3, 1.6, 0.76, "frame"), "blood_money_poster": ("neighbours", -2.5, 1.55, 0.7, "pin")}
# the holodecks' corridor's, between its doors (world/annex.py's corridor_fittings): north wall from the den's end, then the south
HOLO_POSTERS = ["star_trek_first_contact_1996", "virtuosity_1995", "existenz_1999", "last_starfighter_1984", "total_recall_1990", "thirteenth_floor_1999"]


def furnish(b, room, posters_left, rng):
    """What stands in a room. Gives its place, its lights and its solids for Models/rooms.json."""
    number, x0, x1, door_x, couch_side, window_there = room
    cx = (x0 + x1) / 2
    shelf_side = -couch_side
    desk_x = cx + 0.15 * shelf_side
    desk_z = Z0 + DESK_OFF + DESK[1] / 2
    solids, lights = [], []

    # the sofa along the couch's wall, its cushions, a rug before it, a side table at its door end, a lamp at its far end
    couch_x = (x1 if couch_side > 0 else x0) - couch_side * COUCH_OFF[room[0]]   # (where its back would be)
    name, long = SOFAS[number]
    sofa_z = Z0 + 1.65
    b.obj("Room furniture")
    sofa = b.asset(name, (couch_x - couch_side * 0.47, 0, sofa_z), rot=(0, -90 * couch_side, 0), size=(long, None, None))
    solids.append(sorted((couch_x, couch_x - couch_side * 0.9)) + [0, 0.45] + [sofa_z - long / 2, sofa_z + long / 2])
    b.asset("throw_pillows_01", (couch_x - couch_side * 0.55, 0.42, sofa_z + 0.15), rot=(0, -90 * couch_side + rng.uniform(-15, 15), 0),
            size=(0.55, None, None), on=sofa, loose=True)
    rug_x = (couch_x - couch_side * 0.9 + desk_x) / 2 + couch_side * 0.1
    b.obj("Room rugs")
    if number == 14:
        b.quad((rug_x, 0.006, sofa_z), (2.0, 1.16), "Rug", rot=(90, 90, 0), uvrect=(0, 0, 1, 1))
    else:
        b.quad((rug_x, 0.006, sofa_z), (1.7, 1.7), "RoundRug", rot=(90, 0, 0), uvrect=(0, 0, 1, 1))
    b.obj("Room furniture")
    side = (couch_x - couch_side * 0.33, 0, sofa_z + long / 2 + 0.33)
    b.asset("side_table_01", side, rot=(0, rng.uniform(-8, 8), 0), size=(0.5, None, None))
    solids.append([side[0] - 0.25, side[0] + 0.25, 0, 0.5, side[2] - 0.25, side[2] + 0.25])
    lamp = (couch_x - couch_side * 0.3, 0, Z0 + 0.3)
    b.obj("Room lamps")
    floor_lamp(b, lamp)
    lights.append(("Floor", (lamp[0], 1.62, lamp[2]), (1.0, 0.72, 0.45), 1.3, 3.5))
    b.asset("modern_ceiling_lamp_01", (cx, HIGH - 0.5, (Z0 + Z1) / 2), size=(None, 0.5, None))
    lights.append(("Ceiling", (cx, HIGH - 0.45, (Z0 + Z1) / 2), (1.0, 0.84, 0.62), 0.9, 4.5))

    # the bookcase across from the sofa
    shelf_x = x0 if shelf_side < 0 else x1
    shelf_z = Z0 + 1.95
    wall = "west" if shelf_side < 0 else "east"
    at, rot, _ = wall_frame(room, wall)
    along = to_wall(room, wall, shelf_x, shelf_z)
    with b.at(at, rot):
        with b.at((along, 0, 0)):
            b.obj("Room shelf")
            levels, inside = bookcase(b)
            fill_shelves(b, number, levels, inside, rng)
    solids.append(sorted((shelf_x, shelf_x - shelf_side * 0.34)) + [0, 1.95, shelf_z - 0.6, shelf_z + 0.6])

    # the desk and its computer
    b.obj("Room furniture")
    parts = desk(b, (desk_x, 0, desk_z), rng)
    solids.append([desk_x - DESK[0] / 2, desk_x + DESK[0] / 2, 0, TABLE_TOP, desk_z - DESK[1] / 2, desk_z + DESK[1] / 2])
    place = computer(b, number, (desk_x, desk_z), parts, rng)
    b.obj("Room chairs")
    # (as the classroom's: its middle 0.45 before the desk's front edge, which is towards +z here, its seat facing the desk)
    with b.at((desk_x + 0.08 + rng.uniform(-0.05, 0.05), 0, desk_z + DESK[1] / 2 + 0.45 + rng.uniform(-0.05, 0.05)), (0, 180 + rng.uniform(-20, 20), 0)):
        pc.office_chair(b)
    # a lamp clamped to the desk's back edge at its right (the sitter's right is world -x: the place is turned
    # round), its arm over the keys. The file's own origin is 1 mm under the clamp's upper jaw, and the
    # screw's pad is 21 mm under it: at its own size it grips a top of DESK_TOP_T, 0.2 mm free either side.
    b.obj("Room furniture")
    corner = (desk_x - 0.5, TABLE_TOP + 0.0012, desk_z - DESK[1] / 2 + 0.028)   # (over the end panel's side, where the edge is free under)
    b.asset("desk_lamp_arm_01", corner, rot=(0, 180, 0), scale=1.0)
    lights.append(("Desk", (corner[0] - 0.1, TABLE_TOP + 0.5, corner[2] + 0.35), (1.0, 0.8, 0.55), 0.8, 2.2))

    # the walls
    trim(b, room)
    door(b, room)
    if window_there:
        window(b, room)
    b.obj("Room posters")
    for wall_name, along, y, high, kind in ROOM_POSTERS[number]:
        if along == "desk":
            along = -(desk_x - cx) - 0.2
        if posters_left:
            at, rot, _ = wall_frame(room, wall_name)
            with b.at(at, rot):
                poster(b, posters_left.pop(0), along, y, high, kind, tilt=rng.uniform(-1.2, 1.2) if kind == "pin" else 0.0)
    clutter(b, room, desk_x, desk_z, sofa_z, side, rng)
    return place, lights, solids


SHELF_THINGS = {   # a room's bookcase: shelf by shelf, from the bottom
    13: ["magazines", "games", "games", "toys", "games"],
    14: ["encyclopedia", "games", "magazines", "games", "toys"],
    15: ["games", "magazines", "games", "toys", "encyclopedia"],
}


def fill_shelves(b, number, levels, inside, rng):
    from furniture import MAGAZINES, SHELF_GAMES, SOFTWARE, pile, shelf_row, leaning_box
    games = SHELF_GAMES[(number - 13) * 13:] + SHELF_GAMES[:(number - 13) * 13]
    games = games[::-1] if number == 14 else games
    games = list(games) + list(SOFTWARE)
    magazines = list(MAGAZINES[(number - 13) * 5:] + MAGAZINES[:(number - 13) * 5])
    deep = 0.32
    front = -deep + 0.03
    for y, what in zip(levels, SHELF_THINGS[number]):
        b.obj("Books")
        if what == "games":
            end = shelf_row(b, games, -inside + 0.01, inside - 0.01, y, front, rng, most=rng.randint(12, 20))
        elif what == "magazines":
            for k, x in enumerate((-inside + 0.165, -inside + 0.49)):
                some = [magazines.pop(0) for _ in range(rng.randint(3, 6))]
                pile(b, some, (x, y, -deep / 2 + 0.005), [90 + rng.uniform(-6, 6) for _ in some])   # (across: 0.21 m deep, clear of the lip)
            shelf_row(b, games, -inside + 0.67, inside - 0.01, y, front, rng)
        elif what == "encyclopedia":
            b.asset("book_encyclopedia_set_01", (-inside + 0.3, y, -deep + 0.11), rot=(0, 180, 0), limit=20000)
            shelf_row(b, games, -inside + 0.6, inside - 0.01, y, front, rng)
        else:   # toys: a radio or a cassette player, the duck, the elephant, a camera, a game face out
            b.obj("Room shelf things")
            if number == 13:
                b.asset("portable_cassette_player", (-inside + 0.15, y, -0.12), rot=(0, 160, 0), parts=PLAYER, loose=True)
                b.asset("portable_cassette_player", (-inside + 0.25, y + 0.006, -0.24), rot=(-90, 5, 0), parts=TAPE, loose=True)   # its tape out, lying by it
                b.asset("rubber_duck_toy", (0.05, y, -0.14), rot=(0, 200, 0), size=(None, 0.12, None), loose=True)
                b.asset("vintage_video_camera", (inside - 0.15, y, -0.15), rot=(0, 210, 0), size=(None, 0.14, None), loose=True)
            elif number == 14:
                b.asset("carved_wooden_elephant", (-inside + 0.14, y, -0.13), rot=(0, 70, 0), size=(0.14, None, None), loose=True)
                b.asset("Camera_01", (0.08, y, -0.15), rot=(0, 195, 0), size=(0.16, None, None), loose=True)
                b.asset("standing_picture_frame_02", (inside - 0.15, y, -0.12), rot=(0, 280, 0), size=(None, 0.24, None), loose=True)
            else:
                b.asset("cassette_player", (-inside + 0.14, y + 0.022, -0.26), rot=(-90, 178, 0), size=(0.11, None, None), loose=True)   # (on its back, its window up: the file stands it 24 cm high and 5 deep; turned about its foot, here near the shelf's front, its length lies back towards +z, and its depth is half under its foot: 22 mm up)
                b.asset("mantel_clock_01", (0.16, y, -0.12), rot=(0, 180, 0), size=(0.3, None, None), loose=True)
                b.asset("rubber_duck_toy", (inside - 0.13, y, -0.14), rot=(0, 150, 0), size=(None, 0.1, None), loose=True)
            b.obj("Books")
            for i, game in enumerate(games[:1]):
                leaning_box(b, -inside + (0.43 if number == 15 else 0.4) + i * 0.25, y, -0.014, 0, 8, game)


def clutter(b, room, desk_x, desk_z, sofa_z, side, rng):
    """What lies about a room: on the desk, the side table, the floor; a game's box leaning on a wall."""
    from furniture import lying, pile, leaning_box, MAGAZINES
    number, x0, x1, door_x, couch_side, _ = room
    top = TABLE_TOP
    # the room's walls are the shell's (world/shell.py): what falls against them (world/settle.py) has them as boxes
    for at, size in (((x0 - 0.1, 1.4, (Z0 + Z1) / 2), (0.2, 2.8, DEEP)), ((x1 + 0.1, 1.4, (Z0 + Z1) / 2), (0.2, 2.8, DEEP)),
                     (((x0 + x1) / 2, 1.4, Z0 - 0.1), (x1 - x0, 2.8, 0.2)), (((x0 + x1) / 2, 1.4, Z1 + 0.1), (x1 - x0, 2.8, 0.2))):
        b.wall(at, size)
    b.obj("Room desk things")
    # on the desk, left of the monitor as the sitter sees it (world +x: the place faces -z), beyond the keyboard's end
    # (the monitor is -0.11..0.53 of desk_x, the keyboard -0.08..0.5 at +0.2..0.42, the tower and mat -0.37..-0.16)
    lx = desk_x + 0.68
    toys.mug(b, (lx, top, desk_z - 0.05), "Ceramic" if number != 14 else "RedPaint", turn=rng.uniform(0, 360))
    if number == 13:
        toys.joystick(b, (lx - 0.02, top, desk_z + 0.25), turn=180 + rng.uniform(-20, 20))
        toys.floppies(b, (desk_x - 0.68, top, desk_z - 0.33), 180 + rng.uniform(-10, 10), 4, rng)
        toys.pencil(b, (desk_x + 0.38, top, desk_z - 0.4), turn=rng.uniform(0, 180))
    elif number == 14:
        toys.puzzle_cube(b, (lx - 0.02, top, desk_z + 0.25), turn=rng.uniform(0, 90), twist=22)
        toys.cassette(b, (desk_x - 0.66, top, desk_z - 0.36), turn=180 + rng.uniform(-25, 25), label="GamepadYellow")
        toys.soda_can(b, (lx + 0.06, top, desk_z + 0.1), "coke")
    else:
        toys.floppies(b, (lx - 0.02, top, desk_z + 0.25), 180 + rng.uniform(-10, 10), 6, rng)
        toys.die(b, (desk_x - 0.66, top, desk_z - 0.38), turn=rng.uniform(0, 90))
        toys.die(b, (desk_x - 0.62, top, desk_z - 0.41), turn=rng.uniform(0, 90), mat="GamepadRed", pip="Ceramic")
        toys.pencil(b, (desk_x + 0.42, top, desk_z - 0.38), turn=rng.uniform(0, 180), mat="GamepadBlue")
    b.obj("Room desk things")
    games = ["doom2", "duke3d", "quake2", "warcraft2", "diablo", "simcity2000", "civ2", "monkey2", "tentacle", "fallout"]
    rng.shuffle(games)
    lying(b, games[0], (desk_x - 0.62, top, desk_z + 0.25), 180 + rng.uniform(-12, 12))
    # the side table: magazines, a can or a cassette player, the chess set in N14
    st = 0.505
    b.obj("Room things")
    if number == 14:
        b.asset("chess_set", (side[0], st, side[2]), rot=(0, rng.uniform(0, 30), 0), size=(0.36, None, None), limit=30000, loose=True)
    else:
        # (a print's top out from the room's middle, give or take 50 degrees: read from the room, world/check_placement.py's "reads")
        out = lambda x, z: math.degrees(math.atan2(x - (x0 + x1) / 2, z - (Z0 + Z1) / 2))
        pile(b, rng.sample(MAGAZINES, 2), (side[0] - 0.03, st, side[2] + 0.03), [out(side[0], side[2]) + rng.uniform(-50, 50) for _ in range(2)], drift=0.02)
        if number == 13:
            toys.soda_can(b, (side[0] + 0.12, st, side[2] - 0.12), "diet")
        else:
            b.asset("portable_cassette_player", (side[0] - 0.03, st + 0.03, side[2] + 0.03), rot=(-90, 30, 0), parts=PLAYER, loose=True)
            b.asset("portable_cassette_player", (side[0] + 0.12, st + 0.006, side[2] - 0.13), rot=(-90, 70, 0), parts=TAPE, loose=True)
    # on the floor: a game face up by the rug, a pile of magazines by the sofa, a ball in a corner, toys on the rug
    b.obj("Room floor things")
    couch_x = (x1 if couch_side > 0 else x0) - couch_side * COUCH_OFF[room[0]]   # (where its back would be)
    couch_front = couch_x - couch_side * 0.95
    out = lambda x, z: math.degrees(math.atan2(x - (x0 + x1) / 2, z - (Z0 + Z1) / 2))   # (read from the room, as on the side table)
    pile(b, rng.sample(MAGAZINES, 4), (couch_x - couch_side * 0.85, 0.0, side[2]), [out(couch_x - couch_side * 0.85, side[2]) + rng.uniform(-50, 50) for _ in range(4)], drift=0.04)
    lying(b, games[1], (couch_front - couch_side * 0.25, 0.009, sofa_z + 0.2), rng.uniform(0, 360))
    lying(b, games[2], (couch_front - couch_side * 0.35, 0.009, sofa_z - 0.15), rng.uniform(0, 360))
    corner = ((x0 + 0.25) if couch_side > 0 else (x1 - 0.25), Z1 - 0.3)
    if number == 13:
        b.asset("football", (corner[0], 0, corner[1]), rot=(0, 40, 0), size=(0.22, None, None), parts=("football_inflated",), loose=True)
        toys.yoyo(b, (couch_front - couch_side * 0.3, 0.008, sofa_z - 0.35), turn=rng.uniform(0, 360))
    elif number == 14:
        # (its file stands it upright: its length is its height) leaning into the corner
        b.asset("baseball_bat", (corner[0] + 0.08, 0, corner[1] + 0.1), rot=(4, 0, -3), size=(None, 0.86, None), loose=True)
        b.asset("baseball_01", (corner[0] + 0.08, 0, corner[1] - 0.12), size=(0.074, None, None), limit=2500, loose=True)
        toys.puzzle_cube(b, (couch_front - couch_side * 0.4, 0.008, sofa_z + 0.45), turn=rng.uniform(0, 90))
    else:
        # an old television on the floor in the corner by the door wall, its screen to the room's middle
        tv = (corner[0] + couch_side * 0.1, corner[1] - 0.05)
        facing = math.degrees(math.atan2((x0 + x1) / 2 - tv[0], (Z0 + Z1) / 2 - tv[1]))
        b.asset("Television_01", (tv[0], 0, tv[1]), rot=(0, facing, 0), size=(0.56, None, None), loose=True)
        toys.ball(b, (couch_front - couch_side * 0.35, 0.008, sofa_z + 0.5), 0.03, "GamepadRed")
        toys.die(b, (couch_front - couch_side * 0.45, 0.008, sofa_z + 0.3), turn=rng.uniform(0, 90))
    # a game leaning against the wall by the door
    b.obj("Room floor things")
    with b.at(*wall_frame(room, "north")[:2]):
        leaning_box(b, door_x - wall_frame(room, "north")[0][0] + couch_side * 0.85, 0.0, 0.0, 0, 9, games[3])


def computer(b, number, at_xz, parts, rng):
    """The place's computer on the desk, as the classroom's (world/furniture.py's classroom), its
    sitter at world +z (the place turned round), its network lead into a socket in the wall
    behind the desk. Gives its entry for Models/rooms.json."""
    from furniture import TABLE_TOP as CLASS_TOP
    assert abs(CLASS_TOP - TABLE_TOP) < 1e-6
    index = FIRST_PLACE + number - 13
    turn = 180 + rng.uniform(-2.5, 2.5)
    at = (at_xz[0] + 0.1 + rng.uniform(-0.01, 0.01), TABLE_TOP, at_xz[1] + rng.uniform(-0.01, 0.01))
    lie = {"keyboard": (rng.uniform(-0.01, 0.01), rng.uniform(-0.01, 0.01), rng.uniform(-2.5, 2.5)),
           "mat": (rng.uniform(-0.01, 0.01), rng.uniform(-0.01, 0.01), rng.uniform(-6, 6)),
           "mouse": (rng.uniform(-0.03, 0.03), rng.uniform(-0.03, 0.03), rng.uniform(-20, 20))}
    b.obj("Room furniture")
    b.asset("pc_station", at, rot=(0, turn, 0), scale=1.0, parts=("=pc_station",))
    b.obj("Station %d rest" % index, charted=True)
    pc.MODE = "low"
    world = lib.place(at, (0, turn, 0))
    here = world.inverted()
    desk_boxes = [(tuple(here @ Vector(centre)), size, 180 - turn) for centre, size, _ in parts]
    wall_z = (here @ Vector((at[0], 0, Z0))).z                     # the far wall, behind the table, in the place's frame
    socket = (pc.TOWER_X + 0.14, 0.32 - TABLE_TOP, wall_z - 0.012)
    def mains(x):
        # a socket on the wall's own plane at place x (the place is turned off it a little: a plate in the place's frame stood 2 mm off the wall)
        w = world @ Vector((x, socket[1], wall_z))
        return tuple(here @ Vector((w.x, w.y, Z0))), (0, 180 - turn, 0)
    with b.at(at, (0, turn, 0)):
        pc.station(b, "rest", lie, desk=desk_boxes, seed=index, stuck=random.Random(300 + index).randint(2, 4), lan=socket, mains=mains)
        # the network's socket in the wall: a white plate, its socket looking into the room
        b.obj("Room things")
        sx, sy, sz = socket
        b.box((sx, sy, wall_z - 0.006), (0.08, 0.08, 0.012), "TrimWhite", bevel=0.004, segs=2)
        with b.at((sx, sy, sz), (0, 180, 0)):
            pc.lan_socket(b, 0, 0, 0)
        # and the mains socket the strip's lead is plugged into, left of the strip
        with b.at(*pc.MAINS_AT[0]):
            wall_socket(b, 0, 0)
    pc.MODE = "full"
    return {"x": at[0], "z": at[2], "turn": turn, "keyboard": list(lie["keyboard"])}


def build(b):
    """Everything in the side rooms (Annex's part); writes Models/rooms.json."""
    rng = random.Random(1313)
    shown = [n for n in POSTERS if n not in DEN_POSTERS and n not in CORRIDOR_POSTERS and n not in HOLO_POSTERS]
    places, lights, solids = [], [], []
    for room in ROOMS:
        place, its_lights, its_solids = furnish(b, room, shown, rng)
        places.append(place)
        lights += [list(at) + list(colour) + [power, reach] for _, at, colour, power, reach in its_lights]
        solids += its_solids
    here = os.path.dirname(os.path.abspath(__file__))
    json.dump({"places": places, "first": FIRST_PLACE, "numbers": [r[0] for r in ROOMS],
               "rooms": [v for _, x0, x1, door, _, window in ROOMS for v in (x0, x1, door, 1 if window else 0)],
               "z0": Z0, "z1": Z1, "high": HIGH, "window": [WIN_Z, WIN_WIDE, WIN_Y, WIN_HIGH],
               "lights": [v for light in lights for v in light], "solids": [v for solid in solids for v in solid]},
              open(os.path.join(here, "..", "unity", "ShaderEmu", "Models", "rooms.json"), "w"), indent=1)
