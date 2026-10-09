# A desktop computer of the middle of the 1990s, for the classroom's eight places: a monitor of
# twice a 15 inch tube's diagonal, a keyboard and a mouse on the table, the tower on the floor.
# Everything is in a place's own frame: x to the sitter's right, y up from the table, the sitter
# at -z. The glass is a material of its own ("TubeGlass"), its picture's corners at the corners
# of what the bezel shows of it.
import json

import pc_faces
import math
import os

FRONT = -0.150                            # the tower's front face, in its own frame
TOWER_HIGH, TOWER_DEEP = 0.36, 0.42
# Sizes looked up (docs/stations.md has where): a 29 inch tube with 27 inches seen (549 x 411 mm,
# NEC's XM29 Plus: an A68 tube of 108 degrees, stripes 0.60 mm apart at its middle); drive bays
# 146.1 x 41.3 and 101.6 x 26.1 mm; a PS/2 power supply's 150 x 86 mm back. The monitor's case is
# that monitor's own, 645 x 533 x 518 mm; the tower a mini tower's 180 x 360 x 420.
GLASS = (0.549, 0.411, 0.3625, -0.14)     # what shows of the tube: wide, high, its middle's height, its front
BULGE = 1.8                               # the glass is part of a ball of this radius: a flat square tube's face
OUT_W, OUT_H = 0.645, 0.533
LOW = 0.07                                # the case's underside, on its foot
OUT_Y = LOW + OUT_H / 2
CHIN = (LOW + GLASS[2] - GLASS[1] / 2 - 0.012) / 2
BAY_525, BAY_35 = (0.1461, 0.0413), (0.1016, 0.0261)
SLOT_PITCH, BRACKET = 0.02032, (0.1207, 0.01842)


# "full": everything, as geometry. "high": the monitor and the tower with all that stands on them,
# to bake from. "low": their bare shells in one material, "PCBaked", which world/bake_pc.py gives
# the baked pictures of the rest (colour, normals, roughness, light).
MODE = "full"


def skin(mat):
    return "PCBaked" if MODE == "low" else mat


def rrect(w, h, r, cx=0.0, cy=0.0, n=5):
    """A rounded rectangle's outline in x and y, anticlockwise from the middle of its right side."""
    r = min(r, w / 2 - 1e-4, h / 2 - 1e-4)
    points = []
    for corner, (sx, sy) in enumerate(((1, 1), (-1, 1), (-1, -1), (1, -1))):
        for k in range(n + 1):
            a = math.radians(corner * 90 + 90 * k / n)
            points.append((cx + sx * (w / 2 - r) + r * math.cos(a), cy + sy * (h / 2 - r) + r * math.sin(a)))
    return points


def loft(b, rings, mat, first=False, last=False, normal='out', tile=0.3):
    """A skin over rings of as many points each; first and last close its ends. Its picture is
    laid by length: round each ring by the ring's own outline, from ring to ring by how far apart
    they are, so a metre of the picture is a metre of the skin wherever it narrows."""
    def apart(p, q):
        return math.sqrt(sum((p[i] - q[i]) ** 2 for i in range(3)))
    vertices, faces, uvs = [], [], []
    n = len(rings[0])
    for ring in rings:
        vertices += ring
    round_ring = []   # for each ring: how far round its outline each point is, and the whole way round at the end
    for ring in rings:
        lengths = [0.0]
        for k in range(n):
            lengths.append(lengths[-1] + apart(ring[k], ring[(k + 1) % n]))
        round_ring.append(lengths)
    along = [0.0]
    for i in range(len(rings) - 1):
        along.append(along[-1] + sum(apart(rings[i][k], rings[i + 1][k]) for k in range(n)) / n)
    for i in range(len(rings) - 1):
        for k in range(n):
            faces.append((i * n + k, i * n + (k + 1) % n, (i + 1) * n + (k + 1) % n, (i + 1) * n + k))
            uvs.append([(round_ring[i][k] / tile, along[i] / tile), (round_ring[i][k + 1] / tile, along[i] / tile),
                        (round_ring[i + 1][k + 1] / tile, along[i + 1] / tile), (round_ring[i + 1][k] / tile, along[i + 1] / tile)])
    for closed, ring in ((first, 0), (last, len(rings) - 1)):
        if closed:   # an end is flat: its picture is laid flat on it
            faces.append(tuple(ring * n + k for k in range(n)))
            uvs.append([(vertices[i][0] / tile, vertices[i][1] / tile) for i in faces[-1]])
    b.mesh(vertices, faces, uvs, mat, normal=normal)


def at_z(outline, z):
    return [(x, y, z) for x, y in outline]


def round_button(b, x, y, z, radius, mat, proud=0.003):
    """A round button in a ring, on a face that looks towards -z."""
    b.lathe((x, y, z), [(radius * 1.35, 0), (radius * 1.35, 0.0012), (radius * 1.12, 0.0018), (radius * 1.08, 0.0002)], "PlasticCase", segs=20, rot=(-90, 0, 0))
    b.lathe((x, y, z), [(radius, 0), (radius, proud * 0.7), (radius * 0.8, proud), (0, proud * 1.05)], mat, segs=20, rot=(-90, 0, 0))


def monitor(b):
    """A monitor of the decade, in two mouldings: a square-shouldered bezel, and behind a seam a
    housing that narrows in flat faces to the tube's neck, slotted at its sides and top."""
    w, h, mid, front = GLASS
    seam, block = 0.003, 0.11                   # the gap between the mouldings, and how deep the bezel's is
    z0 = front + block + seam                   # where the housing starts
    run, taper = 0.09, 0.305                    # its straight part, then its narrowing: 518 mm in all
    back_w, back_h, back_y = OUT_W * 0.62, OUT_H * 0.66, OUT_Y - 0.012
    # the bezel: its front face in to the opening, a chamfer, flat sides, closed behind
    loft(b, [at_z(rrect(w + 0.024, h + 0.024, 0.03, 0, mid), front), at_z(rrect(OUT_W - 0.014, OUT_H - 0.014, 0.01, 0, OUT_Y), front),
             at_z(rrect(OUT_W, OUT_H, 0.012, 0, OUT_Y), front + 0.009), at_z(rrect(OUT_W, OUT_H, 0.012, 0, OUT_Y), front + block),
             at_z(rrect(OUT_W - 0.03, OUT_H - 0.03, 0.01, 0, OUT_Y), front + block)], skin("PlasticBeige"), last=MODE != "low")   # (the shell has no use for a face nobody sees)
    # what shows in the seam
    loft(b, [at_z(rrect(OUT_W - 0.02, OUT_H - 0.02, 0.01, 0, OUT_Y), front + block - 0.004),
             at_z(rrect(OUT_W - 0.02, OUT_H - 0.02, 0.01, 0, OUT_Y), z0 + 0.004)], "PlasticDark")   # (not of the baked skin: it is in the gap)
    # the housing
    loft(b, [at_z(rrect(OUT_W - 0.03, OUT_H - 0.03, 0.01, 0, OUT_Y), z0), at_z(rrect(OUT_W - 0.006, OUT_H - 0.006, 0.012, 0, OUT_Y), z0),
             at_z(rrect(OUT_W - 0.006, OUT_H - 0.006, 0.012, 0, OUT_Y), z0 + run), at_z(rrect(back_w, back_h, 0.012, 0, back_y), z0 + run + taper),
             at_z(rrect(back_w - 0.016, back_h - 0.016, 0.008, 0, back_y), z0 + run + taper + 0.01)], skin("PlasticCase"), first=MODE != "low", last=True)
    if MODE != "low":
        # slots for the tube's heat in the housing's narrowing faces: both sides, and the top
        side0, side1 = (OUT_W - 0.006) / 2, back_w / 2
        top0, top1 = OUT_Y + (OUT_H - 0.006) / 2, back_y + back_h / 2
        lean_side = math.degrees(math.atan2(side0 - side1, taper))
        lean_top = math.degrees(math.atan2(top0 - top1, taper))
        for i in range(10):
            t = (i + 1.0) / 11.0
            z = z0 + run + taper * t
            x, y = side0 + (side1 - side0) * t, OUT_Y + (back_y - OUT_Y) * t
            tall = (OUT_H * (1 - 0.34 * t)) * 0.52
            for side in (-1, 1):
                b.box((side * (x - 0.0006), y, z), (0.003, tall, 0.011), "PlasticDark", bevel=0.001, segs=1, rot=(0, -side * lean_side, 0))
            b.box((0, top0 + (top1 - top0) * t - 0.0006, z), (OUT_W * (1 - 0.38 * t) * 0.6, 0.003, 0.011), "PlasticDark", bevel=0.001, segs=1, rot=(lean_top, 0, 0))
        for side in (-1, 1):   # a hand grip let into each side, low: it weighs 53 kg
            b.box((side * (side0 - 0.001), LOW + 0.085, z0 + 0.045), (0.004, 0.04, 0.075), "PlasticDark", bevel=0.0015, segs=1)
            b.box((side * (side0 + 0.0015), LOW + 0.108, z0 + 0.045), (0.006, 0.008, 0.085), "PlasticCase", bevel=0.003, segs=2)
        # behind: the sockets the leads go into, and the maker's label
        terminal_board(b, BOARD_X, back_y - BOARD_DOWN, z0 + run + taper + 0.0102)
        mains_socket(b, -0.14, back_y - BOARD_DOWN, z0 + run + taper + 0.0102)
        legend(b, "acin", -0.14, back_y - BOARD_DOWN + 0.016, z0 + run + taper + 0.0105)
        b.quad((0, back_y + 0.012, z0 + run + taper + 0.0105), (0.2, 0.109), "Details", rot=(0, 180, 0), decal="crt_rear")   # under the fans, over the board
        for sx in (-1, 1):
            for sy in (-1, 1):
                b.cyl((sx * (back_w / 2 - 0.035), back_y + sy * (back_h / 2 - 0.035), z0 + run + taper + 0.0102), 0.0045, 0.003, "MetalSteel", segs=8, rot=(90, 0, 0))
        for x in (-0.1, 0.1):   # two fans draw the tube's heat out behind
            fan_grille(b, x, back_y + 0.125, z0 + run + taper + 0.0106, 0.038)
    # the opening's own slope, down to the glass
    def on_glass(outline):   # an outline laid on the tube's own surface, a hair before it
        return [(x, y, front + 0.0115 + BULGE - math.sqrt(BULGE * BULGE - x * x - (y - mid) ** 2)) for x, y in outline]
    loft(b, [at_z(rrect(w + 0.024, h + 0.024, 0.03, 0, mid), front), at_z(rrect(w + 0.008, h + 0.008, 0.025, 0, mid), front + 0.014),
             on_glass(rrect(w, h, 0.022, 0, mid))], skin("PlasticGrey"), normal=(0, 0, -1))
    if MODE == "full":
        # the tube: a patch of a ball, a little larger than what shows, its middle the nearest
        nx, ny, gw, gh = 24, 18, w + 0.012, h + 0.012
        vertices, faces, uvs = [], [], []
        for j in range(ny + 1):
            for i in range(nx + 1):
                x, y = (i / nx - 0.5) * gw, (j / ny - 0.5) * gh
                sag = BULGE - math.sqrt(BULGE * BULGE - x * x - y * y)
                vertices.append((x, mid + y, front + 0.012 + sag))
        for j in range(ny):
            for i in range(nx):
                a = j * (nx + 1) + i
                faces.append((a, a + 1, a + nx + 2, a + nx + 1))
                uvs.append([(vertices[k][0] / w + 0.5, (vertices[k][1] - mid) / h + 0.5) for k in faces[-1]])
        b.mesh(vertices, faces, uvs, "TubeGlass", normal=(0, 0, -1))
    if MODE != "low":
        # under the glass, as NEC's manual lists the front: POWER and its indicator (green when on),
        # the remote's window, and six keys (RGB 2/+, RGB 1/-, VIDEO 2, VIDEO 1, EXIT, PROCEED); and
        # behind grilles in the chin's corners, its two oval loudspeakers of 9 x 5.5 cm
        round_button(b, 0.18, CHIN, front, 0.011, "PlasticBeige")
        b.cyl((0.155, CHIN, front - 0.0005), 0.0035, 0.003, "Led", segs=10, rot=(90, 0, 0))
        for i in range(6):
            b.box((0.02 + i * 0.022, CHIN, front - 0.0012), (0.016, 0.008, 0.0036), "PlasticGrey", bevel=0.0014, segs=2)
        b.quad((0.075, CHIN - 0.011, front - 0.0002), (0.145, 0.007), "Details", decal="crt_osd")
        b.box((-0.06, CHIN, front - 0.0008), (0.124, 0.022, 0.0016), "MetalSteel", bevel=0.0006, segs=1)   # the maker's plate
        b.quad((-0.06, CHIN, front - 0.0018), (0.12, 0.0196), "Details", decal="crt_brand")
        b.box((-0.165, CHIN, front - 0.0006), (0.03, 0.012, 0.0012), "PlasticDark", bevel=0.0004, segs=1)   # the remote control's window
        for side in (-1, 1):
            b.quad((side * 0.262, CHIN, front - 0.0002), (0.09, 0.055), "Details", decal="crt_speaker")
    # the foot it turns and tilts on
    b.lathe((0, 0, 0.1), [(0, 0), (0.2, 0), (0.208, 0.008), (0.2, 0.02), (0.13, 0.034), (0.11, 0.042), (0, 0.042)], skin("PlasticBeige"), segs=40)
    b.lathe((0, 0.04, 0.1), [(0.105, 0), (0.115, 0.014), (0.13, LOW - 0.04 - 0.012), (0.14, LOW - 0.04 + 0.03), (0, LOW - 0.04 + 0.03)], skin("PlasticCase"), segs=40)
    return z0 + run + taper + 0.011, back_y   # where its back is


def tube_glass(b):
    """The tube's face alone, as monitor() makes it."""
    w, h, mid, front = GLASS
    nx, ny, gw, gh = 24, 18, w + 0.012, h + 0.012
    vertices, faces, uvs = [], [], []
    for j in range(ny + 1):
        for i in range(nx + 1):
            x, y = (i / nx - 0.5) * gw, (j / ny - 0.5) * gh
            vertices.append((x, mid + y, front + 0.012 + BULGE - math.sqrt(BULGE * BULGE - x * x - y * y)))
    for j in range(ny):
        for i in range(nx):
            a = j * (nx + 1) + i
            faces.append((a, a + 1, a + nx + 2, a + nx + 1))
            uvs.append([(vertices[k][0] / w + 0.5, (vertices[k][1] - mid) / h + 0.5) for k in faces[-1]])
    b.mesh(vertices, faces, uvs, "TubeGlass", normal=(0, 0, -1))


def legend(b, name, x, y, z, back=True):
    """A control's or socket's legend (world/pc_faces.py), its middle at x, y: on a back, which
    looks towards +z, or on a front."""
    u0, v0, u1, v1 = b.atlas["lg_" + name]
    size = ((u1 - u0) * 2048 + 1) * pc_faces.LEGEND_M, ((v1 - v0) * 2048 + 1) * pc_faces.LEGEND_M
    b.quad((x, y, z), size, "Details", rot=(0, 180, 0) if back else (0, 0, 0), decal="lg_" + name)


def d_ring(cx, cy, z, top, high, cut=0.001):
    """A D-subminiature shell's outline: wider above, its sides leaning 10 degrees, corners cut."""
    low = top - 2 * high * math.tan(math.radians(10))
    corners = [(-top / 2, high / 2), (top / 2, high / 2), (low / 2, -high / 2), (-low / 2, -high / 2)]
    ring = []
    for i, (px, py) in enumerate(corners):
        for qx, qy in (corners[i - 1], corners[(i + 1) % 4]):
            dx, dy = qx - px, qy - py
            length = math.hypot(dx, dy)
            ring.append((cx + px + dx / length * cut, cy + py + dy / length * cut, z))
    return ring


# a D-sub: its flange's width, its shell's width above, its posts apart, pins a row, their pitch, rows apart
DSUB = {"9": (0.03081, 0.01692, 0.02499, (5, 4), 0.00277, 0.00284), "15hd": (0.03081, 0.01692, 0.02499, (5, 5, 5), 0.00229, 0.00198),
        "15": (0.03914, 0.02525, 0.03332, (8, 7), 0.00277, 0.00284), "25": (0.05304, 0.03896, 0.04704, (13, 12), 0.00277, 0.00284)}


def dsub(b, x, y, z, kind, pins=False, insert="PlasticDark"):
    """A D-subminiature connector in a face that looks towards +z: flange 12.55 mm high, shell
    8.36, two posts; holes in its insert, or pins standing in it."""
    flange, top, posts, rows, pitch, apart = DSUB[kind]
    b.box((x, y, z + 0.0004), (flange, 0.01255, 0.0008), "MetalSteel", bevel=0.0003, segs=1)
    loft(b, [d_ring(x, y, z + 0.0008, top, 0.00836), d_ring(x, y, z + 0.0066, top, 0.00836)], "MetalSteel")
    face = z + (0.0018 if pins else 0.0058)
    b.mesh(d_ring(x, y, face, top - 0.0016, 0.0068, 0.0008), [tuple(range(8))], [[(0.5, 0.5)] * 8], insert, normal=(0, 0, 1))
    for row, count in enumerate(rows):
        ry = y + ((len(rows) - 1) / 2 - row) * apart
        for i in range(count):
            px = x + (i - (count - 1) / 2) * pitch
            if pins:
                b.cyl((px, ry, face + 0.002), 0.0005, 0.004, "Brass", segs=6, rot=(90, 0, 0))
            else:
                b.cyl((px, ry, face + 0.0002), 0.00055, 0.0004, "Rubber", segs=6, rot=(90, 0, 0))
    for k in (-1, 1):
        b.cyl((x + k * posts / 2, y, z + 0.003), 0.0026, 0.005, "MetalSteel", segs=6, rot=(90, 0, 0))


def bnc(b, x, y, z):
    """A BNC socket: a nut, a barrel 9.7 mm across with two lugs, its white insert and pin."""
    b.cyl((x, y, z + 0.00125), 0.0072, 0.0025, "MetalSteel", segs=6, rot=(90, 0, 0))
    b.cyl((x, y, z + 0.0065), 0.00483, 0.011, "MetalSteel", segs=16, rot=(90, 0, 0))
    b.box((x, y, z + 0.009), (0.0125, 0.002, 0.002), "MetalSteel")
    b.cyl((x, y, z + 0.0121), 0.0036, 0.0004, "Ceramic", segs=12, rot=(90, 0, 0))
    b.cyl((x, y, z + 0.0124), 0.0011, 0.0006, "Brass", segs=8, rot=(90, 0, 0))


def phono(b, x, y, z, mat):
    """A phono socket: a sleeve 8.3 mm across in a ring of its channel's colour."""
    b.lathe((x, y, z + 0.0002), [(0.0042, 0), (0.0042, 0.002), (0.0066, 0.002), (0.0066, 0)], mat, segs=14, rot=(90, 0, 0), cap=False)
    b.cyl((x, y, z + 0.0047), 0.00415, 0.009, "MetalSteel", segs=14, rot=(90, 0, 0))
    b.cyl((x, y, z + 0.0093), 0.0016, 0.0004, "Rubber", segs=8, rot=(90, 0, 0))


def mains_socket(b, x, y, z, inlet=True):
    """An IEC 60320 appliance coupler in a face that looks towards +z: the inlet (C14) with its
    three blades, or the outlet (C13) for a monitor's lead. The earth is the middle one, above."""
    wide, high, c = 0.024, 0.016, 0.004
    b.box((x, y, z + 0.001), (0.0305, 0.0225, 0.002), "PlasticDark", bevel=0.0015, segs=1)
    shape = [(-wide / 2, -high / 2), (wide / 2, -high / 2), (wide / 2, high / 2 - c), (wide / 2 - c, high / 2), (-wide / 2 + c, high / 2), (-wide / 2, high / 2 - c)]
    b.mesh([(x + px, y + py, z + 0.0022) for px, py in shape], [tuple(range(6))], [[(0.5, 0.5)] * 6], "Rubber" if inlet else "PlasticGrey", normal=(0, 0, 1))
    for dx, dy in ((-0.007, -0.002), (0.007, -0.002), (0, 0.002)):
        b.box((x + dx, y + dy, z + (0.004 if inlet else 0.0024)), (0.002, 0.004, 0.004 if inlet else 0.0004), "Brass" if inlet else "Rubber")


def din5(b, x, y, z):
    """The keyboard's socket: a 13.2 mm DIN, five contacts on half a circle, its key above."""
    b.lathe((x, y, z), [(0.0066, 0), (0.0066, 0.004), (0.0082, 0.004), (0.0082, 0)], "MetalSteel", segs=18, rot=(90, 0, 0), cap=False)
    b.cyl((x, y, z + 0.001), 0.0066, 0.002, "PlasticDark", segs=18, rot=(90, 0, 0))
    for k in range(5):
        angle = math.radians(180 + k * 45)
        b.cyl((x + 0.0035 * math.cos(angle), y + 0.0035 * math.sin(angle), z + 0.0022), 0.0007, 0.0004, "Brass", segs=6, rot=(90, 0, 0))
    b.box((x, y + 0.0058, z + 0.0022), (0.002, 0.0016, 0.0006), "MetalSteel")


def jack(b, x, y, z):
    """A 3.5 mm socket and its nut."""
    b.cyl((x, y, z + 0.001), 0.004, 0.002, "MetalSteel", segs=6, rot=(90, 0, 0))
    b.cyl((x, y, z + 0.0022), 0.00175, 0.0004, "Rubber", segs=10, rot=(90, 0, 0))


# The monitor's terminal board, as one behind it sees it: its middle, and each socket's place
# right of that (+u) and above. A legend stands LABEL over its socket's middle.
BOARD_X, BOARD_DOWN, ROW_TOP, ROW_LOW, LABEL = 0.03, 0.1, 0.010, -0.022, 0.0102
BOARD = {"rgb1": (-0.085, ROW_TOP), "r": (-0.045, ROW_TOP), "g": (-0.021, ROW_TOP), "b": (0.003, ROW_TOP), "hcs": (0.027, ROW_TOP), "v": (0.051, ROW_TOP),
         "term": (0.088, ROW_TOP), "video": (-0.085, ROW_LOW), "audio_l": (-0.056, ROW_LOW), "audio_r": (-0.030, ROW_LOW),
         "lp": (0.020, ROW_LOW), "lm": (0.036, ROW_LOW), "rp": (0.052, ROW_LOW), "rm": (0.068, ROW_LOW)}


def terminal_board(b, x, y, z):
    """The monitor's inputs on a dark board that looks towards +z, middle at x, y: a 15-pin socket
    (RGB 1), five BNC (RGB 2: red, green, blue, two syncs), the 75 ohm switches; below them a BNC
    and two phono sockets, and four spring clips for outside loudspeakers. Each under its legend."""
    b.box((x, y, z + 0.0006), (0.22, 0.075, 0.0012), "PlasticDark", bevel=0.0004, segs=1)
    face = z + 0.0012
    for name, (u, v) in BOARD.items():
        sx, sy = x - u, y + v   # seen from behind, right is -x
        legend(b, name, sx, sy + LABEL, face + 0.0002)
        if name == "rgb1":
            dsub(b, sx, sy, face, "15hd", insert="GamepadBlue")
        elif name in ("r", "g", "b", "hcs", "v", "video"):
            bnc(b, sx, sy, face)
        elif name == "term":
            b.box((sx, sy, face + 0.0015), (0.022, 0.009, 0.003), "GamepadBlue", bevel=0.0008, segs=1)
            for i in range(4):
                b.box((sx - 0.0078 + i * 0.0052, sy + 0.001, face + 0.0033), (0.0026, 0.004, 0.0012), "Ceramic")
        elif name.startswith("audio"):
            phono(b, sx, sy, face, "Ceramic" if name == "audio_l" else "RedPaint")
        else:
            b.box((sx, sy - 0.001, face + 0.0035), (0.009, 0.013, 0.007), "RedPaint" if name in ("lp", "rp") else "PlasticDark", bevel=0.0015, segs=1)
    legend(b, "rgb2", x - BOARD["b"][0], y + ROW_TOP + LABEL + 0.0068, face + 0.0002)
    legend(b, "speaker", x - (BOARD["lm"][0] + BOARD["rp"][0]) / 2, y + ROW_LOW + LABEL + 0.0068, face + 0.0002)


def speed_display(b, x, y):
    """A 486's case shows its speed in megahertz on two digits of seven bars each: 66."""
    b.box((x, y, FRONT - 0.0008), (0.034, 0.022, 0.0016), "PlasticDark", bevel=0.0006, segs=1)
    bars = {"top": (0, 0.0062, 0.0058, 0.0012), "mid": (0, 0, 0.0058, 0.0012), "low": (0, -0.0062, 0.0058, 0.0012),
            "upper left": (-0.0034, 0.0031, 0.0012, 0.005), "lower left": (-0.0034, -0.0031, 0.0012, 0.005), "lower right": (0.0034, -0.0031, 0.0012, 0.005)}
    for digit in (-0.0075, 0.0075):   # a 6 lights all but the upper right bar
        for dx, dy, w, h in bars.values():
            b.box((x + digit + dx, y + dy, FRONT - 0.0018), (w, h, 0.0006), "Amber")


def drive_gap(b, x, y, w, h):
    """The dark line round a drive's face, where it meets the case."""
    for dx, dy, sw, sh in ((0, h / 2, w, 0.0012), (0, -h / 2, w, 0.0012), (w / 2, 0, 0.0012, h), (-w / 2, 0, 0.0012, h)):
        b.box((x + dx, y + dy, FRONT - 0.0002), (sw + 0.0012, sh, 0.0012), "PlasticDark")


def cd_drive(b, cx, cy):
    """A CD-ROM drive's face: its tray, the key that opens it, a lamp, the wheel and the socket for headphones."""
    cw, ch = BAY_525[0] + 0.002, BAY_525[1] + 0.001   # the face a little over the bay's opening
    drive_gap(b, cx, cy, cw, ch)
    b.box((cx, cy, FRONT - 0.0008), (cw - 0.002, ch - 0.002, 0.002), "PlasticBeige", bevel=0.0008, segs=1)
    b.box((cx, cy + 0.008, FRONT - 0.0028), (0.130, 0.016, 0.003), "PlasticBeige", bevel=0.0012, segs=2)       # the tray: a disc is 120 mm
    b.box((cx, cy + 0.0005, FRONT - 0.0018), (0.130, 0.0008, 0.001), "PlasticDark")
    b.box((cx + 0.054, cy - 0.012, FRONT - 0.003), (0.018, 0.006, 0.003), "PlasticCase", bevel=0.0012, segs=2)       # eject
    b.box((cx + 0.034, cy - 0.012, FRONT - 0.002), (0.006, 0.003, 0.002), "Amber")
    b.cyl((cx - 0.058, cy - 0.012, FRONT - 0.0012), 0.0032, 0.002, "PlasticDark", segs=12, rot=(90, 0, 0))            # headphones
    b.cyl((cx - 0.058, cy - 0.012, FRONT - 0.0016), 0.0048, 0.0012, "MetalSteel", segs=12, rot=(90, 0, 0))
    b.cyl((cx - 0.038, cy - 0.012, FRONT - 0.001), 0.006, 0.004, "PlasticDark", segs=16, rot=(0, 0, 90))              # the volume's wheel, on edge
    b.cyl((cx + 0.012, cy - 0.012, FRONT - 0.0008), 0.0008, 0.001, "PlasticDark", segs=6, rot=(90, 0, 0))             # the hole for a paper clip
    b.quad((cx - 0.02, cy + 0.008, FRONT - 0.0044), (0.06, 0.008), "Details", decal="pc_cd")


def floppy_drive(b, fx, fy):
    """A floppy drive's face: the slot with its flap, the eject key, a lamp."""
    fw, fh = BAY_35
    drive_gap(b, fx, fy, fw, fh)
    b.box((fx, fy, FRONT - 0.0008), (fw - 0.002, fh - 0.002, 0.002), "PlasticBeige", bevel=0.0008, segs=1)
    b.box((fx, fy + 0.004, FRONT - 0.0019), (0.09, 0.0045, 0.001), "PlasticDark")
    b.box((fx, fy + 0.0052, FRONT - 0.0022), (0.088, 0.0016, 0.001), "PlasticGrey")
    b.box((fx + 0.034, fy - 0.006, FRONT - 0.0045), (0.012, 0.006, 0.005), "PlasticCase", bevel=0.0012, segs=2)
    b.box((fx - 0.04, fy - 0.006, FRONT - 0.002), (0.005, 0.0025, 0.002), "Led")


def fan_grille(b, x, y, z, radius):
    """A fan's opening in a face that looks towards +z: rings and spokes of wire, four screws."""
    b.cyl((x, y, z), radius, 0.0012, "PlasticDark", segs=32, rot=(90, 0, 0))
    for k in range(1, 5):
        r = radius * k / 4.2
        b.lathe((x, y, z + 0.0004), [(r - 0.001, 0), (r - 0.001, 0.0016), (r + 0.001, 0.0016), (r + 0.001, 0)], "MetalSteel", segs=32, rot=(90, 0, 0), cap=False)
    for k in range(4):
        b.box((x, y, z + 0.0012), (radius * 2, 0.002, 0.0016), "MetalSteel", rot=(0, 0, k * 45))
    for sx in (-1, 1):
        for sy in (-1, 1):   # an 80 mm fan's screws are 71.5 mm apart
            b.cyl((x + sx * radius * 0.94, y + sy * radius * 0.94, z + 0.0006), 0.0028, 0.002, "MetalSteel", segs=8, rot=(90, 0, 0))


def tower_back(h=TOWER_HIGH):
    """Where things are on the tower's back, from its middle and its foot: the power supply's
    middle, the row of the board's sockets, the first slot, and each socket a lead goes to."""
    py = h - 0.012 - 0.043
    sy = py - 0.065
    first = sy - 0.028
    return {"supply": py, "row": sy, "first": first, "keyboard": (-0.074, sy), "com1": (-0.040, sy), "com2": (-0.004, sy), "printer": (0.046, sy),
            "video": (-0.02, first - SLOT_PITCH), "inlet": (-0.052, py + 0.022)}


def rear(b, w, h, back):
    """The tower's back, which a visitor behind the table sees: bare steel; the power supply's
    own plate (PS/2 form, 150 x 86 mm) with its fan, the mains inlet, the monitor's outlet and
    the voltage switch; the board's sockets under their legends; seven slots' brackets, a video
    card's socket in the second and a sound card's in the fourth; screws."""
    z = back + 0.0006
    at = tower_back(h)
    b.box((0, h / 2 + 0.006, back + 0.0002), (w - 0.006, h - 0.02, 0.001), "MetalSteel")
    py = at["supply"]
    b.box((0, py, z + 0.0002), (0.150, 0.086, 0.0012), "MetalSteel", bevel=0.0004, segs=1)
    fan_grille(b, 0.03, py, z + 0.0008, 0.038)
    mains_socket(b, at["inlet"][0], at["inlet"][1], z + 0.0008)
    mains_socket(b, -0.052, py - 0.005, z + 0.0008, inlet=False)
    b.box((-0.052, py - 0.031, z + 0.002), (0.016, 0.01, 0.004), "RedPaint", bevel=0.0015, segs=2)   # 115 / 230 V
    b.box((-0.049, py - 0.031, z + 0.0042), (0.006, 0.006, 0.001), "PlasticDark")
    legend(b, "volts", -0.027, py - 0.031, z + 0.0009)
    for x, y in ((-0.071, py + 0.039), (0.071, py + 0.039), (-0.071, py - 0.039), (0.071, py - 0.039)):
        b.cyl((x, y, z + 0.001), 0.0032, 0.002, "MetalSteel", segs=8, rot=(90, 0, 0))
    # the board's sockets: the keyboard's DIN, two serial (9 pins standing), the printer's (25 holes)
    for name in ("keyboard", "com1", "com2", "printer"):
        x, y = at[name]
        legend(b, name, x, y + LABEL, z + 0.0004)
        if name == "keyboard":
            din5(b, x, y, z)
        else:
            dsub(b, x, y, z, "25" if name == "printer" else "9", pins=name != "printer")
    # seven slots, 0.8 inch apart, a bracket and its screw in each
    for i in range(7):
        y = at["first"] - i * SLOT_PITCH
        b.box((-0.005, y, z + 0.0006), (BRACKET[0], BRACKET[1], 0.0014), "MetalSteel", bevel=0.0005, segs=1)
        b.cyl((0.064, y, z + 0.0012), 0.003, 0.002, "MetalSteel", segs=8, rot=(90, 0, 0))
        if i == 1:      # the video card
            dsub(b, at["video"][0], y, z + 0.0012, "15hd", insert="GamepadBlue")
        elif i == 3:    # the sound card: three sockets and the joystick's 15 pins
            dsub(b, -0.03, y, z + 0.0012, "15")
            for k in range(3):
                jack(b, 0.012 + k * 0.012, y, z + 0.0012)
        else:
            b.box((-0.005, y, z + 0.0015), (0.098, 0.0036, 0.0004), "PlasticDark")   # a blank's slot for air
    for x in (-0.084, 0.084):   # the cover's screws down each side
        for y in (0.03, h * 0.4, h - 0.16):
            b.cyl((x, y, z + 0.0006), 0.0038, 0.0022, "MetalSteel", segs=8, rot=(90, 0, 0))


def tower(b):
    """The computer itself, standing on the floor: its foot at the frame's origin, its front at FRONT."""
    w, h, deep = 0.18, TOWER_HIGH, TOWER_DEEP
    back = FRONT + deep
    # the steel cover, the plastic front with rounded edges, the feet
    b.box((0, h / 2 + 0.006, FRONT + 0.012 + (deep - 0.012) / 2), (w, h - 0.012, deep - 0.012), skin("PlasticCase"), bevel=0.0025, segs=1)   # a folded steel cover: one chamfer, flat faces
    loft(b, [at_z(rrect(w + 0.004, h, 0.008, 0, h / 2 + 0.006), FRONT + 0.016), at_z(rrect(w + 0.004, h, 0.01, 0, h / 2 + 0.006), FRONT + 0.004),
             at_z(rrect(w - 0.002, h - 0.006, 0.01, 0, h / 2 + 0.006), FRONT)], skin("PlasticBeige"), last=True, normal=(0, 0, -1))
    lip = at_z(rrect(w + 0.004, h, 0.008, 0, h / 2 + 0.006), FRONT + 0.016)   # the front's back, where it stands out round the cover
    b.mesh(lip, [tuple(range(len(lip)))], [[(x / 0.3, y / 0.3) for x, y, z in lip]], skin("PlasticBeige"), normal=(0, 0, 1))
    for x in (-0.07, 0.07):
        for z in (FRONT + 0.05, back - 0.05):
            b.cyl((x, 0.003, z), 0.014, 0.006, skin("Rubber"), segs=14)
    if MODE == "low":
        return
    rear(b, w, h, back)
    for i in range(12):   # the slots in its side
        b.box((-w / 2 - 0.0003, 0.12, FRONT + 0.16 + i * 0.014), (0.001, 0.12, 0.006), "PlasticDark")
    # from the top: the CD-ROM drive, a blank bay of its size, the floppy drive, all on the middle line
    cd_drive(b, 0, h - 0.04)
    drive_gap(b, 0, h - 0.088, BAY_525[0] + 0.002, BAY_525[1] + 0.001)
    b.box((0, h - 0.088, FRONT - 0.0006), (BAY_525[0], BAY_525[1] - 0.001, 0.0016), "PlasticBeige", bevel=0.0008, segs=1)
    floppy_drive(b, 0, h - 0.132)
    # the panel: the lock, three lamps over their names, turbo and reset over theirs, the speed
    cy = h - 0.175
    b.box((0, cy, FRONT - 0.0004), (0.156, 0.034, 0.001), "PlasticCase", bevel=0.0004, segs=1)
    b.cyl((-0.06, cy, FRONT - 0.002), 0.008, 0.004, "MetalSteel", segs=20, bevel=0.001, rot=(90, 0, 0))
    b.box((-0.06, cy, FRONT - 0.0042), (0.002, 0.008, 0.0006), "PlasticDark")
    for x, mat, name in ((-0.034, "Led", "power"), (-0.018, "Amber", "turbo"), (-0.002, "Led", "hdd")):
        b.box((x, cy + 0.005, FRONT - 0.0016), (0.007, 0.003, 0.002), mat, bevel=0.0006, segs=1)
        legend(b, name, x, cy - 0.005, FRONT - 0.0011, back=False)
    for x, name in ((0.016, "turbo"), (0.034, "reset")):
        b.box((x, cy + 0.004, FRONT - 0.003), (0.013, 0.008, 0.004), "PlasticBeige", bevel=0.0016, segs=2)
        legend(b, name, x, cy - 0.007, FRONT - 0.0011, back=False)
    speed_display(b, 0.059, cy)
    # under it the power key in its well, at the right, over its name; the badge at the left
    px, py = 0.045, h - 0.222
    b.box((px, py, FRONT - 0.0004), (0.04, 0.03, 0.001), "PlasticCase", bevel=0.0004, segs=1)
    b.box((px, py, FRONT - 0.004), (0.034, 0.024, 0.006), "PlasticBeige", bevel=0.003, segs=3)
    b.box((px, py, FRONT - 0.0072), (0.016, 0.0016, 0.0006), "PlasticGrey")
    legend(b, "power", px, py - 0.019, FRONT - 0.0003, back=False)
    b.box((-0.045, py, FRONT - 0.0012), (0.036, 0.014, 0.0016), "MetalSteel", bevel=0.0006, segs=1)
    b.quad((-0.045, py, FRONT - 0.0022), (0.032, 0.0128), "Details", decal="pc_badge")
    for row in range(2):   # the grille: two rows of slots, centred
        for i in range(16):
            b.box((-0.06375 + i * 0.0085, 0.04 + row * 0.038, FRONT - 0.0002), (0.0045, 0.028, 0.0012), "PlasticDark", bevel=0.0004, segs=1)


def keyboard(b):
    """The keyboard: world/keyboard.json's layout at a real keyboard's size, caps that
    narrow to their tops, in a tray that slopes."""
    layout = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "keyboard.json"), encoding="utf-8"))
    wide, tall, s = layout["width"], layout["height"], 0.01905 / 60   # the layout's keys are 60 apart: 19.05 mm, the standard pitch
    vertices, faces, uvs, normals = [], [], [], []

    def ring(x0, y0, x1, y1, z):
        first = len(vertices)
        for px, py in ((x0, y1), (x1, y1), (x1, y0), (x0, y0)):
            vertices.append(((px - wide / 2) * s, (tall / 2 - py) * s, z))
        return first

    def face(corners, normal):
        faces.append(tuple(corners))
        uvs.append([(vertices[i][0] / (s * wide) + 0.5, vertices[i][1] / (s * tall) + 0.5) for i in corners])
        normals.append(normal)
    row0 = min(key[1] for key in layout["keys"] if len(key[4]) == 1) - 60   # the number row is the second
    for x0, y0, w, h, legend in layout["keys"]:
        if legend.startswith("Use my"):
            continue   # the console's own switch
        base = ring(x0 + 1.5, y0 + 1.5, x0 + w - 1.5, y0 + h - 1.5, 0)
        shoulder = ring(x0 + 4.0, y0 + 3.0, x0 + w - 4.0, y0 + h - 6.0, -0.0065)
        # rows are sculpted: the top and the bottom rows stand a little higher than the home row
        rise = (0.0014, 0.0016, 0.0008, 0.0, 0.0006, 0.0014)[min(5, max(0, int(round((y0 - row0) / 60.0))))]
        top = ring(x0 + 6.0, y0 + 4.5, x0 + w - 6.0, y0 + h - 8.0, -0.0078 - rise)
        face((top, top + 1, top + 2, top + 3), (0, 0, -1))
        for lower, upper in ((base, shoulder), (shoulder, top)):
            for k, normal in enumerate(((0, -1, 0), (1, 0, 0), (0, 1, 0), (-1, 0, 0))):
                face((lower + k, lower + (k + 1) % 4, upper + (k + 1) % 4, upper + k), normal)
    kw, kh = wide * s, tall * s
    with b.at((0, 0.016, -0.31), (90 - 7, 0, 0)):   # -z of this frame is up from the keys
        with b.at((0, 0, -0.012)):
            b.mesh(vertices, faces, uvs, "Keyboard", normal=normals)
        loft(b, [at_z(rrect(kw + 0.03, kh + 0.03, 0.01), 0.012), at_z(rrect(kw + 0.03, kh + 0.03, 0.012), -0.006),
                 at_z(rrect(kw + 0.022, kh + 0.022, 0.012), -0.0105), at_z(rrect(kw + 0.008, kh + 0.008, 0.006), -0.0105)], "PlasticBeige",
             first=True, normal='out')
        b.quad((0, 0, -0.0104), (kw + 0.008, kh + 0.008), "PlasticCase")   # the well the keys stand in
        for i, mat in enumerate(("Led", "PlasticDark", "PlasticDark")):
            b.box((kw / 2 - 0.02 - i * 0.012, kh / 2 + 0.006, -0.0108), (0.006, 0.002, 0.0008), mat)
        b.quad((kw / 2 - 0.032, kh / 2 + 0.0105, -0.0106), (0.04, 0.005), "Details", decal="kb_lamps")
    for x in (-0.18, 0.18):   # the feet that fold out behind
        b.box((x, 0.008, -0.242), (0.03, 0.016, 0.008), "PlasticCase", bevel=0.002, segs=1)


def mouse(b):
    """A mouse with two buttons on a mat."""
    b.box((0, 0.0015, 0), (0.21, 0.003, 0.25), "Rubber", bevel=0.001, segs=1)   # the mat, lying flat
    mouse_body(b)


def mouse_body(b):
    """The mouse itself, standing on y = 0.004: sections of a pebble, flat below, humped under the
    palm, its buttons away from the sitter (+z)."""
    def section(t, lift=0.0, around=28, half_only=False):
        z = 0.052 - t * 0.104                         # from its tail (towards the sitter's wrist) to the buttons' edge
        half = 0.031 * (1 - (2 * t - 1) ** 4) ** 0.5 + 0.002 + lift
        high = (0.012 + 0.024 * math.sin(math.pi * (0.12 + 0.72 * (1 - t))) ** 1.2) * (0.25 + 0.75 * (1 - (2 * t - 1) ** 8) ** 0.5) + lift   # its ends rounded over, not cut off
        ring = []
        for a in range(around // 2 + 1 if half_only else around):
            angle = 2 * math.pi * a / around
            x, y = math.cos(angle), math.sin(angle)
            ring.append((x * half, 0.004 + (high * max(y, 0) ** 0.8 if y > 0 else 0.002 * y), z))
        return ring
    square = [(0, 0), (0.02, 0), (0.02, 0.02), (0, 0.02)]
    with b.at((0.0, 0, 0.0), (0, 192, 0)):   # its buttons away from the sitter, turned a little as a hand leaves it
        rings = [section(t) for t in (0.0, 0.025, 0.07, 0.14, 0.24, 0.36, 0.5, 0.64, 0.76, 0.86, 0.93, 0.975, 1.0)]
        loft(b, rings, "PlasticBeige", first=True, last=True, normal='out', tile=0.2)
        # the seams are ribbons a hair over the skin: across behind the buttons, and between them
        near, far = section(0.51, 0.0003, half_only=True), section(0.525, 0.0003, half_only=True)
        n = len(near)
        b.mesh(near + far, [(k, k + 1, n + k + 1, n + k) for k in range(1, n - 2)], [square] * (n - 3), "PlasticDark", normal=(0, 1, 0))
        ridge = [section(t, 0.0003)[7] for t in (0.525, 0.64, 0.76, 0.86, 0.93, 0.975)]   # the top of each section
        left = [(-0.0006, y, z) for x, y, z in ridge]
        right = [(0.0006, y, z) for x, y, z in ridge]
        n = len(ridge)
        b.mesh(left + right, [(k, k + 1, n + k + 1, n + k) for k in range(n - 1)], [square] * (n - 1), "PlasticDark", normal=(0, 1, 0))


def office_chair(b):
    """An office chair of the decade at the frame's origin, its seat's front towards +z: a five
    star base on twin-wheel casters, a gas lift, a tilting seat, a back on a bent bar, arms."""
    for i in range(5):
        with b.at((0, 0, 0), (0, i * 72 + 18, 0)):
            # a spoke, lower at its tip, and the socket there that a caster's stem goes up into
            b.box((0, 0.098, 0.165), (0.05, 0.03, 0.29), "MetalBlack", bevel=0.01, segs=2, rot=(6, 0, 0))
            b.cyl((0, 0.078, 0.305), 0.02, 0.046, "MetalBlack", segs=14, bevel=0.005)
            b.cyl((0, 0.056, 0.305), 0.006, 0.02, "MetalSteel", segs=8)
            with b.at((0, 0, 0.305), (0, 25 + i * 31, 0)):   # each caster trails its own way
                b.box((0, 0.044, 0.012), (0.03, 0.012, 0.05), "PlasticDark", bevel=0.005, segs=2)        # the fork's bridge
                b.prism((0, 0, 0), [(-0.012, 0.05), (0.04, 0.05), (0.052, 0.034), (0.046, 0.022), (0.0, 0.03)], 0.012, "PlasticDark", plane='zy', bevel=0.002)
                for side in (-1, 1):
                    b.cyl((side * 0.016, 0.026, 0.024), 0.026, 0.014, "Rubber", segs=18, bevel=0.004, rot=(0, 0, 90))
                    b.cyl((side * 0.0235, 0.026, 0.024), 0.012, 0.002, "PlasticGrey", segs=12, rot=(0, 0, 90))
                b.cyl((0, 0.026, 0.024), 0.004, 0.05, "MetalSteel", segs=8, rot=(0, 0, 90))            # the axle
    # the hub and the lift: three sleeves, each inside the one below, and the bright piston
    b.lathe((0, 0.07, 0), [(0, 0), (0.05, 0), (0.058, 0.012), (0.058, 0.05), (0.045, 0.062), (0, 0.062)], "MetalBlack", segs=24)
    for radius, y0, y1 in ((0.036, 0.13, 0.22), (0.031, 0.22, 0.29), (0.026, 0.29, 0.35)):
        b.cyl((0, (y0 + y1) / 2, 0), radius, y1 - y0, "PlasticDark", segs=20, bevel=0.003)
    b.cyl((0, 0.375, 0), 0.014, 0.07, "MetalSteel", segs=14)
    # the mechanism under the seat, with the lever that lets it down
    b.box((0, 0.418, 0.0), (0.2, 0.034, 0.24), "MetalBlack", bevel=0.008, segs=2)
    b.tube([(0.08, 0.415, 0.04), (0.2, 0.41, 0.05), (0.27, 0.405, 0.05)], 0.006, "MetalBlack", segs=8, smooth=3)
    b.box((0.285, 0.404, 0.05), (0.045, 0.014, 0.03), "PlasticDark", bevel=0.005, segs=2)
    # the seat: a shell, and the cushion on it
    b.box((0, 0.447, 0.01), (0.47, 0.02, 0.45), "PlasticDark", bevel=0.008, segs=2)
    b.box((0, 0.495, 0.012), (0.49, 0.085, 0.47), "FabricChair", bevel=0.038, segs=5)
    # the back: a bar bent up behind the seat, a shell, a cushion that leans a little
    b.tube([(0, 0.42, -0.08), (0, 0.42, -0.24), (0, 0.45, -0.3), (0, 0.6, -0.315), (0, 0.8, -0.31)], 0.016, "MetalBlack", segs=10, smooth=5)
    with b.at((0, 0.87, -0.285), (-7, 0, 0)):
        b.box((0, 0, -0.028), (0.43, 0.45, 0.02), "PlasticDark", bevel=0.008, segs=2)
        b.box((0, 0, 0.01), (0.45, 0.47, 0.075), "FabricChair", bevel=0.034, segs=5)
    # the arms: a tube out from the mechanism and up, a pad on it
    for side in (-1, 1):
        b.tube([(side * 0.09, 0.425, -0.02), (side * 0.24, 0.43, -0.02), (side * 0.285, 0.46, -0.02), (side * 0.29, 0.56, -0.02), (side * 0.29, 0.655, -0.02)],
               0.012, "MetalBlack", segs=8, smooth=4)
        b.box((side * 0.29, 0.672, 0.01), (0.058, 0.03, 0.27), "PlasticDark", bevel=0.013, segs=3)


PLUG_LONG = 0.05   # a D-sub plug from the panel it is in to the end of its sleeve


def dsub_plug(b, x, y, z, wide, sleeve=0.006):
    """A lead's D-sub plug in a socket of a face that looks towards +z: its steel shell in the
    socket, the hood on the panel's own sockets' rims, two thumbscrews, the sleeve the lead leaves by."""
    b.box((x, y, z + 0.004), (wide - 0.013, 0.0095, 0.008), "MetalSteel", bevel=0.001, segs=1)
    b.box((x, y, z + 0.0215), (wide, 0.0155, 0.029), "PlasticCase", bevel=0.003, segs=2)
    for k in (-1, 1):
        b.cyl((x + k * (wide / 2 - 0.0035), y, z + 0.0385), 0.0032, 0.006, "PlasticCase", segs=10, rot=(90, 0, 0))
    b.cyl((x, y, z + 0.043), sleeve, 0.014, "PlasticCase", segs=12, bevel=0.001, rot=(90, 0, 0))


TABLE_HIGH = 0.765   # the classroom's table top over the floor (furniture.TABLE_TOP): the mains leads reach the floor
SET_X, TOWER_X, TOWER_Z = -0.11, 0.38, 0.05   # the monitor and keyboard a little left, the tower at their right


def station(b, which="all"):
    """A whole place on the table at the frame's origin: the monitor and keyboard, the tower
    beside them, the mouse before the tower, and their leads. which: "shell" is the monitor and
    the tower alone, "rest" all but them (with the tube's glass, which no mode but "full" gives)."""
    block, run, taper = 0.11 + 0.003, 0.09, 0.305
    back, plug_y = GLASS[3] + block + run + taper + 0.011, OUT_Y - 0.012   # the monitor's back, as monitor() builds it
    if which != "rest":
        with b.at((SET_X, 0, 0)):
            monitor(b)
        with b.at((TOWER_X, 0, TOWER_Z)):
            tower(b)
    if which == "shell":
        return
    with b.at((SET_X, 0, 0)):
        if MODE != "full":
            tube_glass(b)
        keyboard(b)
    with b.at((TOWER_X - 0.01, 0, -0.31)):
        mouse(b)
    behind = TOWER_Z + FRONT + TOWER_DEEP   # the tower's back
    edge = 0.445                            # the table's, behind
    at = tower_back()
    # the picture's lead: from RGB 1 on the monitor's board to the video card's socket, a plug at each end
    mx, my = SET_X + BOARD_X + 0.085, plug_y - BOARD_DOWN + ROW_TOP
    vx, vy = TOWER_X + at["video"][0], at["video"][1]
    b.tube([(mx, my, back + PLUG_LONG - 0.01), (mx, my, back + PLUG_LONG + 0.012), (mx + 0.012, my - 0.012, back + PLUG_LONG + 0.03),
            (mx + 0.05, my - 0.1, back + PLUG_LONG + 0.02), (mx + 0.1, 0.012, edge - 0.012), (TOWER_X - 0.16, 0.008, edge - 0.02),
            (vx, 0.012, behind + PLUG_LONG + 0.04), (vx, vy - 0.06, behind + PLUG_LONG + 0.035), (vx, vy - 0.006, behind + PLUG_LONG + 0.028),
            (vx, vy, behind + PLUG_LONG + 0.012), (vx, vy, behind + PLUG_LONG - 0.01)], 0.0045, "PlasticCase", segs=8, smooth=5)
    dsub_plug(b, mx, my, back, 0.032)
    dsub_plug(b, vx, vy, behind, 0.032)
    # each one's mains lead: out of its inlet, down onto the table, and over the edge behind
    floor, strip = -TABLE_HIGH, 0.06   # a strip of sockets on the floor behind the table: both leads end in it, its own goes on under the table
    b.box((strip, floor + 0.02, edge + 0.07), (0.3, 0.04, 0.055), "PlasticBeige", bevel=0.004, segs=2)
    b.box((strip + 0.125, floor + 0.041, edge + 0.07), (0.022, 0.004, 0.014), "RedPaint", bevel=0.001, segs=1)   # its lit switch
    b.tube([(strip - 0.15, floor + 0.02, edge + 0.07), (strip - 0.22, floor + 0.006, edge + 0.07), (strip - 0.3, floor + 0.005, edge + 0.03),
            (strip - 0.32, floor + 0.005, edge - 0.3)], 0.004, "Rubber", segs=8, smooth=4)
    for x, y, z, socket in ((SET_X - 0.14, plug_y - BOARD_DOWN, back, strip - 0.07), (TOWER_X + at["inlet"][0], at["inlet"][1], behind, strip + 0.04)):
        b.tube([(x, y, z), (x, y - 0.012, z + 0.035), (x, y * 0.45, z + 0.055), (x, 0.012, min(z + 0.075, edge - 0.02)), (x, 0.008, edge - 0.008),
                (x, -0.03, edge + 0.012), (x, -0.4, edge + 0.014), (x, floor + 0.14, edge + 0.016), (x * 0.7 + socket * 0.3, floor + 0.01, edge + 0.03),
                (x * 0.2 + socket * 0.8, floor + 0.1, edge + 0.06), (socket, floor + 0.1, edge + 0.07), (socket, floor + 0.06, edge + 0.07)], 0.0035, "Rubber", segs=8, smooth=4)
        b.box((socket, floor + 0.052, edge + 0.07), (0.036, 0.03, 0.036), "Rubber", bevel=0.006, segs=2)   # its plug, in the strip
        b.box((x, y, z + 0.014), (0.03, 0.022, 0.028), "Rubber", bevel=0.004, segs=2)   # its moulded plug (IEC C13)
    # the keyboard's lead round the monitor's foot to the DIN socket, the mouse's round the tower to COM 1
    kx, ky = TOWER_X + at["keyboard"][0], at["keyboard"][1]
    b.tube([(SET_X + 0.12, 0.02, -0.24), (SET_X + 0.2, 0.006, -0.2), (0.25, 0.004, -0.05), (0.262, 0.004, behind - 0.05),
            (kx, 0.012, behind + 0.07), (kx, ky - 0.06, behind + 0.07), (kx, ky - 0.006, behind + 0.062), (kx, ky, behind + 0.046), (kx, ky, behind + 0.02)],
           0.0028, "PlasticCase", segs=6, smooth=5)
    b.cyl((kx, ky, behind + 0.004), 0.0062, 0.008, "MetalSteel", segs=14, rot=(90, 0, 0))                    # the DIN plug: its shell in the socket,
    b.cyl((kx, ky, behind + 0.02), 0.0078, 0.026, "PlasticCase", segs=14, bevel=0.002, rot=(90, 0, 0))     # its grip,
    b.cyl((kx, ky, behind + 0.038), 0.0045, 0.012, "PlasticCase", segs=10, rot=(90, 0, 0))                 # and its sleeve
    cx, cy = TOWER_X + at["com1"][0], at["com1"][1]
    b.tube([(TOWER_X - 0.0, 0.014, -0.258), (TOWER_X + 0.01, 0.006, -0.22), (TOWER_X + 0.1, 0.004, -0.15), (TOWER_X + 0.108, 0.004, behind - 0.05),
            (cx + 0.05, 0.012, behind + PLUG_LONG + 0.03), (cx, cy - 0.06, behind + PLUG_LONG + 0.035), (cx, cy - 0.006, behind + PLUG_LONG + 0.028),
            (cx, cy, behind + PLUG_LONG + 0.012), (cx, cy, behind + PLUG_LONG - 0.01)], 0.0022, "PlasticCase", segs=6, smooth=5)
    dsub_plug(b, cx, cy, behind, 0.031, sleeve=0.004)
