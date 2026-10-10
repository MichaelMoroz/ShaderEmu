# The room's shell: floor, walls, panelling, mouldings, beams, the window, the door, the lamps.
import math

HALF_W, HALF_D, HEIGHT = 4.5, 5.5, 3.2
WINDOW_WIDE, WINDOW_HIGH, WINDOW_Y, WINDOW_Z = 3.6, 1.5, 1.75, -1.6
DOOR_X, DOOR_WIDE, DOOR_HIGH = 3.4, 0.95, 2.1
# the holodeck's doorway, in the left wall where the volume display hung (world/annex.py is beyond it)
HOLO_Z, HOLO_WIDE, HOLO_HIGH = 3.9, 1.3, 2.3
WALL_THICK = 0.2

# a wall's own space: x along it, y up, the room towards -z (as a screen is seen)
WALLS = {
    "front": ((0, 0, HALF_D), (0, 0, 0), HALF_W),
    "back": ((0, 0, -HALF_D), (0, 180, 0), HALF_W),
    "left": ((-HALF_W, 0, 0), (0, -90, 0), HALF_D),
    "right": ((HALF_W, 0, 0), (0, 90, 0), HALF_D),
}


def on_wall(b, wall):
    at, rot, _ = WALLS[wall]
    return b.at(at, rot)


def frame(b, centre, wide, high, profile, mat):
    """A mitred frame round a rectangle in the local xy plane; profile is (outward, towards the viewer)."""
    w, h = wide / 2, high / 2
    path = [(centre[0] - w, centre[1] - h, centre[2]), (centre[0] + w, centre[1] - h, centre[2]),
            (centre[0] + w, centre[1] + h, centre[2]), (centre[0] - w, centre[1] + h, centre[2])]
    b.sweep(profile, path, mat, up=(0, 0, -1), closed=True)


def wall_with_door(b, length, height, x0, x1, top, mat):
    """A wall in its own space with a doorway from x0 to x1, `top` high: three pieces, their
    picture laid as one quad's would be."""
    from lib import MATERIALS
    tu, tv = MATERIALS[mat][0]
    half = length / 2
    vertices, faces, uvs = [], [], []
    for ax, ay, bx, by in ((-half, 0, x0, height), (x1, 0, half, height), (x0, top, x1, height)):
        first = len(vertices)
        vertices += [(ax, ay, 0), (bx, ay, 0), (bx, by, 0), (ax, by, 0)]
        faces.append((first, first + 1, first + 2, first + 3))
        uvs.append([(x / tu, (y - height / 2) / tv) for x, y in ((ax, ay), (bx, ay), (bx, by), (ax, by))])
    b.mesh(vertices, faces, uvs, mat, normal=(0, 0, -1))


def reveal(b, x0, x1, top, mat, deep=WALL_THICK, sill=None):
    """The inside of a doorway through a wall `deep` thick: two jambs, a head and a threshold."""
    b.quad((x0, top / 2, deep / 2), (deep, top), mat, rot=(0, -90, 0))
    b.quad((x1, top / 2, deep / 2), (deep, top), mat, rot=(0, 90, 0))
    b.quad(((x0 + x1) / 2, top, deep / 2), (x1 - x0, deep), mat, rot=(-90, 0, 0))
    b.quad(((x0 + x1) / 2, 0, deep / 2), (x1 - x0, deep), sill or mat, rot=(90, 0, 0))


def wainscot(b, x0, x1):
    """Frame-and-panel oak to 0.92 m along a wall, from x0 to x1 in the wall's space."""
    length, mid = x1 - x0, (x0 + x1) / 2
    b.quad((mid, 0.45, -0.006), (length, 0.9), "WoodDark")
    b.box((mid, 0.055, -0.019), (length, 0.11, 0.026), "WoodDark", bevel=0.006, grain=0)
    b.box((mid, 0.83, -0.017), (length, 0.10, 0.022), "WoodDark", bevel=0.003, grain=0)
    b.box((mid, 0.9, -0.026), (length, 0.03, 0.05), "WoodDark", bevel=0.008, grain=0)
    bays = max(1, round(length / 0.78))
    bay = length / bays
    for i in range(bays + 1):
        x = min(max(x0 + i * bay, x0 + 0.045), x1 - 0.045)
        b.box((x, 0.445, -0.017), (0.09, 0.67, 0.022), "WoodDark", bevel=0.003, grain=1)
    for i in range(bays):
        b.box((x0 + (i + 0.5) * bay, 0.445, -0.013), (bay - 0.17, 0.59, 0.014), "WoodDark", bevel=0.006, segs=1, grain=1)


def shell(b):
    # (the floor, the ceiling and the walls are world/shell.py's: one mesh a surface)
    b.obj("Panelling")
    with on_wall(b, "front"):   # not behind the screens' wall of boards
        wainscot(b, -HALF_W, -3.87)
        wainscot(b, 3.87, HALF_W)
    with on_wall(b, "left"):    # nor behind the bookshelf, which is at -3.6 to -1.6 along this wall
        wainscot(b, -HALF_D, -3.63)
        wainscot(b, -1.57, HOLO_Z - HOLO_WIDE / 2 - 0.22)
        wainscot(b, HOLO_Z + HOLO_WIDE / 2 + 0.22, HALF_D)
    with on_wall(b, "right"):
        wainscot(b, -HALF_D, HALF_D)
    with on_wall(b, "back"):
        wainscot(b, -HALF_W, -DOOR_X - DOOR_WIDE / 2 - 0.09)
        wainscot(b, -DOOR_X + DOOR_WIDE / 2 + 0.09, HALF_W)

    b.obj("Mouldings")
    cove = [(0, 0), (0.07, 0), (0.07, -0.012), (0.045, -0.02), (0.022, -0.045), (0.014, -0.075), (0, -0.075)]
    loop = [(-HALF_W, HEIGHT, HALF_D), (HALF_W, HEIGHT, HALF_D), (HALF_W, HEIGHT, -HALF_D), (-HALF_W, HEIGHT, -HALF_D)]
    b.sweep(cove, loop, "TrimWhite", closed=True)
    # a picture rail where paint meets the panelling's cap would crowd it: one higher up instead
    rail = [(0, 0), (0.018, 0.004), (0.018, 0.03), (0, 0.036)]
    b.sweep(rail, [(x, 2.84, z) for x, _, z in loop], "TrimWhite", closed=True)

    b.obj("Beams")
    for z in (-4.3, 0.0, 4.3):
        b.box((0, HEIGHT - 0.09, z), (HALF_W * 2, 0.18, 0.22), "WoodDark", bevel=0.01, grain=0)
        for x in (-HALF_W + 0.06, HALF_W - 0.06):   # a bracket where it meets the wall
            b.prism((x, HEIGHT - 0.27, z), [(-0.08, 0.09), (0.08, 0.09), (0.08, 0.05), (-0.08, -0.09)] if x < 0 else
                    [(-0.08, 0.09), (0.08, 0.09), (0.08, -0.09), (-0.08, 0.05)], 0.12, "WoodDark", plane='xy', bevel=0.006, grain=1)


def lamps(b):
    b.obj("Ceiling lamps")
    for z in (-2.2, 2.3):
        for i in (-1, 0, 1):
            with b.at((i * 2.7, HEIGHT, z)):
                b.box((0, -0.03, 0), (1.40, 0.06, 0.44), "TrimWhite", bevel=0.012)
                b.box((0, -0.064, 0), (1.30, 0.012, 0.34), "Lamp" if z < 0 else "LampLow", bevel=0.004, segs=1)
                for x in (-0.66, 0.66):
                    b.box((x, -0.062, 0), (0.035, 0.02, 0.38), "MetalSteel", bevel=0.004, segs=1)
                for k in range(1, 6):   # the diffuser's ribs
                    b.box((-0.65 + k * 1.3 / 6, -0.071, 0), (0.008, 0.004, 0.34), "TrimWhite")


def window(b):
    cx, w = -WINDOW_Z, WINDOW_WIDE / 2
    y0, y1 = WINDOW_Y - WINDOW_HIGH / 2, WINDOW_Y + WINDOW_HIGH / 2
    with on_wall(b, "right"):
        b.obj("Window")
        # the casing on the wall, the frame in the reveal, two mullions
        casing = [(0, 0), (0.085, 0), (0.085, 0.012), (0.07, 0.022), (0.012, 0.026), (0, 0.016)]
        # up one side, over and down the other: the sill is the foot (a fourth side lay in the sill's own faces)
        b.sweep(casing, [(cx + w, y0, 0), (cx + w, y1, 0), (cx - w, y1, 0), (cx - w, y0, 0)], "TrimWhite", up=(0, 0, -1))
        sash = [(0, -0.03), (0, 0.03), (-0.05, 0.03), (-0.06, 0.02), (-0.06, -0.03)]
        frame(b, (cx, WINDOW_Y, 0.11), WINDOW_WIDE, WINDOW_HIGH, sash, "TrimWhite")
        for k in (-1, 1):
            b.box((cx + k * WINDOW_WIDE / 6, WINDOW_Y, 0.11), (0.056, WINDOW_HIGH - 0.1, 0.056), "TrimWhite", bevel=0.008, grain=1)
        # the sill's top stands a little over the opening's own floor, which it covers
        b.box((cx, y0 - 0.016, -0.05), (WINDOW_WIDE + 0.24, 0.04, 0.25), "WoodOak", bevel=0.012, grain=0)
        b.box((cx, y0 - 0.066, -0.009), (WINDOW_WIDE + 0.16, 0.06, 0.018), "WoodOak", bevel=0.005, grain=0)
        # a blind, drawn most of the way up
        b.box((cx, y1 - 0.03, 0.045), (WINDOW_WIDE - 0.02, 0.045, 0.05), "TrimWhite", bevel=0.006)
        slats = 16
        for i in range(slats):
            b.box((cx, y1 - 0.07 - i * 0.021, 0.045), (WINDOW_WIDE - 0.04, 0.0015, 0.026), "TrimWhite", rot=(28, 0, 0))
        b.box((cx, y1 - 0.08 - slats * 0.021, 0.045), (WINDOW_WIDE - 0.03, 0.018, 0.03), "TrimWhite", bevel=0.004)
        for k in (-1, 1):
            b.tube([(cx + k * 1.5, y1 - 0.05, 0.03), (cx + k * 1.5, y1 - 0.09 - slats * 0.021, 0.03)], 0.0015, "Paper", segs=4)
        b.tube([(cx + w - 0.12, y1 - 0.05, 0.02), (cx + w - 0.12, y0 + 0.35, 0.02)], 0.002, "Paper", segs=4)
        b.lathe((cx + w - 0.12, y0 + 0.33, 0.02), [(0, 0.03), (0.008, 0.02), (0.01, 0), (0, -0.004)], "TrimWhite", segs=8)
        b.obj("Window glass")
        b.quad((cx, WINDOW_Y, 0.12), (WINDOW_WIDE, WINDOW_HIGH), "Glass")

        b.obj("Radiator")
        with b.at((cx, 0.44, -0.075)):
            b.box((0, 0, 0.03), (1.8, 0.54, 0.012), "TrimWhite", bevel=0.004)
            for i in range(30):
                b.box((-0.87 + i * 0.06, 0, 0), (0.034, 0.56, 0.05), "TrimWhite", bevel=0.012, grain=1)
            b.box((0, 0.285, 0.005), (1.8, 0.02, 0.07), "TrimWhite", bevel=0.006)
            for k in (-1, 1):
                b.tube([(k * 0.84, -0.26, 0), (k * 0.84, -0.44, 0)], 0.011, "MetalSteel")
                b.box((k * 0.6, 0.15, 0.05), (0.03, 0.05, 0.04), "TrimWhite")
            b.cyl((0.92, -0.2, 0), 0.022, 0.05, "TrimWhite", segs=12, bevel=0.004, rot=(0, 0, 90))
            b.tube([(0.84, -0.2, 0), (0.9, -0.2, 0)], 0.009, "MetalSteel")


def door_leaf(b, w=DOOR_WIDE, h=DOOR_HIGH, both=False):
    """A six-panel door in a wall's space, its hinges at -w/2, its face a little before the wall.
    both: panelled behind too, for a door that stands open."""
    b.box((0, h / 2, -0.02), (w, h, 0.04), "WoodDark", grain=1)
    for side in ((-1, 1) if both else (-1,)):
        z = -0.02 + side * 0.024
        # stiles and rails standing proud of six panels
        for x in (-w / 2 + 0.06, 0, w / 2 - 0.06):
            b.box((x, h / 2, z), (0.12 if x else 0.10, h, 0.012), "WoodDark", bevel=0.003, segs=1, grain=1)
        for y, t in ((0.11, 0.22), (0.86, 0.14), (1.42, 0.12), (h - 0.07, 0.14)):
            b.box((0, y, z - side * 0.0005), (w, t, 0.011), "WoodDark", bevel=0.003, segs=1, grain=0)   # a hair under the stiles
        for x in (-0.2175, 0.2175):
            for y, t in ((0.505, 0.57), (1.14, 0.42), (1.72, 0.48)):
                b.box((x, y, z - side * 0.003), (0.255, t - 0.06, 0.01), "WoodDark", bevel=0.008, segs=1, grain=1)
        with b.at((0.37, 1.0, -0.02 + side * 0.03), (0, 0 if side < 0 else 180, 0)):   # the handle's side
            b.lathe((0, 0, 0), [(0.026, 0), (0.026, 0.004), (0.012, 0.008), (0.009, 0.03), (0.02, 0.04), (0.03, 0.055),
                                (0.028, 0.07), (0.012, 0.078), (0, 0.08)], "Brass", segs=16, rot=(-90, 0, 0))
            b.box((0, -0.09, 0.002), (0.03, 0.05, 0.004), "Brass", bevel=0.002, segs=1)
    for y in (0.25, 1.05, 1.85):
        b.cyl((-w / 2 - 0.004, y, -0.046), 0.007, 0.09, "Brass", segs=8)


def door_casing(b, w=DOOR_WIDE, h=DOOR_HIGH):
    casing = [(0, 0), (0.09, 0), (0.09, 0.014), (0.075, 0.026), (0.014, 0.03), (0, 0.02)]
    b.sweep(casing, [(w / 2, 0, 0), (w / 2, h, 0), (-w / 2, h, 0), (-w / 2, 0, 0)], "WoodDark", up=(0, 0, -1))   # round this way it lies outside the door


def holo_door(b):
    """The holodeck's doorway from the den: a heavy frame of dark metal with a lit head."""
    w, h = HOLO_WIDE, HOLO_HIGH
    with on_wall(b, "left"):
        with b.at((HOLO_Z, 0, 0)):
            b.obj("Holodeck door")
            reveal(b, -w / 2, w / 2, h, "RackSteel", sill="MetalBlack")
            for k in (-1, 1):
                b.box((k * (w / 2 + 0.1), h / 2, -0.05), (0.2, h, 0.1), "RackSteel", bevel=0.012, grain=1)
                b.box((k * (w / 2 + 0.1), h * 0.5, -0.103), (0.05, h - 0.5, 0.008), "MetalBlack", bevel=0.003, segs=1)
            b.prism((0, h, -0.06), [(-w / 2 - 0.2, 0), (w / 2 + 0.2, 0), (w / 2 + 0.06, 0.3), (-w / 2 - 0.06, 0.3)], 0.12, "RackSteel",
                    plane='xy', bevel=0.012)
            b.box((0, h + 0.13, -0.123), (w - 0.3, 0.05, 0.008), "Amber", bevel=0.002, segs=1)


def door(b):
    with on_wall(b, "back"):
        with b.at((-DOOR_X, 0, 0)):
            b.obj("Door")
            w, h = DOOR_WIDE, DOOR_HIGH
            door_casing(b)
            reveal(b, -w / 2, w / 2, h, "WoodDark")
            with b.at((-w / 2, 0, -0.01), (0, 100, 0)):   # open, back against the wall's corner
                with b.at((w / 2, 0, 0)):
                    door_leaf(b, both=True)
        b.obj("Wall things")
        b.box((-DOOR_X + 0.72, 1.25, -0.004), (0.075, 0.115, 0.008), "TrimWhite", bevel=0.003, segs=1)
        b.quad((-DOOR_X + 0.72, 1.25, -0.0085), (0.072, 0.108), "Details", decal="switch")
        # a clock and a calendar by the door
        b.asset("wall_clock", (-2.45, 2.26, -0.024), rot=(0, 180, 0), parts=("=wall_clock",),
                hands=((0, 0, 0), ("hour", "wall_clock_hours_hand"), ("minute", "wall_clock_minute_hand"), ("second", "wall_clock_second_hand")))
        b.box((-2.45, 1.62, -0.003), (0.30, 0.375, 0.004), "Paper")
        b.quad((-2.45, 1.62, -0.007), (0.296, 0.371), "Details", decal="calendar")
    for wall, x in (("front", 4.2), ("left", -4.6), ("right", 4.6), ("back", 1.8)):
        with on_wall(b, wall):
            b.box((x, 1.05, -0.004), (0.085, 0.085, 0.008), "TrimWhite", bevel=0.003, segs=1)
            b.quad((x, 1.05, -0.0085), (0.08, 0.08), "Details", decal="socket")


def build(b):
    shell(b)
    lamps(b)
    window(b)
    door(b)
    holo_door(b)
