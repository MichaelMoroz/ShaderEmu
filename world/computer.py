# The computer's wall: the panelled wall the screens hang on, their housings, the desk and its
# keyboard consoles, the tower, the rack, the chair. Screens and canvases are Unity's own; the
# rectangles below are where it puts them (ShaderEmuBuilder.cs).
import json
import math
import os

import random

import drape
import furniture
import lib
import pc
import pc_faces
from mathutils import Vector
from pc import mouse_body, office_chair
from room import HALF_D, HALF_W, frame, on_wall

SCREEN_Z = HALF_D - 0.09
TERMINAL = (-2.35, 1.80, 1.36, 1.02)
DISPLAY = (0.0, 1.82, 1.6, 1.2)
MEMORY = (2.45, 2.13, 1.7, 0.85)
CONTROL = (2.45, 1.22, 1.7, 0.76)
PLAYERS = (1.28, 1.82, 0.5, 1.0)
CPU = (1.28, 2.54, 0.38, 0.38)
LINKS = (DISPLAY[0] - DISPLAY[2] / 2 - 0.17, DISPLAY[1] - DISPLAY[3] / 2 + 0.12, 0.22, 0.22)   # as the builder's WebLinks puts its button
KEYBOARDS = (-2.35, 0.0)
DESK_TOP = 0.765
TOWER = (1.55, 0.26, HALF_D - 0.4)
RACK = (3.75, 1.0, HALF_D - 0.45)


def housing(b, rect, mat="PlasticBeige", border=0.045, chin=0.0, rim=True, depth=0.05):
    """A screen's case: flush with the screen round it, a raised rim at its edge, a chin for its knobs."""
    x, y, w, h = rect
    z = SCREEN_Z + 0.005
    depth = HALF_D - 0.0405 - z   # (back to the boards: wall_unit's, whatever it was asked)
    b.box((x, y - chin / 2, z + depth / 2), (w + border * 2, h + border * 2 + chin, depth), mat, bevel=0.014, segs=3)
    if rim:
        lip = [(0.004, 0), (0.004, 0.006), (0.012, 0.014), (0.03, 0.014), (0.036, 0.006), (0.036, 0)]
        with b.at((0, 0, z)):
            frame(b, (x, y, 0), w, h, lip, mat)
    if chin > 0:
        cy = y - h / 2 - border - chin / 2 + 0.012
        b.quad((x - w / 2 + 0.06, cy, z - 0.0006), (0.123, 0.04), "Details", decal="badge")
        for i in range(3):
            b.lathe((x + w / 2 - 0.07 - i * 0.06, cy, z), [(0.016, 0), (0.016, 0.004), (0.011, 0.007), (0.010, 0.016), (0, 0.017)],
                    "PlasticGrey", segs=14, rot=(-90, 0, 0))
        b.box((x + w / 2 - 0.27, cy, z - 0.001), (0.018, 0.008, 0.004), "Led", bevel=0.002, segs=1)


def wall_unit(b):
    b.obj("Screen wall")
    b.pose("hung", (7.72, 2.0, 0.45), at=(0, 1.78, HALF_D - 0.225), rot=(0, 180, 0), name="Screen wall", back=0.03, clear=0.8)
    # oak boards behind the screens, a shelf above and a ledge below
    boards, x0, wide = 8, -3.8, 7.6
    for i in range(boards):
        w = wide / boards
        b.box((x0 + (i + 0.5) * w, (DESK_TOP + 0.031 + 2.75) / 2, HALF_D - 0.02), (w - 0.006, 2.75 - DESK_TOP - 0.031, 0.04), "WoodOak", bevel=0.004, segs=1, grain=1)   # (from the ledge up)
    b.box((0, 2.775, HALF_D - 0.07), (7.72, 0.05, 0.14), "WoodDark", bevel=0.008, grain=0)
    b.box((0, DESK_TOP + 0.0155, HALF_D - 0.03), (7.72, 0.03, 0.06), "WoodDark", bevel=0.006, grain=0)   # (on the desk's back edge, not through it)
    for x in (-3.829, 3.829):   # (2 mm short of the shelf's and the ledge's ends: flush, they flickered)
        b.box((x, 1.75, HALF_D - 0.035), (0.058, 2.06, 0.07), "WoodDark", bevel=0.008, grain=1)

    b.obj("Screen housings")
    housing(b, TERMINAL, chin=0.07)
    housing(b, DISPLAY, chin=0.07)
    housing(b, MEMORY, border=0.04, rim=False)
    housing(b, CPU, border=0.012, rim=False)
    housing(b, CONTROL, "PlasticDark", border=0.03, rim=False, depth=0.04)
    housing(b, PLAYERS, "PlasticDark", border=0.015, rim=False, depth=0.04)
    housing(b, LINKS, "PlasticDark", border=0.012, rim=False, depth=0.04)
    # notes stuck to the boards
    for i, (x, y, turn) in enumerate(((-3.42, 1.55, 6), (-3.36, 1.42, -8), (3.55, 1.0, 4))):
        b.quad((x, y, HALF_D - 0.041), (0.076, 0.076), "Details", rot=(0, 0, turn), decal="note_%d" % i)

    # the board on the left wall, in that wall's space
    with on_wall(b, "left"):
        b.obj("Board")
        with b.at((-0.2, 1.86, 0)):   # its frame's foot clear of the panelling's cap at 0.92
            b.box((0, 0, -0.012), (2.36, 1.76, 0.024), "PlasticDark")
            moulding = [(0, 0), (0.07, 0), (0.07, 0.03), (0.05, 0.046), (0.012, 0.046), (0, 0.036)]
            frame(b, (0, 0, 0), 2.34, 1.74, moulding, "WoodDark")


KEY_HIGH = 0.012     # a cap, from the tray to its top
KEY_UNDER = 0.0015   # its top below the canvas the beams touch


def keyboard(b, x):
    """The caps of the keyboard whose canvas is at x: the builder's layout (world/keyboard.json),
    each cap a tapered block, all of them one picture seen from above (Keyboard.png)."""
    layout = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "keyboard.json"), encoding="utf-8"))
    wide, high = layout["width"], layout["height"]
    vertices, faces, uvs, normals = [], [], [], []

    def ring(x0, y0, x1, y1, z):
        """Four corners of a rectangle given in the panel's millimetres, as the canvas's own metres."""
        first = len(vertices)
        for px, py in ((x0, y1), (x1, y1), (x1, y0), (x0, y0)):
            vertices.append(((px - wide / 2) * 0.001, (high / 2 - py) * 0.001, z))
        return first

    def face(corners, normal, uv=None):
        faces.append(tuple(corners))
        uvs.append(uv or [pc_faces.canvas_uv(vertices[i][0] * 1000 + wide / 2, high / 2 - vertices[i][1] * 1000, wide, high) for i in corners])
        normals.append(normal)
    sh, tp = pc_faces.CAP_SHOULDER, pc_faces.CAP_TOP   # (the classroom's keyboards have the same caps)
    for x0, y0, w, h, legend in layout["keys"]:
        rects = [(x0, y0, x0 + w, y0 + h), (x0 + sh[0], y0 + sh[1], x0 + w - sh[0], y0 + h - sh[2]),
                 (x0 + tp[0], y0 + tp[1], x0 + w - tp[0], y0 + h - tp[2])]
        rises = (0.0, KEY_HIGH * (1 - pc_faces.CAP_STEP) * 1000, KEY_HIGH * 1000)
        base, shoulder, top = (ring(*r, KEY_UNDER + KEY_HIGH - rise * 0.001) for r, rise in zip(rects, rises))
        corners = [((r[0], r[3]), (r[2], r[3]), (r[2], r[1]), (r[0], r[1])) for r in rects]
        face((top, top + 1, top + 2, top + 3), (0, 0, -1))
        face((base + 3, base + 2, base + 1, base), (0, 0, 1))   # its foot, closing it (world/check_closed.py)
        for k, normal in enumerate(((0, -1, 0), (1, 0, 0), (0, 1, 0), (-1, 0, 0))):   # its sides, unrolled into their band of the picture
            sides = pc_faces.cap_side_uvs(corners, k, rises, pc_faces.cap_named(legend), wide, high)
            for (lower, upper), uv in zip(((base, shoulder), (shoulder, top)), sides):
                face((lower + k, lower + (k + 1) % 4, upper + (k + 1) % 4, upper + k), normal, uv)
    with b.at((x, 0.95, 4.88), (58, 0, 0)):
        b.mesh(vertices, faces, uvs, "Keyboard", normal=normals)


def console(b, x):
    """The sloping case a keyboard stands on: 58 degrees from upright, far enough under the
    keyboard's canvas for the caps to stand between."""
    under = KEY_UNDER + KEY_HIGH + 0.002
    slope = math.radians(32)
    dz, dy = math.cos(slope), math.sin(slope)
    z0, y0 = 4.88 + under * dy, 0.95 - under * dz
    low, high = (z0 - 0.245 * dz, y0 - 0.245 * dy), (z0 + 0.40 * dz, y0 + 0.40 * dy)
    profile = [(4.655, DESK_TOP), (4.655, low[1] - 0.012), low, high, (5.31, high[1]), (5.31, DESK_TOP)]
    b.prism((x, 0, 0), profile, 1.46, "PlasticBeige", plane='zy', bevel=0.008)
    for side in (-1, 1):   # cheeks that stand above the slope, as high as the caps
        b.prism((x + side * 0.738, 0, 0), [(4.67, DESK_TOP), (4.67, low[1] + 0.012), (low[0] + 0.01, low[1] + 0.02),
                                             (high[0], high[1] + 0.02), (5.30, high[1] + 0.02), (5.30, DESK_TOP)],
                0.016, "PlasticGrey", plane='zy', bevel=0.003, segs=1)
    # the tray the caps stand in
    b.box((x, y0 - 0.001, z0), (1.40, 0.004, 0.47), "PlasticDark", rot=(-32, 0, 0))
    keyboard(b, x)


def desk(b):
    b.obj("Desk")
    # (its drawers and the keys' console towards the room, -z; one stands at it: its chair is pushed back by the rack)
    b.pose("against", (4.7, DESK_TOP, 1.0), at=(-1.15, DESK_TOP / 2, HALF_D - 0.5), rot=(0, 180, 0), name="Den desk", back=0.2, clear=0.6)
    b.box((-1.15, DESK_TOP - 0.0098, HALF_D - 0.5), (4.7, 0.0196, 1.0), "WoodOak", bevel=0.006, segs=3, grain=0)   # (as thick as the clamp lamp takes)
    b.box((-1.15, 0.45, HALF_D - 0.075), (4.5, 0.5, 0.022), "WoodOak", grain=0)
    for x in (-3.2, 0.95):   # two pedestals of three drawers
        with b.at((x, 0, HALF_D - 0.52)):
            b.box((0, 0.3877, 0), (0.48, 0.7154, 0.9), "WoodOak", bevel=0.004, segs=1, grain=1)
            b.box((0, 0.015, 0), (0.46, 0.03, 0.86), "PlasticDark")
            for i, (y, h) in enumerate(((0.61, 0.16), (0.435, 0.16), (0.205, 0.27))):
                b.box((0, y, -0.455), (0.45, h - 0.012, 0.018), "WoodOak", bevel=0.004, segs=1, grain=0)
                b.box((0, y + h / 2 - 0.04, -0.474), (0.14, 0.014, 0.02), "MetalSteel", bevel=0.005, segs=2)
            b.cyl((0.17, 0.655, -0.466), 0.008, 0.006, "Brass", segs=10, rot=(90, 0, 0))   # a lock
    b.box((-1.17, 0.3727, HALF_D - 0.45), (0.03, 0.7454, 0.7), "WoodOak", grain=1)
    for x in (-1.9, -0.5):   # grommets for the cables
        b.lathe((x, 0.7655, HALF_D - 0.14), [(0, 0), (0.028, 0), (0.032, 0.002), (0.032, 0.004), (0.02, 0.005)], "PlasticDark", segs=16)

    b.obj("Consoles")
    for x in (-2.35, 0.0):
        b.pose("front", (1.48, 0.3, 0.66), at=(x, DESK_TOP + 0.15, 4.98), rot=(0, 180, 0), name="Console", clear=0.3)   # (its keys to the room)
        console(b, x)


TOWER_BACK = 0.17   # the tower's back behind TOWER: the classroom's case (world/pc.py), its front where the old one's was


def tower(b):
    """The classroom's own case (world/bake_pc.py's tower alone), on the floor by the desk."""
    x, y, z = TOWER
    b.asset("pc_tower", (x, 0, z + TOWER_BACK - (pc.FRONT + pc.TOWER_DEEP)), scale=1.0)
    cables(b)


LAID = []   # leads laid out and not yet fallen: [name, the points of its middle, its radius, its material]
CHANCE = random.Random(41)


def cable(b, points, radius=0.005, mat="Rubber", up=0.03, wander=0.006):
    """A lead laid out through `points`, ruffled by chance; fall() lets those laid out fall together."""
    LAID.append(["lead %d" % len(LAID), drape.ruffled(drape.curve(points), CHANCE, up=up * CHANCE.uniform(0.3, 1.0), wander=wander), radius, mat])


def fall(b, boxes, stiff=1.0):
    """The leads laid out fall on `boxes` and on each other (drape.settle), and are made."""
    leads = [(name, path, radius) for name, path, radius, _ in LAID]
    fallen = drape.settle(leads, boxes, [], stiff)
    for (name, _, radius, mat), path in zip(LAID, fallen):
        b.tube([tuple(q) for q in path], radius, mat, segs=8, smooth=0)
    drape.check([(name, path, radius) for (name, _, radius), path in zip(leads, fallen)], boxes, [])
    del LAID[:]


def cables(b):
    """Leads let fall where they hang and lie (world/drape.py), held at their plugs and by the clips along the skirting."""
    b.obj("Cables")
    x, _, z = TOWER
    back, wall = z + TOWER_BACK, HALF_D - 0.045
    at = pc.tower_back()   # where its sockets are, from its middle and the floor
    ground, panels = ((0, -0.05, HALF_D - 2), (12, 0.1, 6), 0), ((0, 1.5, wall + 0.106), (12, 3.0, 0.2), 0)
    # the tower: power to a strip beside it, two leads up through the wall, one along the floor to the rack
    by_tower = (x + 0.42, 0, HALF_D - 0.16)   # (turned: its own lead leaves towards the rack, its switch is by the tower)
    pc.strip(b, by_tower, 180, plugs=(4,))
    sx = pc.strip_socket(4, by_tower, 180)[0]
    ix, iy = x + at["inlet"][0], at["inlet"][1]
    b.box((ix, iy, back + 0.014), (0.03, 0.022, 0.028), "Rubber", bevel=0.004, segs=2)   # the mains lead's moulded plug
    cable(b, [(ix, iy, back + 0.02), (ix, iy - 0.01, back + 0.05), (ix + 0.06, 0.2, back + 0.08), (sx - 0.08, 0.14, HALF_D - 0.13),
              pc.strip_socket(4, by_tower, 180, up=pc.PLUG_HIGH - 0.004)], 0.006)
    # the two screens' leads: from the video card's socket and the second serial one, up through the wall
    for (sx, sy), up in ((at["video"], x - 0.06), (at["com2"], x - 0.035)):
        pc.dsub_plug(b, x + sx, sy, back, 0.032)
        cable(b, [(x + sx, sy, back + pc.PLUG_LONG - 0.01), (x + sx, sy, back + pc.PLUG_LONG + 0.012), (x + sx, sy + 0.04, back + pc.PLUG_LONG + 0.035),
                  (up, 0.50, back + 0.085), (up, 0.72, wall - 0.012), (up, 0.80, wall + 0.004)], 0.005, "PlasticGrey")
        b.lathe((up, 0.80, wall), [(0.011, 0), (0.014, 0.003), (0.014, 0.006), (0.008, 0.008)], "PlasticDark", segs=12, rot=(-90, 0, 0))
    # its sound: from the sound card's line out up through the wall beside them (its own chance: the other leads keep theirs)
    # (up close to the case, clear of COM 2's plug and under the mains lead, which lies further out)
    jx, jy, up = x + 0.012, at["first"] - 3 * pc.SLOT_PITCH, x + 0.03
    pc.jack_plug(b, jx, jy, back + 0.0018)
    LAID.append(["sound", drape.ruffled(drape.curve([(jx, jy, back + pc.JACK_LONG - 0.002), (jx, jy, back + pc.JACK_LONG + 0.012), (x + 0.022, jy + 0.05, back + 0.046),
                                                     (up, 0.45, back + 0.05), (up, 0.72, wall - 0.012), (up, 0.80, wall + 0.004)]), random.Random(43), up=0.01, wander=0.003), 0.002, "Rubber"])
    b.lathe((up, 0.80, wall), [(0.011, 0), (0.014, 0.003), (0.014, 0.006), (0.008, 0.008)], "PlasticDark", segs=12, rot=(-90, 0, 0))
    run = HALF_D - 0.06   # the floor run, behind the strip
    clips = (x + 0.95, x + 1.4, RACK[0] - 0.7)   # over the run: a lead is held under each, and falls as it will between them
    nx, ny = x + at["lan"][0], at["lan"][1]   # the network card, in the sixth slot
    pc.lan_plug(b, nx, ny, back, "CableBlue")
    for first, last, radius, depth, mat in (
            ([(nx, ny, back + pc.LAN_PLUG - 0.004), (nx + 0.01, ny - 0.01, back + 0.05), (x + 0.06, 0.04, back + 0.09), (x + 0.20, 0.006, run), (x + 0.6, 0.006, run)],
             [(RACK[0] - 0.5, 0.006, run), (RACK[0] - 0.37, 0.006, run - 0.008), (RACK[0] - 0.34, 0.03, run - 0.02)], 0.005, run, "CableBlue"),
            ([(x + 0.612, 0.02, HALF_D - 0.16), (x + 0.70, 0.008, HALF_D - 0.12), (x + 0.82, 0.007, run - 0.012)],
             [(RACK[0] - 0.5, 0.007, run - 0.012), (RACK[0] - 0.36, 0.007, run - 0.06)], 0.006, run - 0.012, "Rubber")):
        held = [(cx, radius + 0.001, depth) for cx in clips]
        cable(b, first + [held[0]], radius, mat, up=0.012, wander=0.003)
        for a, c in zip(held, held[1:]):
            cable(b, [a, c], radius, mat, up=0.012, wander=0.003)
        cable(b, [held[-1]] + last, radius, mat, up=0.012, wander=0.003)
    for cx in clips:
        b.box((cx, 0.009, run - 0.006), (0.016, 0.018, 0.034), "PlasticBeige", bevel=0.005, segs=2)
    fall(b, [ground, panels, ((x, pc.TOWER_HIGH / 2, back - pc.TOWER_DEEP / 2), (0.184, pc.TOWER_HIGH, pc.TOWER_DEEP), 0),
             pc.strip_box(by_tower, 180), ((0.95, 0.375, HALF_D - 0.52), (0.48, 0.75, 0.9), 0)])
    # under the desk: a strip at each side of its middle panel (which stands at -1.17), and two leads down to it from the grommet there
    for cx, under in ((-1.9, -1.62), (-0.5, -0.82)):
        lies = (under, 0, HALF_D - 0.2)
        toward = 1 if cx < under else -1
        sockets = (1, 0) if toward > 0 else (3, 4)   # the two nearest the grommet, the nearer lead in the nearer
        pc.strip(b, lies, plugs=sockets)
        # its own lead, as a classroom strip's: out of the sleeve, along the floor, into a socket let into it
        end, sz = under - pc.STRIP[0] / 2 - 0.012, HALF_D - 0.5   # (0.07 aside: clear of the middle panel)
        LAID.append(["strip %d" % len(LAID), drape.ruffled(drape.curve(
            [(end, 0.02, lies[2]), (end - 0.035, 0.005, lies[2]), (end - 0.07, 0.005, lies[2]), (end - 0.07, 0.005, sz + 0.03), (end - 0.07, 0.03, sz)]),
            random.Random(int(-under * 100)), up=0.012, wander=0.003), 0.004, "Rubber"])   # (its own chance: the other leads keep theirs)
        b.box((end - 0.07, 0.002, sz), (0.12, 0.004, 0.12), "MetalSteel", bevel=0.001, segs=1)
        b.box((end - 0.07, 0.017, sz), (0.036, 0.026, 0.036), "Rubber", bevel=0.006, segs=2)
        for k, mat in ((0, "Rubber"), (1, "PlasticGrey")):
            end = pc.strip_socket(sockets[k], lies)[0]
            cable(b, [(cx + 0.008 * (2 * k - 1), 0.716, HALF_D - 0.14), (cx + 0.008 * (2 * k - 1), 0.62, HALF_D - 0.125 - 0.012 * k), (cx + toward * (0.03 + 0.03 * k), 0.32, HALF_D - 0.115 - 0.03 * k),
                      (cx + toward * (0.12 + 0.04 * k), 0.09, HALF_D - 0.125 - 0.035 * k), ((cx + end) / 2, 0.012, HALF_D - 0.12 - 0.03 * k), (end - toward * 0.1, 0.012, HALF_D - 0.12),
                      (end - toward * 0.015, 0.14, HALF_D - 0.18), pc.strip_socket(sockets[k], lies, up=pc.PLUG_HIGH - 0.004)], 0.005, mat)
    fall(b, [ground, panels, ((-1.15, DESK_TOP - 0.0098, HALF_D - 0.5), (4.7, 0.0196, 1.0), 0), ((-1.15, 0.45, HALF_D - 0.075), (4.5, 0.5, 0.022), 0),
             pc.strip_box((-1.62, 0, HALF_D - 0.2)), pc.strip_box((-0.82, 0, HALF_D - 0.2)),
             ((-1.17, 0.3727, HALF_D - 0.45), (0.03, 0.7454, 0.7), 0)])
    # the rack: patch leads hanging between its hubs in loose loops
    rx, _, rz = RACK
    front = rz - 0.372
    for k, (y0, y1, mat) in enumerate(((0.86, 0.69, "CableBlue"), (0.86, 0.77, "CableYellow"), (0.77, 0.69, "PlasticGrey"), (0.86, 1.30, "CableYellow"),
                                       (0.69, 1.30, "CableBlue"))):
        x0, x1 = rx - 0.21 + k * 0.05, rx - 0.17 + k * 0.07
        out = 0.05 + 0.012 * k
        low = min(y0, y1) - 0.05 - 0.02 * k
        cable(b, [(x0, y0, front), (x0, y0 - 0.01, front - out * 0.6), ((x0 + x1) / 2, low if abs(y0 - y1) < 0.3 else min(y0, y1) - 0.03, front - out),
                  (x1, y1 - 0.01, front - out * 0.6), (x1, y1, front)], 0.0035, mat, up=0.0, wander=0.004)
    fall(b, [ground, ((rx, 1.0, rz + 0.016), (0.7, 2.0, 0.77), 0)], stiff=2.5)   # short patch leads: stiffer, or they fold at the bottom of a loop



def rack(b):
    b.obj("Rack")
    x, _, z = RACK
    with b.at((x, 0, z)):
        b.pose("against", (0.7, 2.0, 0.81), at=(0, 1.0, -0.005), rot=(0, 180, 0), name="Rack", back=0.3, clear=0.8)   # (its units' faces to -z, their plates 3 mm out)
        b.box((0, 0.04, 0), (0.7, 0.08, 0.8), "RackSteel", bevel=0.006)
        b.box((0, 1.97, 0), (0.7, 0.06, 0.8), "RackSteel", bevel=0.006)
        for sx in (-0.325, 0.325):
            for sz in (-0.375, 0.375):
                b.box((sx, 1.0, sz), (0.05, 1.86, 0.05), "RackSteel", bevel=0.004, segs=1, grain=1)
            b.box((sx + (0.018 if sx < 0 else -0.018), 1.0, 0), (0.008, 1.84, 0.7), "RackSteel", grain=1)
        b.box((0, 1.0, 0.39), (0.6, 1.84, 0.008), "RackSteel", grain=1)
        y = 0.13
        units = (("rack_vent", 0.088), ("rack_drives", 0.176), ("rack_drives", 0.176), ("rack_panel", 0.132), ("rack_switch", 0.088),
                 ("rack_switch", 0.088), ("rack_vent", 0.088), ("rack_drives", 0.176), ("rack_panel", 0.132), ("rack_drives", 0.176),
                 ("rack_vent", 0.088), ("rack_panel", 0.132), ("rack_switch", 0.088))
        for i, (decal, high) in enumerate(units):
            deep = 0.6 if decal != "rack_switch" else 0.3
            # its body between the posts, and its face plate, as wide as the posts, whose ears lie on their fronts
            b.box((0, y + high / 2, -0.4 + deep / 2), (0.598, high - 0.006, deep), "RackSteel", bevel=0.003, segs=1)
            b.box((0, y + high / 2, -0.40175), (0.69, high - 0.006, 0.0025), "RackSteel", grain=1)
            b.quad((0, y + high / 2, -0.4036), (0.58, high - 0.008), "Details", decal=decal)
            if decal != "rack_vent":
                for k in range(3):
                    if b.random.random() < 0.8:
                        b.box((0.2 + k * 0.03, y + high - 0.022, -0.4045), (0.01, 0.006, 0.003), "Led" if b.random.random() < 0.75 else "Amber")
            y += high
        for sx in (-0.26, 0.26):
            for sz in (-0.3, 0.3):
                b.cyl((sx, 0.0, sz), 0.03, 0.02, "Rubber", segs=10)


def chair(b):
    b.obj("Chair")
    with b.at((3.0, 0, HALF_D - 1.7), (0, 205, 0)):   # in the corner by the rack: the other one is the holodeck's door
        office_chair(b, kind="floor")   # (pushed back from the desk: it faces none)


def clutter(b):
    b.obj("Desk things")
    top = DESK_TOP
    # between the keyboards: speakers, a mouse on its mat, a mug of pens, floppies
    for x, turn in ((-1.52, 14), (-0.82, -14)):
        with b.at((x, top, HALF_D - 0.24), (0, turn, 0)):
            b.box((0, 0.12, 0), (0.12, 0.24, 0.14), "PlasticBeige", bevel=0.01, segs=3)
            b.lathe((0, 0.15, -0.0705), [(0.045, 0), (0.045, 0.003), (0.04, 0.006), (0, 0.004)], "PlasticGrey", segs=18, rot=(-90, 0, 0))
            b.lathe((0, 0.06, -0.0705), [(0.022, 0), (0.022, 0.003), (0.018, 0.005), (0, 0.004)], "PlasticGrey", segs=14, rot=(-90, 0, 0))
            b.box((0.04, 0.02, -0.071), (0.006, 0.006, 0.003), "Led")
    with b.at((-1.17, top, 4.86), (0, -6, 0)):
        b.box((0, 0.002, 0), (0.24, 0.004, 0.2), "Rubber", bevel=0.002, segs=1)
        b.quad((0, 0.0052, 0), (0.23, 0.19), "Details", rot=(90, 0, 0), decal="mousepad")
        with b.at((0.02, 0.0012, 0.0)):
            mouse_body(b)
    # its lead: out of the sleeve under its buttons (by the matrices that place the mouse), across the desk and
    # down a grommet of its own; let fall like the others (it was drawn from where an older mouse's ended, to the bare wall)
    body = lib.place((-1.17, top, 4.86), (0, -6, 0)) @ lib.place((0.02, 0.0012, 0.0)) @ lib.place((0, 0, 0), (0, pc.MOUSE_TURN, 0))
    m0, m1 = body @ Vector(pc.MOUSE_LEAD), body @ Vector((pc.MOUSE_LEAD[0], 0.0075, pc.MOUSE_LEAD[2] - 0.03))
    gx, gz = -1.05, HALF_D - 0.14
    b.lathe((gx, top + 0.0005, gz), [(0, 0), (0.028, 0), (0.032, 0.002), (0.032, 0.004), (0.02, 0.005)], "PlasticDark", segs=16)
    cable(b, [tuple(m0), tuple(m1), (-1.15, top + 0.004, 5.06), (-1.09, top + 0.004, 5.22), (gx - 0.005, top + 0.03, gz - 0.035), (gx, top + 0.022, gz), (gx, top + 0.003, gz)],
          0.0022, "PlasticBeige", up=0.02)
    fall(b, [((-1.15, DESK_TOP - 0.0098, HALF_D - 0.5), (4.7, 0.0196, 1.0), 0), ((-1.17, top + 0.002, 4.86), (0.24, 0.004, 0.2), -6), ((-1.3, top + 0.065, 5.2), (0.1, 0.13, 0.07), 0),
             ((-1.52, top + 0.12, HALF_D - 0.24), (0.12, 0.24, 0.14), 14), ((-0.82, top + 0.12, HALF_D - 0.24), (0.12, 0.24, 0.14), -14),
             ((0, 1.5, HALF_D - 0.045 + 0.106), (12, 3.0, 0.2), 0)])
    furniture.mug(b, (-1.5, top, 4.98), turn=-30)
    for i, (lean, turn, mat) in enumerate(((10, 0, "CableYellow"), (-12, 80, "PlasticDark"), (8, 200, "CableBlue"))):
        b.cyl((-1.5 + 0.01 * math.cos(i * 2.1), top + 0.088, 4.98 + 0.01 * math.sin(i * 2.1)), 0.0035, 0.16, mat, segs=8, rot=(lean, turn, lean / 2))
    for i, (decal, x, z, turn) in enumerate((("floppy_black", -0.93, 4.78, 12), ("floppy_blue", -0.935, 4.782, -8), ("floppy_red", -0.925, 4.776, 31),
                                             ("floppy_grey", -1.42, 4.74, -24))):
        y = top + 0.0017 + (i if i < 3 else 0) * 0.0034
        b.box((x, y, z), (0.09, 0.0032, 0.094), "PlasticDark", rot=(0, turn, 0))
        b.quad((x, y + 0.0026, z), (0.088, 0.092), "Details", rot=(90, turn, 0), decal=decal)
    # at the desk's left end: a lamp on an arm, magazines, a listing
    # clamped to the left end's edge (rooms.py's lamp: its jaw takes the top's 19.6 mm), its arm over the desk
    b.asset("desk_lamp_arm_01", (-1.15 - 2.35 + 0.028, top + 0.0012, HALF_D - 0.39), rot=(0, 270, 0), scale=1.0)
    b.asset("alarm_clock_01", (-1.3, top, 5.2), rot=(0, 190, 0), size=(None, 0.13, None), parts=("=alarm_clock_01",),
            hands=((0, 0, 0.065), ("hour", "houd_hand"), ("minute", "minute_hand"), ("second", "second_hand")))
    # (clear of the console, whose cheek is at -3.10)
    furniture.pile(b, ["byte_1993_03", "pc_computing_magazine_v10i6", "computer_gaming_world_issue_140"], (-3.29, top, 4.80), (-10, 4, -3))
    # at its right end, behind the controllers: games in their boxes
    for i, (x, turn, game) in enumerate(((0.87, -9, "doom"), (1.08, 6, "quake"))):
        furniture.leaning_box(b, x, top, HALF_D - 0.04, turn, 11 + 3 * i, game)


def build(b):
    wall_unit(b)
    desk(b)
    tower(b)
    rack(b)
    chair(b)
    clutter(b)
