# What is beyond the den's two doors: the holodecks, through the left wall, and the corridor
# behind the back wall, which is where a visitor arrives. The plain floors, ceilings and walls
# of all of it are world/shell.py's.
import json
import math
import os

from room import (DOOR_HIGH, DOOR_WIDE, DOOR_X, HALF_D, HALF_W, HOLO_HIGH, HOLO_WIDE, HOLO_Z, WALL_THICK,
                  door_casing, door_leaf, frame, wainscot)
from furniture import pot
import pc

# The holodecks (docs/world/holodeck.md): a corridor from the den's left doorway, as plain as the one
# by the lift, and eight rooms of grid off it, four a side, a seat in each. Room k is on the north side (towards +z) when k is
# even; a room's own frame has its door behind it and the room towards +z.
ROOMS = 8
ROOM, ROOM_HIGH = 3.6, 3.0
ROOM_DOOR_WIDE, ROOM_DOOR_HIGH = 1.2, 2.4
GRID = 0.6         # a panel of a room's grid: a room is 6 by 5 of them a wall, its doorway 2 by 4
MASK_IN = 0.02     # the mask's faces inside the grid's: in front of its bars (grid_bars), which stand 15 mm proud at most
HC_WIDE, HC_HIGH = 2.0, 2.8
HC_X1 = -HALF_W - WALL_THICK                 # its east end: the den's wall from behind
ROOM_PITCH = ROOM + WALL_THICK
HC_LONG = (ROOMS // 2) * ROOM_PITCH + WALL_THICK
HC_X0 = HC_X1 - HC_LONG
HC_Z0, HC_Z1 = HOLO_Z - HC_WIDE / 2, HOLO_Z + HC_WIDE / 2
HOLO_LAMPS = [HC_X1 - 1.2 - i * (HC_LONG - 2.4) / 3 for i in range(4)]
# what stands in the holodecks' corridor and stops a visitor: x0, x1, z0, z1 and the height of its top
# (as Blender measured the models standing there: the table's top is 0.761, the bench's seat 0.651)
# (both clear of the panelling, whose cap stands 51 mm out)
FURNITURE = {"table": [HC_X1 - 1.05, HC_X1 - 0.65, HC_Z0 + 0.05, HC_Z0 + 0.45, 0.761],
             "bench": [HC_X0 + 0.055, HC_X0 + 0.555, HOLO_Z - 0.59, HOLO_Z + 0.59, 0.651]}
# in a room's frame: the seat, the screen before it (its middle, wide, high), and the stand at
# the seat's right hand with the room's controls and the two controllers
SEAT_Z = 0.9
# The console at the seat's right hand, turned to the sitter: one sloped face with the room's
# keys below and its screen above, in one plane (a screen before the seat stood in the
# program's world). In the console's own frame the face begins at FACE_FOOT and rises at
# FACE_LEAN from the level; the keys' and the screen's middles are so far up it.
STAND = (0.62, SEAT_Z + 0.65)
CONSOLE_TURN, FACE_LEAN = 44.0, 58.0
FACE_FOOT, FACE_LONG, FACE_WIDE = (0.62, -0.14), 0.645, 0.424   # the foot's height and z; the face's length and width
KEYS_UP, KEYS = 0.155, (0.38, 0.28)
SCREEN_UP, SCREEN_SIZE = 0.465, (0.40, 0.30)


def console_point(up, across=0.0, off=0.0):
    """A point of the console's face in a room's frame: `up` the slope, `across` it, `off` it towards the sitter."""
    lean, turn = math.radians(FACE_LEAN), math.radians(CONSOLE_TURN)
    y = FACE_FOOT[0] + up * math.sin(lean) + off * math.cos(lean)
    z = FACE_FOOT[1] + up * math.cos(lean) - off * math.sin(lean)
    return (STAND[0] + across * math.cos(turn) + z * math.sin(turn), y, STAND[1] - across * math.sin(turn) + z * math.cos(turn))


SCREEN = console_point(SCREEN_UP, 0.0, 0.002) + SCREEN_SIZE
SCREEN_TURN = (90 - FACE_LEAN, CONSOLE_TURN)


def console(b):
    """The console in its own frame (its face looks towards -z and up), as a made thing: a
    weighted base plate with rounded corners, a flat stem that tapers and leans back, a hinge
    in a yoke at its top, and a head in two mouldings: a dark front with the keys' and the
    screen's openings, and a lighter shell behind it that swells to the hinge's boss."""
    rr, sk = pc.rrect, pc.loft
    lean = math.radians(FACE_LEAN)
    mid = (0, FACE_FOOT[0] + FACE_LONG / 2 * math.sin(lean), FACE_FOOT[1] + FACE_LONG / 2 * math.cos(lean))
    w, long = FACE_WIDE, FACE_LONG
    hub_up, hub_in = -0.06, 0.062                  # the hinge behind the head: along the face from its middle, and behind the face
    hub = (mid[1] + hub_up * math.sin(lean) - hub_in * math.cos(lean), mid[2] + hub_up * math.cos(lean) + hub_in * math.sin(lean))   # its height and z here

    with b.at((0, 0, 0), (-90, 0, 0)):   # outlines in the floor's plane, stacked upwards: here +z is up and +y towards the sitter
        # the base: a plate with a chamfered edge, a shallow step, and a collar where the stem stands
        cz = -0.09
        sk(b, [pc.at_z(rr(0.30, 0.36, 0.07, 0, cz), 0.0), pc.at_z(rr(0.30, 0.36, 0.07, 0, cz), 0.010), pc.at_z(rr(0.288, 0.348, 0.064, 0, cz), 0.016),
               pc.at_z(rr(0.20, 0.25, 0.05, 0, cz), 0.019), pc.at_z(rr(0.19, 0.24, 0.045, 0, cz), 0.024)], "RackSteel", first=True, last=True)
        sk(b, [pc.at_z(rr(0.10, 0.07, 0.022, 0, -0.07), 0.024), pc.at_z(rr(0.092, 0.062, 0.02, 0, -0.07), 0.04),
               pc.at_z(rr(0.078, 0.05, 0.017, 0, -0.07), 0.046)], "MetalSteel", first=True, last=True)
        # the stem: a flat bar with rounded edges, narrower as it rises and leaning back to the hinge
        rings = []
        for t in (0.0, 0.25, 0.5, 0.75, 1.0):
            y = 0.04 + t * (hub[0] - 0.035 - 0.04)
            z = 0.07 + t * t * (hub[1] - 0.07)
            rings.append(pc.at_z(rr(0.07 - 0.02 * t, 0.034 - 0.008 * t, 0.011, 0, -z), y))
        sk(b, rings, "SciTrim", first=True, last=True)
    # the yoke and the hinge: two cheeks either side of a barrel across the stem's top
    for side in (-1, 1):
        b.box((side * 0.034, hub[0] - 0.012, hub[1]), (0.008, 0.07, 0.044), "SciTrim", bevel=0.004, segs=2)
        b.cyl((side * 0.041, hub[0], hub[1]), 0.011, 0.006, "MetalSteel", segs=20, bevel=0.0015, rot=(0, 0, 90))
    b.cyl((0, hub[0], hub[1]), 0.017, 0.058, "MetalSteel", segs=24, bevel=0.002, rot=(0, 0, 90))

    with b.at(mid, (90 - FACE_LEAN, 0, 0)):   # here x is across the face, y up it, and +z into the console
        r = 0.022
        # the front moulding: a chamfer round the face, then straight sides to a seam
        sk(b, [pc.at_z(rr(w - 0.006, long - 0.006, r - 0.003), 0.0), pc.at_z(rr(w, long, r), 0.003), pc.at_z(rr(w, long, r), 0.013)], "RackSteel", first=True, last=True)
        sk(b, [pc.at_z(rr(w - 0.005, long - 0.005, r - 0.002), 0.013), pc.at_z(rr(w - 0.005, long - 0.005, r - 0.002), 0.0155)], "MetalBlack", first=True, last=True)
        # the shell behind the seam: full size, drawn in, then swelling to the boss the hinge holds
        sk(b, [pc.at_z(rr(w, long, r), 0.0155), pc.at_z(rr(w, long, r), 0.021), pc.at_z(rr(w - 0.03, long - 0.03, r), 0.03),
               pc.at_z(rr(w * 0.62, long * 0.5, 0.05, 0, hub_up), 0.041), pc.at_z(rr(0.16, 0.13, 0.04, 0, hub_up), 0.05),
               pc.at_z(rr(0.11, 0.09, 0.03, 0, hub_up), 0.054)], "SciPanel", first=True, last=True)
        b.box((0, hub_up, 0.058), (0.05, 0.05, 0.016), "SciTrim", bevel=0.005, segs=2)            # the boss
        # vents in the shell's upper part, and a lit line under the keys
        for i in range(5):
            b.box((0, long * 0.28 + i * 0.012, 0.0335 - i * 0.0006), (w * 0.42, 0.004, 0.003), "MetalBlack", bevel=0.001, segs=1)
        b.box((0, -long / 2 + 0.006, -0.0004), (w * 0.5, 0.0025, 0.002), "Amber", bevel=0.0008, segs=1)
        # the two openings: a fine step round each, let into the face
        rim = [(0, 0), (0.004, 0), (0.004, 0.0015), (0.0015, 0.0025), (0, 0.0025)]
        for up, (rw_, rh) in ((KEYS_UP, KEYS), (SCREEN_UP, SCREEN_SIZE)):
            frame(b, (0, up - long / 2, -0.0002), rw_, rh, rim, "MetalBlack")
    # a lead from the head's boss down the back of the stem into the base
    b.tube([(0.012, hub[0] + 0.02, hub[1] + 0.03), (0.014, hub[0] - 0.08, hub[1] + 0.035), (0.012, 0.45, 0.115), (0.01, 0.12, 0.105), (0.01, 0.03, 0.12), (0.0, 0.012, 0.19)],
           0.0035, "Rubber", segs=6, smooth=4)


def room_place(k):
    """Where room k's frame is: its door's middle on the floor, at the room's own side of the wall, and its turn."""
    x = HC_X1 - WALL_THICK - ROOM / 2 - (k // 2) * ROOM_PITCH
    return (x, HC_Z1 + WALL_THICK, 0) if k % 2 == 0 else (x, HC_Z0 - WALL_THICK, 180)


def grid_shell(b, mat, inset):
    """A room's six faces in its own frame, seen from inside, `inset` nearer its middle: what
    marks where a program's world may show (ShaderEmuHolodeck.cs moves it to the room in use)."""
    s, t, i = ROOM, ROOM_HIGH, inset
    half = s / 2 - i
    door = [(-ROOM_DOOR_WIDE / 2, -1, ROOM_DOOR_WIDE / 2, ROOM_DOOR_HIGH)]
    with b.at((0, 0, i), (0, 180, 0)):
        b.sheet((-half, i, half, t - i), mat, door)
    with b.at((0, 0, s - i)):
        b.sheet((-half, i, half, t - i), mat)
    with b.at((-half, 0, s / 2), (0, -90, 0)):
        b.sheet((-half, i, half, t - i), mat)
    with b.at((half, 0, s / 2), (0, 90, 0)):
        b.sheet((-half, i, half, t - i), mat)
    with b.at((0, i, s / 2), (90, 0, 0)):
        b.sheet((-half, -half, half, half), mat)
    with b.at((0, t - i, s / 2), (-90, 0, 0)):
        b.sheet((-half, -half, half, half), mat)


def holo_room(b, k):
    """What stands in room k: the doorway's casing and the room's name plate on the corridor's
    side, the grid's bars, the seat, and the console at its right hand."""
    x, z, turn = room_place(k)
    w, h = ROOM_DOOR_WIDE, ROOM_DOOR_HIGH
    with b.at((x, 0, z), (0, turn, 0)):
        b.obj("Holo doors")
        with b.at((0, 0, -WALL_THICK)):   # the corridor's face of the wall, the corridor towards -z, as a wall's own space
            door_casing(b, w, h)
            # where the room's name plate is (ShaderEmuHolodeck.cs puts its words there): a board
            b.box((w / 2 + 0.42, 1.45, -0.006), (0.36, 0.22, 0.012), "WoodDark", bevel=0.004, segs=2, grain=0)   # (its face 2 mm behind the words)
        b.obj("Holo grid")
        grid_bars(b)
        b.obj("Holo seats")
        with b.at((0, 0, SEAT_Z), (0, 0, 0)):
            command_chair(b)
        b.obj("Holo consoles")   # (an object of their own: with the seats they were a mesh Unity's unwrapper gave up on)
        with b.at((STAND[0], 0, STAND[1]), (0, CONSOLE_TURN, 0)):
            console(b)


def feet():
    """What stands on a room's floor, as outlines in its frame (x, z): the seat's round foot and
    the console's base. The floor's lines stop short of them (grid_bars), not under them."""
    import lib
    from mathutils import Vector
    chair = [(0.245 * math.cos(a), SEAT_Z + 0.245 * math.sin(a)) for a in (i * math.pi / 24 for i in range(48))]
    frame = lib.place((STAND[0], 0, STAND[1]), (0, CONSOLE_TURN, 0)) @ lib.place((0, 0, 0), (-90, 0, 0))
    base = [frame @ Vector((x, y, 0)) for x, y in pc.rrect(0.30, 0.36, 0.07, 0, -0.09)]
    return [chair, [(p.x, p.z) for p in base]]


def cut_line(along, across, half, a0, a1, outlines, gap=0.003):
    """The pieces of a line at `across` (of half width `half`) from a0 to a1 along `along` (0 x,
    1 z) that are clear of every outline by `gap`."""
    pieces = [(a0, a1)]
    for outline in outlines:
        inside = []
        for p, q in zip(outline, outline[1:] + outline[:1]):   # the outline's edges, finely
            for k in range(20):
                point = [p[i] + (q[i] - p[i]) * k / 20 for i in (0, 1)]
                if abs(point[1 - along] - across) <= half + gap:
                    inside.append(point[along])
        if not inside:
            continue
        low, high = min(inside) - gap, max(inside) + gap
        pieces = [piece for a, c in pieces for piece in ((a, min(c, low)), (max(a, high), c)) if piece[1] - piece[0] > 0.002]
    return pieces


def less(a0, a1, cuts):
    """What is left of a0 to a1 without the intervals `cuts`."""
    pieces = [(a0, a1)]
    for lo, hi in cuts:
        pieces = [piece for a, e in pieces for piece in ((a, min(e, lo)), (max(a, hi), e)) if piece[1] - piece[0] > 0.002]
    return pieces


def grid_bars(b):
    """The grid's lines in a room's frame: a lit bar along every line between two panels of the
    shell (world/shell.py's holo_room), and a heavier one in each of the room's twelve edges, so
    that no panel ends at a corner unframed. No two bars go into each other or share a face's
    plane where they meet: an edge's bar lies in the corner, against both faces, the upright ones
    the room's height and the level ones between them; a line ends against the bar it meets (the
    walls' level lines between the upright ones, the floor's and the ceiling's across between those
    along), and the floor's stop short of the seat's foot and the console's base. The walls' bars
    stand 8 mm proud (those across 7), the floor's 3 and 2, the ceiling's 8 and 7, the corners' 15:
    the mask that lets the program's world through is in front of all of them (MASK_IN)."""
    s, t, g = ROOM, ROOM_HIGH, GRID
    n, m = round(s / g), round(t / g)
    dw, dh = round(ROOM_DOOR_WIDE / g), round(ROOM_DOOR_HIGH / g)
    d0 = (n - dw) // 2
    bar, mat = 0.022, "HoloLine"
    hw = bar / 2
    c = 0.015      # an edge's bar: a square this deep into the room from each of its two faces
    floor_proud, floor_across = 0.003, 0.002
    walls = [((s / 2, 0.0), (-1, 0), (0, 1)), ((-s / 2, 0.0), (0, 1), (1, 0)), ((-s / 2, s), (1, 0), (0, -1)), ((s / 2, s), (0, -1), (-1, 0))]

    def block(lo, hi, bevel=0.002):
        b.box(tuple((p + q) / 2 for p, q in zip(lo, hi)), tuple(q - p for p, q in zip(lo, hi)), mat, bevel=bevel, segs=1)

    def run(p, q, inward, proud, wide=bar):
        """A bar from p to q (a line on a face), standing `proud` off it towards `inward`."""
        lo, hi = [], []
        for a, e, i in zip(p, q, inward):
            if abs(i) > 0.5:
                lo.append(min(a, a + i * proud))
                hi.append(max(a, a + i * proud))
            elif abs(e - a) < 1e-9:
                lo.append(a - wide / 2)
                hi.append(a + wide / 2)
            else:
                lo.append(min(a, e))
                hi.append(max(a, e))
        block(lo, hi)
    # the twelve edges
    for x0 in (-s / 2, s / 2):
        for z0 in (0.0, s):
            ix, iz = (1 if x0 < 0 else -1), (1 if z0 == 0 else -1)
            block((min(x0, x0 + ix * c), 0.0, min(z0, z0 + iz * c)), (max(x0, x0 + ix * c), t, max(z0, z0 + iz * c)), 0.003)
    door_half = dw * g / 2
    for y0, y1 in ((0.0, c), (t - c, t)):
        for x0 in (-s / 2, s / 2):   # along z, between the upright ones
            block((min(x0, x0 + (c if x0 < 0 else -c)), y0, c), (max(x0, x0 + (c if x0 < 0 else -c)), y1, s - c), 0.003)
        block((-s / 2 + c, y0, s - c), (s / 2 - c, y1, s), 0.003)   # the far wall's
        if y0 > 0:
            block((-s / 2 + c, y0, 0.0), (s / 2 - c, y1, c), 0.003)   # the door wall's, under the ceiling
        else:   # either side of the doorway, to the floor's line at its jamb
            for k in (-1, 1):
                a, e = sorted((k * (s / 2 - c), k * (door_half + hw)))
                block((a, y0, 0.0), (e, y1, c), 0.003)
    # the walls' lines: the upright ones whole, the level ones between them
    for w, ((sx, sz), (ax, az), (ix, iz)) in enumerate(walls):
        at = lambda a, y: (sx + ax * a, y, sz + az * a)
        uprights = []
        for i in range(1, n):
            low = c
            if w == 0 and i in (d0, d0 + dw):
                low = floor_proud          # a jamb's: on the floor's line along the doorway's side
            elif w == 0 and d0 < i < d0 + dw:
                low = dh * g - hw          # over the doorway: from the head's line
            uprights.append((i * g, low))
            run(at(i * g, low), at(i * g, t - c), (ix, 0, iz), 0.008)
        for j in range(1, m):
            y = j * g
            spans = [(c, s - c)] if w != 0 or j >= dh else [(c, d0 * g), ((d0 + dw) * g, s - c)]
            for a0, a1 in spans:
                for a, e in less(a0, a1, [(a - hw, a + hw) for a, low in uprights if low <= y - hw + 1e-6]):
                    run(at(a, y), at(e, y), (ix, 0, iz), 0.007)
    # the floor's lines and the ceiling's: along z whole (but where something stands), across between them
    for y, up, proud, across, outlines in ((0.0, 1, floor_proud, floor_across, feet()), (t, -1, 0.008, 0.007, [])):
        laid = {}
        for i in range(1, n):
            x = -s / 2 + i * g
            z0 = 0.0 if up > 0 and abs(x) <= door_half + 1e-6 else c
            laid[x] = cut_line(1, x, hw, z0, s - c, outlines)
            for a, e in laid[x]:
                run((x, y, a), (x, y, e), (0, up, 0), proud)
        for j in range(1, n):
            z = j * g
            crossing = [(x - hw, x + hw) for x, pieces in laid.items() if any(a - hw <= z <= e + hw for a, e in pieces)]
            for a0, a1 in less(-s / 2 + c, s / 2 - c, crossing):
                for a, e in cut_line(0, z, hw, a0, a1, outlines):
                    run((a, y, z), (e, y, z), (0, up, 0), across)


def corridor_fittings(b):
    """The holodecks' corridor as the one by the lift: oak panelling to the hand's height, a cove
    under the ceiling, a casing round every doorway, lamps hanging on rods, and a few things
    along its walls (where they stand is FURNITURE: ShaderEmuHolodeck.cs stops visitors there)."""
    high, wide, long = HC_HIGH, HC_WIDE, HC_LONG
    mid_x = (HC_X0 + HC_X1) / 2
    b.obj("Holo corridor trim")
    half = long / 2
    for side, (z, turn) in enumerate(((HC_Z1, 0), (HC_Z0, 180))):
        with b.at((mid_x, 0, z), (0, turn, 0)):
            doors = sorted((room_place(k)[0] - mid_x) * (1 if turn == 0 else -1) for k in range(side, ROOMS, 2))
            edges = [-half] + [x + q * (ROOM_DOOR_WIDE / 2 + 0.09) for x in doors for q in (-1, 1)] + [half]
            for i in range(0, len(edges), 2):
                wainscot(b, edges[i], edges[i + 1])
    with b.at((HC_X0, 0, HOLO_Z), (0, -90, 0)):   # the far end
        wainscot(b, -wide / 2, wide / 2, (True, True))   # (against the side walls' in the corners)
    with b.at((HC_X1, 0, HOLO_Z), (0, 90, 0)):    # the den's wall from behind: its doorway in a casing
        wainscot(b, -wide / 2, -HOLO_WIDE / 2 - 0.09, (True, False))
        wainscot(b, HOLO_WIDE / 2 + 0.09, wide / 2, (False, True))
        door_casing(b, HOLO_WIDE, HOLO_HIGH)
    cove = [(0, 0), (0.05, 0), (0.05, -0.01), (0.03, -0.016), (0.014, -0.034), (0.01, -0.055), (0, -0.055)]
    b.sweep(cove, [(HC_X0, high, HC_Z1), (HC_X1, high, HC_Z1), (HC_X1, high, HC_Z0), (HC_X0, high, HC_Z0)], "TrimWhite", closed=True)

    for x in HOLO_LAMPS:   # as the lift's corridor: hanging lamps, drawn up short of a tall visitor's head
        b.asset("modern_ceiling_lamp_01", (x, high - 0.6, HOLO_Z), size=(None, 0.6, None))
    b.obj("Holo corridor things")
    # between the doors, a picture each side; by the den's end a side table with magazines, at the far end a bench
    # (no plant: one is 30 to 38 thousand triangles)
    gaps = [HC_X1 - WALL_THICK - (i + 1) * ROOM_PITCH + WALL_THICK / 2 for i in range(ROOMS // 2 - 1)]   # between two rooms' doors
    for i, x in enumerate(gaps):
        with b.at((x, 0, HC_Z1)):
            b.asset("hanging_picture_frame_0%d" % (1 + i % 2), (0, 1.45, -0.02), rot=(0, 180, 0))
        with b.at((x, 0, HC_Z0), (0, 180, 0)):
            b.asset("hanging_picture_frame_0%d" % (2 - i % 2), (0, 1.45, -0.02), rot=(0, 180, 0))
    (tx0, tx1, tz0, tz1), (bx0, bx1, bz0, bz1) = FURNITURE["table"][:4], FURNITURE["bench"][:4]
    table = b.asset("side_table_tall_01", ((tx0 + tx1) / 2, 0, (tz0 + tz1) / 2), rot=(0, 0, 0))
    bench = b.asset("painted_wooden_bench", ((bx0 + bx1) / 2, 0, (bz0 + bz1) / 2), rot=(0, 90, 0))
    import furniture
    # (a magazine's top towards the wall: away from whoever stands before it; a game lying on them)
    furniture.pile(b, ["computer_gaming_world_issue_148", "bytevolume20number12", "pc_computing_1998_05", "earthworm_jim"],
                   ((tx0 + tx1) / 2, FURNITURE["table"][4] + 0.001, (tz0 + tz1) / 2), [furniture.TOP_AWAY + 180 + t for t in (-8, 5, 11, -4)])
    # on the bench at the far end, their tops towards its wall (turned so, they had read upside down from the corridor)
    furniture.pile(b, ["computer_gaming_world_issue_116"], ((bx0 + bx1) / 2 + 0.075, FURNITURE["bench"][4] + 0.001, (bz0 + bz1) / 2 - 0.35), (furniture.TOP_AWAY + 262,))
    furniture.pile(b, ["linuxjournalissue58", "bytevol21no11"], ((bx0 + bx1) / 2 + 0.075, FURNITURE["bench"][4] + 0.001, (bz0 + bz1) / 2 + 0.3),
                   (furniture.TOP_AWAY + 275, furniture.TOP_AWAY + 263))   # (clear of its back, which leans over the seat's far edge)


def holodecks(b):
    corridor_fittings(b)
    # what marks where the program's world may show: one room's faces, inside the grid's bars
    b.obj("Holodeck mask")
    grid_shell(b, "HoloMask", MASK_IN)
    for k in range(ROOMS):
        holo_room(b, k)
    here = os.path.dirname(os.path.abspath(__file__))
    # for ShaderEmuHolodeck.cs, beside the models (rooms: x, z and turn of each, one after another;
    # furniture: x0, x1, z0, z1 and height of each thing a visitor walks round)
    json.dump({"rooms": [v for k in range(ROOMS) for v in room_place(k)], "room": [ROOM, ROOM_HIGH], "door": [ROOM_DOOR_WIDE, ROOM_DOOR_HIGH],
               "corridor": [HC_X0, HC_X1, HC_Z0, HC_Z1, HC_HIGH], "seat": SEAT_Z, "screen": list(SCREEN), "screenTurn": list(SCREEN_TURN), "panel": list(console_point(KEYS_UP, 0.0, 0.004)),
               "stand": list(STAND), "lamps": HOLO_LAMPS, "furniture": [v for name in ("table", "bench") for v in FURNITURE[name]]},
              open(os.path.join(here, "..", "unity", "ShaderEmu", "Models", "holodeck.json"), "w"), indent=1)


def command_chair(b):
    """A starship's command chair at the frame's origin, its seat's front towards +z: a pedestal
    on a round foot, a shell that wraps the sitter's back and sides, dark cushions, and wide
    arms that end in small consoles."""
    # a low foot and a slender column, with a small plate under the seat's pan
    b.lathe((0, 0, 0), [(0, 0), (0.24, 0), (0.245, 0.008), (0.21, 0.022), (0.08, 0.038), (0.055, 0.06), (0, 0.06)], "SciTrim", segs=40)
    b.cyl((0, 0.064, 0), 0.06, 0.008, "MetalSteel", segs=28, bevel=0.002)   # on the foot; the column on it, up to the pan's plate
    b.lathe((0, 0.068, 0), [(0.04, 0), (0.036, 0.1), (0.036, 0.222), (0.05, 0.262), (0.13, 0.272), (0.13, 0.292), (0, 0.292)], "RackSteel", segs=28)
    # the pan and the shell: a base under the seat, sides up to the arms, a back that leans
    b.box((0, 0.41, 0.0), (0.60, 0.06, 0.56), "SciPanel", bevel=0.025, segs=4)
    b.box((0, 0.375, 0.0), (0.5, 0.03, 0.46), "SciTrim", bevel=0.012, segs=2)
    for side in (-1, 1):
        b.box((side * 0.305, 0.54, -0.02), (0.075, 0.24, 0.52), "SciPanel", bevel=0.03, segs=4)
        # the arm: a pad the length of the forearm, and a console at its end, tipped to the hand
        b.box((side * 0.315, 0.672, 0.0), (0.11, 0.035, 0.46), "SciSeat", bevel=0.016, segs=4)
        with b.at((side * 0.315, 0.672, 0.27), (22, 0, 0)):
            b.box((0, 0.0, 0.0), (0.125, 0.04, 0.13), "SciTrim", bevel=0.012, segs=3)
            b.box((0, 0.021, 0.0), (0.105, 0.003, 0.11), "MetalBlack", bevel=0.001, segs=1)
            for row in range(3):
                for column in range(3):
                    mat = "Amber" if (row + column + (side > 0)) % 3 == 0 else "LampLow" if (row * 2 + column) % 4 == 1 else "GamepadBlue"
                    b.box(((column - 1) * 0.03, 0.0235, (row - 1) * 0.03), (0.022, 0.002, 0.018), mat, bevel=0.0008, segs=1)
    with b.at((0, 0.44, -0.265), (-11, 0, 0)):
        b.box((0, 0.36, -0.03), (0.60, 0.74, 0.07), "SciPanel", bevel=0.03, segs=4)
        b.box((0, 0.755, -0.03), (0.42, 0.06, 0.075), "SciTrim", bevel=0.02, segs=3)     # the cap along its top
        b.box((0, 0.32, 0.03), (0.44, 0.56, 0.075), "SciSeat", bevel=0.034, segs=5)      # the back's cushion
        b.box((0, 0.66, 0.03), (0.30, 0.10, 0.07), "SciSeat", bevel=0.03, segs=4)        # and the head's
        for side in (-1, 1):   # a lamp let into the shell's back edge, either side
            b.box((side * 0.275, 0.36, -0.03), (0.012, 0.5, 0.03), "Amber", bevel=0.004, segs=1)
    b.box((0, 0.485, 0.01), (0.50, 0.09, 0.48), "SciSeat", bevel=0.04, segs=5)           # the seat's cushion
    # a foot rail before the pedestal
    b.tube([(-0.15, 0.03, 0.17), (-0.14, 0.11, 0.33), (0.14, 0.11, 0.33), (0.15, 0.03, 0.17)], 0.011, "MetalSteel", segs=10, smooth=4)


# the corridor: along the back wall's far side, from the lift at its west end to past the den's door
COR_X0, COR_X1 = -6.0, HALF_W   # its east end is the building's outer wall, as the den's right wall is
COR_Z1 = -HALF_D - WALL_THICK
COR_WIDE, COR_HIGH = 2.4, 2.8
COR_Z0 = COR_Z1 - COR_WIDE
COR_LONG, COR_MID_X, COR_MID_Z = COR_X1 - COR_X0, (COR_X0 + COR_X1) / 2, (COR_Z0 + COR_Z1) / 2
WIN_WIDE, WIN_HIGH, WIN_Y = 1.5, 1.3, 1.75
LAMPS_X = (-3.9, -0.4, 3.1)


def corridor_door(b, x, name=None):
    """A neighbour's door, shut, in a wall's own space."""
    with b.at((x, 0, 0)):
        door_casing(b)
        door_leaf(b)
        b.box((0, 1.62, -0.054), (0.09, 0.06, 0.004), "Brass", bevel=0.002, segs=1)   # its number, on the middle stile
        b.quad((0, 0.008, -0.38), (0.7, 0.42), "RoundRug", rot=(90, 0, 0), uvrect=(0.3, 0.3, 0.7, 0.54))   # a mat


def on(b, asset, x, y, deep, off=0.0, **more):
    """One of Poly Haven's models against a wall in the wall's own space: its back to the wall,
    its foot at y; deep is how far it stands out. off: how far its back is from the wall (clear
    of the panelling, whose skirting stands 32 mm out)."""
    return b.asset(asset, (x, y, -deep / 2 - off), rot=(0, 180, 0), **more)


def corridor(b):
    door_at = DOOR_X - COR_MID_X                               # the den's door, in the north wall's space
    with b.at((COR_X1, 0, COR_MID_Z), (0, 90, 0)):   # the outer wall: a window onto the city
        w, y0, y1 = WIN_WIDE / 2, WIN_Y - WIN_HIGH / 2, WIN_Y + WIN_HIGH / 2
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
        wainscot(b, -COR_WIDE / 2, COR_WIDE / 2, (True, True))   # (against the side walls' in the corners)
        b.obj("Corridor radiator")
        with b.at((0, 0.44, -0.075)):   # a radiator under the window
            b.box((0, 0, 0.03), (1.2, 0.54, 0.012), "TrimWhite", bevel=0.004)
            for i in range(20):
                b.box((-0.57 + i * 0.06, 0, 0), (0.034, 0.56, 0.05), "TrimWhite", bevel=0.012, grain=1)
            b.box((0, 0.285, 0.005), (1.2, 0.02, 0.07), "TrimWhite", bevel=0.006)
        b.obj("Corridor trim")
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
        # the back between the jambs, the jambs under the head, the doors on the back
        b.box((0, 1.07, -0.01), (1.24, 2.14, 0.02), "RackSteel")
        for k in (-1, 1):
            b.box((k * 0.3, 1.04, -0.026), (0.59, 2.06, 0.012), "MetalSteel", bevel=0.003, segs=1, grain=1)
            b.box((k * 0.67, 1.07, -0.04), (0.1, 2.14, 0.08), "MetalSteel", bevel=0.008, grain=1)
        b.box((0, 2.2, -0.04), (1.44, 0.12, 0.08), "MetalSteel", bevel=0.008, grain=0)
        b.box((0, 2.36, -0.012), (0.36, 0.1, 0.024), "PlasticDark", bevel=0.004, segs=1)
        b.box((0, 2.36, -0.026), (0.2, 0.05, 0.004), "Amber")                      # the floor it is at
        b.box((0.92, 1.15, -0.008), (0.1, 0.2, 0.016), "MetalSteel", bevel=0.004, segs=1)
        for y, mat in ((1.19, "Amber"), (1.11, "PlasticDark")):
            b.cyl((0.92, y, -0.02), 0.016, 0.008, mat, segs=14, rot=(90, 0, 0))

    b.obj("Corridor things")
    with b.at((COR_MID_X, 0, COR_Z1)):   # the den's wall
        on(b, "painted_wooden_bench", -1.2, 0, 0.5, off=0.052)   # (its back clear of the panelling's cap)
        on(b, "rubber_boots", -0.3, 0, 0.3, off=0.035, parts=("rubber_boots_l", "rubber_boots_r"))
        on(b, "korean_fire_extinguisher_01", door_at - 1.0, 0, 0.38, off=0.035)
        on(b, "fire_alarm", door_at - 1.0, 1.35, 0.03)
        on(b, "korean_public_payphone_01", -3.2, 1.0, 0.3)
        on(b, "vintage_suitcase", -4.3, 0, 0.26, off=0.035, parts=("vintage_suitcase_01",))
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
        on(b, "hanging_picture_frame_01", -1.6, 0.93, 0.02)   # (over the panelling's cap)
        table = on(b, "side_table_tall_01", 2.15, 0, 0.38, off=0.05)
        on(b, "wicker_basket_01", 2.15, 0.76, 0.34, off=0.05, on=table)
        on(b, "ornate_mirror_01", 2.15, 1.1, 0.03)
    b.obj("Pots")
    spot = (COR_X1 - 0.5, 0, COR_Z0 + 0.5)   # by the window, clear of the panelling
    pot(b, spot, 0.19)
    b.asset("pachira_aquatica_01", (spot[0], 0.33, spot[2]), rot=(0, 110, 0), parts=("_c",))
    low = b.asset("cardboard_box_01", (COR_X0 + 0.5, 0, COR_Z0 + 0.4), rot=(0, 12, 0), size=(0.5, 0.42, 0.5), limit=5000)
    b.asset("cardboard_box_01", (COR_X0 + 0.52, 0.42, COR_Z0 + 0.42), rot=(0, -20, 0), size=(0.4, 0.3, 0.4), limit=5000, on=low)   # (let down on its flaps)


def build(b):
    holodecks(b)
    corridor(b)
