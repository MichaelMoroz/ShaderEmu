# What is beyond the den's two doors: the holodecks, through the left wall, and the corridor
# behind the back wall, which is where a visitor arrives. The plain floors, ceilings and walls
# of all of it are world/shell.py's.
import json
import math
import os

from room import (DOOR_HIGH, DOOR_WIDE, DOOR_X, HALF_D, HALF_W, HOLO_HIGH, HOLO_WIDE, HOLO_Z, WALL_THICK,
                  door_casing, door_leaf, frame, wainscot)
from furniture import pot, PLAYER, TAPE
import pc
import rooms

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
FURNITURE = {"table": [HC_X1 - 1.25, HC_X1 - 0.85, HC_Z0 + 0.02, HC_Z0 + 0.42, 0.761],
             "bench": [HC_X0 + 0.02, HC_X0 + 0.52, HOLO_Z - 0.59, HOLO_Z + 0.59, 0.651]}
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
    b.pose("front", (0.45, 1.2, 0.45), at=(0, 0.6, 0), rot=(0, 180, 0), name="Holo console", clear=0.3)   # (its face to the seat)
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
        b.box((0, hub_up, 0.066), (0.05, 0.05, 0.032), "SciTrim", bevel=0.005, segs=2)            # the boss, round the hinge's barrel
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
    shell (world/shell.py's holo_room), and a heavier one along each of the room's twelve edges,
    so that no panel ends at a corner unframed. The walls' bars stand 8 mm proud (those across
    7, so that no two faces of a crossing lie in one plane), the floor's 3 and 2, the corners'
    15: the mask that lets the program's world through is in front of all of them (MASK_IN)."""
    s, t, g = ROOM, ROOM_HIGH, GRID
    n, m = round(s / g), round(t / g)
    dw, dh = round(ROOM_DOOR_WIDE / g), round(ROOM_DOOR_HIGH / g)
    d0 = (n - dw) // 2
    bar, mat, edge = 0.022, "HoloLine", 0.03   # (edge: the square bars along the room's twelve edges)
    walls = [((s / 2, 0.0), (-1, 0), (0, 1)), ((-s / 2, 0.0), (0, 1), (1, 0)), ((-s / 2, s), (1, 0), (0, -1)), ((s / 2, s), (0, -1), (-1, 0))]

    def run(p, q, inward, proud, wide=bar, off=0.0):
        """A bar from p to q (a line on a face), standing `proud` off it towards `inward`, its back
        `off` from the face."""
        mid = [(a + c) / 2 + i * (proud + off) / 2 for a, c, i in zip(p, q, inward)]
        size = [abs(c - a) + (wide if abs(c - a) < 1e-6 else 0) for a, c in zip(p, q)]
        size = [proud - off if abs(i) > 0.5 else v for v, i in zip(size, inward)]
        b.box(tuple(mid), tuple(size), mat, bevel=0.002, segs=1)
    for w, ((sx, sz), (ax, az), (ix, iz)) in enumerate(walls):
        at = lambda a, y: (sx + ax * a, y, sz + az * a)
        for i in range(1, n):   # upright lines
            low = dh * g if w == 0 and d0 < i < d0 + dw else 0.0
            run(at(i * g, low), at(i * g, t), (ix, 0, iz), 0.008)
        for j in range(1, m):   # level lines
            spans = [(0.0, s)] if w != 0 or j >= dh else [(0.0, d0 * g), ((d0 + dw) * g, s)]
            for a, c in spans:   # (from corner bar to corner bar: into the corner, two walls' bars crossed in one plane)
                a, c = (edge / 2 if a == 0.0 else a), (s - edge / 2 if c == s else c)
                run(at(a, j * g), at(c, j * g), (ix, 0, iz), 0.007, off=0.0005)   # (its back off the wall: at a crossing the two backs lay in one plane)
    for i in range(1, n):   # the floor's lines, and the ceiling's, from edge bar to edge bar
        for y, up, proud in ((0.0, 1, 0.003), (t, -1, 0.008)):
            # (2 mm narrower than the walls' bars: where one met an upright at a wall, their sides lay in one plane)
            run((-s / 2 + i * g, y, edge / 2), (-s / 2 + i * g, y, s - edge / 2), (0, up, 0), proud, wide=bar - 0.002)
            run((-s / 2 + edge / 2, y, i * g), (s / 2 - edge / 2, y, i * g), (0, up, 0), proud - 0.001, wide=bar - 0.002)
    # the twelve edges: a square bar along each, half on either face (the door wall's floor edge either side of the doorway)
    for (x0, z0) in ((-s / 2, 0.0), (s / 2, 0.0), (-s / 2, s), (s / 2, s)):
        b.box((x0, t / 2, z0), (edge, t - edge, edge), mat, bevel=0.003, segs=1)   # (butting the floor's and the ceiling's: three bars in one corner had faces in common)
    for y in (0.0, t):   # those along z run into the corners, those along x butt them
        b.box((-s / 2, y, s / 2), (edge, edge, s), mat, bevel=0.003, segs=1)
        b.box((s / 2, y, s / 2), (edge, edge, s), mat, bevel=0.003, segs=1)
        b.box((0.0, y, s), (s - edge, edge, edge), mat, bevel=0.003, segs=1)
    b.box((0.0, t, 0.0), (s - edge, edge, edge), mat, bevel=0.003, segs=1)
    side = (s - dw * g) / 2
    for k in (-1, 1):   # (from the corner's bar to a millimetre short of the doorway's jambs)
        outer, inner = s / 2 - edge / 2, s / 2 - side + 0.001
        b.box((k * (outer + inner) / 2, 0.0, 0.0), (outer - inner, edge, edge), mat, bevel=0.003, segs=1)


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
        wainscot(b, -wide / 2, wide / 2, scribe=(True, True))   # (the long walls' run into the corners)
    with b.at((HC_X1, 0, HOLO_Z), (0, 90, 0)):    # the den's wall from behind: its doorway in a casing
        wainscot(b, -wide / 2, -HOLO_WIDE / 2 - 0.09, scribe=(True, False))
        wainscot(b, HOLO_WIDE / 2 + 0.09, wide / 2, scribe=(False, True))
        door_casing(b, HOLO_WIDE, HOLO_HIGH)
    cove = [(0, 0), (0.05, 0), (0.05, -0.01), (0.03, -0.016), (0.014, -0.034), (0.01, -0.055), (0, -0.055)]
    b.sweep(cove, [(HC_X0, high, HC_Z1), (HC_X1, high, HC_Z1), (HC_X1, high, HC_Z0), (HC_X0, high, HC_Z0)], "TrimWhite", closed=True)

    for x in HOLO_LAMPS:   # as the lift's corridor: hanging lamps, drawn up short of a tall visitor's head
        b.asset("modern_ceiling_lamp_01", (x, high - 0.6, HOLO_Z), size=(None, 0.6, None))
    b.obj("Holo corridor things")
    # between the doors, a film's poster each side, each its own (world/rooms.py's HOLO_POSTERS); by the den's end a side
    # table with magazines, at the far end a bench (no plant: one is 30 to 38 thousand triangles)
    gaps = [HC_X1 - WALL_THICK - (i + 1) * ROOM_PITCH + WALL_THICK / 2 for i in range(ROOMS // 2 - 1)]   # between two rooms' doors
    b.obj("Holo corridor posters")
    names = iter(rooms.HOLO_POSTERS)
    for frame_at in [((x, 0, HC_Z1), (0, 0, 0)) for x in gaps] + [((x, 0, HC_Z0), (0, 180, 0)) for x in gaps]:
        with b.at(*frame_at):
            rooms.poster(b, next(names), 0, 1.58, 0.82, "frame")
    b.obj("Holo corridor things")
    (tx0, tx1, tz0, tz1), (bx0, bx1, bz0, bz1) = FURNITURE["table"][:4], FURNITURE["bench"][:4]
    table = b.asset("side_table_tall_01", ((tx0 + tx1) / 2, 0, (tz0 + tz1) / 2), rot=(0, 0, 0))
    bench = b.asset("painted_wooden_bench", ((bx0 + bx1) / 2, 0, (bz0 + bz1) / 2), rot=(0, 90, 0))
    import furniture
    furniture.pile(b, ["computer_gaming_world_issue_148", "bytevolume20number12", "pc_computing_1998_05"],
                   ((tx0 + tx1) / 2 - 0.02, FURNITURE["table"][4], (tz0 + tz1) / 2 + 0.01), (172, 185, 201))   # (their print to the trim behind: read from the corridor)
    furniture.pile(b, ["computer_gaming_world_issue_116"], ((bx0 + bx1) / 2 + 0.05, FURNITURE["bench"][4], (bz0 + bz1) / 2 - 0.35), (262,))
    furniture.pile(b, ["linuxjournalissue58", "bytevol21no11"], ((bx0 + bx1) / 2 - 0.02, FURNITURE["bench"][4], (bz0 + bz1) / 2 + 0.3), (275, 250))


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
    b.pose("front", (0.8, 1.3, 0.75), at=(0, 0.65, 0), name="Holo seat", clear=0.8)   # (towards the holodeck's room)
    # a low foot and a slender column, with a small plate under the seat's pan
    b.lathe((0, 0, 0), [(0, 0), (0.24, 0), (0.245, 0.008), (0.21, 0.022), (0.08, 0.038), (0.055, 0.06), (0, 0.06)], "SciTrim", segs=40)
    b.cyl((0, 0.064, 0), 0.06, 0.008, "MetalSteel", segs=28, bevel=0.002)
    b.lathe((0, 0.06, 0), [(0.04, 0.001), (0.036, 0.1), (0.036, 0.24), (0.05, 0.29), (0.13, 0.31), (0.13, 0.329), (0, 0.329)], "RackSteel", segs=28)   # (its ends a millimetre inside the plate and the trim: flush, they flickered)
    # the pan and the shell: a base under the seat, sides up to the arms, a back that leans
    b.box((0, 0.41, 0.0), (0.60, 0.06, 0.56), "SciPanel", bevel=0.025, segs=4)
    b.box((0, 0.375, 0.0), (0.5, 0.03, 0.46), "SciTrim", bevel=0.012, segs=2)
    for side in (-1, 1):
        b.box((side * 0.305, 0.54, -0.02), (0.075, 0.24, 0.52), "SciPanel", bevel=0.03, segs=4)
        # the arm: a pad the length of the forearm, and a console at its end, tipped to the hand
        b.box((side * 0.315, 0.672, 0.0), (0.11, 0.035, 0.46), "SciSeat", bevel=0.016, segs=4)
        b.obj("Holo consoles")   # (the arms' consoles with the room's: in the seats' mesh they took it past the 65,536 vertices Unity's unwrapper keeps to)
        # (resting on the pad's rounded end, its underside where the pad's 16 mm bevel turns 22 degrees: at (0.672, 0.27)
        # it stood 15 mm into the pad; with the seat's other parts in one mesh that went unseen)
        with b.at((side * 0.315, 0.6908, 0.2678), (22, 0, 0)):
            b.box((0, 0.0, 0.0), (0.125, 0.04, 0.13), "SciTrim", bevel=0.012, segs=3)
            b.box((0, 0.021, 0.0), (0.105, 0.003, 0.11), "MetalBlack", bevel=0.001, segs=1)
            for row in range(3):
                for column in range(3):
                    mat = "Amber" if (row + column + (side > 0)) % 3 == 0 else "LampLow" if (row * 2 + column) % 4 == 1 else "GamepadBlue"
                    b.box(((column - 1) * 0.03, 0.0235, (row - 1) * 0.03), (0.022, 0.002, 0.018), mat, bevel=0.0008, segs=1)
        b.obj("Holo seats")
    with b.at((0, 0.44, -0.265), (-11, 0, 0)):
        b.box((0, 0.36, -0.03), (0.60, 0.74, 0.07), "SciPanel", bevel=0.03, segs=4)
        b.box((0, 0.755, -0.03), (0.42, 0.06, 0.075), "SciTrim", bevel=0.02, segs=3)     # the cap along its top
        b.box((0, 0.32, 0.03), (0.44, 0.56, 0.075), "SciSeat", bevel=0.034, segs=5)      # the back's cushion
        b.box((0, 0.66, 0.03), (0.30, 0.10, 0.07), "SciSeat", bevel=0.03, segs=4)        # and the head's
        for side in (-1, 1):   # a lamp let into the shell's back edge, either side
            b.box((side * 0.299, 0.36, -0.03), (0.012, 0.5, 0.03), "Amber", bevel=0.004, segs=1)
    b.box((0, 0.485, 0.01), (0.50, 0.09, 0.48), "SciSeat", bevel=0.04, segs=5)           # the seat's cushion
    # a foot rail before the pedestal
    b.tube([(-0.15, 0.012, 0.17), (-0.14, 0.11, 0.33), (0.14, 0.11, 0.33), (0.15, 0.012, 0.17)], 0.011, "MetalSteel", segs=10, bend=0.045)   # (bent round 2 diameters; smoothed through its corners they were tighter, its picture 1.6 times)


# the corridor: along the back wall's far side, from the lift at its west end to past the den's door
COR_X0, COR_X1 = -6.0, HALF_W   # its east end is the building's outer wall, as the den's right wall is
COR_Z1 = -HALF_D - WALL_THICK
COR_WIDE, COR_HIGH = 2.4, 2.8
COR_Z0 = COR_Z1 - COR_WIDE
COR_LONG, COR_MID_X, COR_MID_Z = COR_X1 - COR_X0, (COR_X0 + COR_X1) / 2, (COR_Z0 + COR_Z1) / 2
WIN_WIDE, WIN_HIGH, WIN_Y = 1.5, 1.3, 1.75
LAMPS_X = (-3.9, -0.4, 3.1)
assert (rooms.COR_Z0, rooms.COR_HIGH, rooms.COR_MID_X) == (COR_Z0, COR_HIGH, COR_MID_X)   # (world/rooms.py's copy)


def corridor_door(b, x, number):
    """A side room's doorway in a wall's own space (its door stands open inside, world/rooms.py),
    its number on a brass plate beside it, a mat before it."""
    with b.at((x, 0, 0)):
        door_casing(b)
        with b.at((DOOR_WIDE / 2 + 0.2, 1.55, 0)):   # the plate: brass, its number engraved (RoomSigns), two screws
            b.box((0, 0, -0.003), (0.13, 0.065, 0.006), "Brass", bevel=0.0015, segs=2)
            k = number - 13
            b.quad((0, 0, -0.0066), (0.12, 0.055), "RoomSigns", uvrect=(0, 1 - (k + 1) / 4, 1, 1 - k / 4))
            for sx in (-1, 1):
                b.lathe((sx * 0.055, 0, -0.006), [(0.0028, 0), (0.0024, 0.0008), (0, 0.0012)], "Brass", segs=10, rot=(-90, 0, 0))
        b.quad((0, 0.008, -0.38), (0.7, 0.42), "RoundRug", rot=(90, 0, 0), uvrect=(0.3, 0.3, 0.7, 0.54))   # a mat


PANEL_OUT = 0.034   # how far the panelling's skirting stands out of a wall (world/room.py's wainscot: its rail 0.052, at 0.885 to 0.915)


def on(b, asset, x, y, deep, **more):
    """One of Poly Haven's models against a wall in the wall's own space: its back to the wall,
    its foot at y; deep is how far it stands out. What stands on the floor stands clear of the
    panelling's skirting; what hangs has its back on the wall, whatever the model's depth."""
    if y < 0.85:
        return b.asset(asset, (x, y, -deep / 2 - PANEL_OUT), rot=(0, 180, 0), **more)
    return b.asset(asset, (x, y, -deep / 2), rot=(0, 180, 0), back_at=deep / 2, **more)


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
    doors = rooms.corridor_doors()   # the side rooms', in the south wall's space
    with b.at((COR_MID_X, 0, COR_Z0), (0, 180, 0)):
        edges = [-half] + [x + k * (DOOR_WIDE / 2 + 0.09) for x in sorted(doors) for k in (-1, 1)] + [half]
        for i in range(0, len(edges), 2):
            wainscot(b, edges[i], edges[i + 1])
    with b.at((COR_X1, 0, COR_MID_Z), (0, 90, 0)):
        wainscot(b, -COR_WIDE / 2, COR_WIDE / 2, scribe=(True, True))   # (the long walls' run into the corners)
        rooms.radiator(b, 0, 0.44, 0.54)   # under the window
    cove = [(0, 0), (0.05, 0), (0.05, -0.01), (0.03, -0.016), (0.014, -0.034), (0.01, -0.055), (0, -0.055)]
    b.sweep(cove, [(COR_X0, COR_HIGH, COR_Z1), (COR_X1, COR_HIGH, COR_Z1), (COR_X1, COR_HIGH, COR_Z0), (COR_X0, COR_HIGH, COR_Z0)],
            "TrimWhite", closed=True)

    b.obj("Corridor doors")
    with b.at((COR_MID_X, 0, COR_Z0), (0, 180, 0)):
        for x, room in zip(doors, rooms.ROOMS):
            corridor_door(b, x, room[0])

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
            b.box((k * 0.67, 1.07, -0.04), (0.1, 2.14, 0.08), "MetalSteel", bevel=0.008, grain=1)   # (up to the header, not into it)
        b.box((0, 2.2, -0.04), (1.436, 0.12, 0.08), "MetalSteel", bevel=0.008, grain=0)        # (its ends 2 mm inside the jambs': flush, they flickered)
        b.box((0, 2.36, -0.012), (0.36, 0.1, 0.024), "PlasticDark", bevel=0.004, segs=1)
        b.box((0, 2.36, -0.026), (0.2, 0.05, 0.004), "Amber")                      # the floor it is at
        b.box((0.92, 1.15, -0.008), (0.1, 0.2, 0.016), "MetalSteel", bevel=0.004, segs=1)
        for y, mat in ((1.19, "Amber"), (1.11, "PlasticDark")):
            b.cyl((0.92, y, -0.02), 0.016, 0.008, mat, segs=14, rot=(90, 0, 0))

    b.obj("Corridor things")
    with b.at((COR_MID_X, 0, COR_Z1)):   # the den's wall
        bench = on(b, "painted_wooden_bench", -1.2, 0, 0.5)
        on(b, "portable_cassette_player", -1.45, 0.652, 0.18, parts=PLAYER, loose=True)   # left on the bench
        b.asset("portable_cassette_player", (-1.29, 0.658, -0.3 - PANEL_OUT), rot=(-90, 165, 0), parts=TAPE, loose=True)   # and its tape
        on(b, "rubber_boots", -0.3, 0, 0.3, parts=("rubber_boots_l", "rubber_boots_r"))
        on(b, "korean_fire_extinguisher_01", door_at - 1.0, 0, 0.38)
        on(b, "fire_alarm", door_at - 1.0, 1.35, 0.03)
        on(b, "korean_public_payphone_01", -3.2, 1.0, 0.3)
        on(b, "vintage_suitcase", -4.3, 0, 0.26, parts=("vintage_suitcase_01",))
        on(b, "hanging_picture_frame_02", 1.45, 1.45, 0.04)
        # a notice board over the bench
        with b.at((-1.2, 1.65, 0)):
            b.pose("hung", (1.04, 0.74, 0.03), at=(0, 0, -0.015), rot=(0, 180, 0), name="Notice board")
            b.box((0, 0, -0.008), (1.0, 0.7, 0.016), "Cardboard")
            moulding = [(0, 0), (0.03, 0), (0.03, 0.018), (0.02, 0.026), (0.006, 0.026), (0, 0.016)]
            frame(b, (0, 0, 0), 0.98, 0.68, moulding, "WoodOak")
            for i, (x, y, turn) in enumerate(((-0.3, 0.12, 4), (0.05, 0.15, -6), (0.3, -0.05, 3), (-0.15, -0.14, -3))):
                b.quad((x, y, -0.0165), (0.12, 0.12), "Details", rot=(0, 0, turn), decal="note_%d" % (i % 3))
            b.quad((0.28, 0.2, -0.0167), (0.21, 0.26), "Paper", rot=(0, 0, -2))
    b.obj("Corridor posters")   # (world/posters.py's, as the den's)
    for name, (wall, along, y, high, kind) in rooms.CORRIDOR_POSTERS.items():
        with b.at((COR_MID_X, 0, COR_Z1) if wall == "den" else (COR_MID_X, 0, COR_Z0), (0, 0, 0) if wall == "den" else (0, 180, 0)):
            rooms.poster(b, name, along, y, high, kind, tilt=0.8 if kind == "pin" else 0.0)
    b.obj("Corridor things")
    with b.at((COR_MID_X, 0, COR_Z0), (0, 180, 0)):   # the neighbours' wall
        on(b, "hanging_picture_frame_01", -1.6, 0.95, 0.02)   # (over the panelling's rail)
        on(b, "side_table_tall_01", 2.15, 0, 0.38)
        on(b, "wicker_basket_01", 2.15, 0.76, 0.34)
        on(b, "ornate_mirror_01", 2.15, 1.1, 0.03)
    b.obj("Pots")
    spot = (COR_X1 - 0.5, 0, COR_Z0 + 0.5)   # (its leaves clear of the panelling)   # by the window
    pot(b, spot, 0.19)
    b.asset("pachira_aquatica_01", (spot[0], 0.33, spot[2]), rot=(0, 150, 0), parts=("_c",))
    low = b.asset("cardboard_box_01", (COR_X0 + 0.5, 0, COR_Z0 + 0.4), rot=(0, 12, 0), size=(0.5, 0.42, 0.5), limit=5000)
    b.asset("cardboard_box_01", (COR_X0 + 0.52, 0.42, COR_Z0 + 0.42), rot=(0, -20, 0), size=(0.4, 0.3, 0.4), limit=5000, on=low)   # (let down on its flaps)


def build(b):
    holodecks(b)
    corridor(b)
    rooms.build(b)
