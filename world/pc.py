# A desktop computer of the middle of the 1990s, for the classroom's eight places: a monitor of
# twice a 15 inch tube's diagonal, a keyboard and a mouse on the table, the tower on the floor.
# Everything is in a place's own frame: x to the sitter's right, y up from the table, the sitter
# at -z. The glass is a material of its own ("TubeGlass"), its picture's corners at the corners
# of what the bezel shows of it.
import json

import pc_faces
import math
import os
import random

from mathutils import Vector

import drape
import lib

FRONT = -0.150                            # the tower's front face, in its own frame
TOWER_HIGH, TOWER_DEEP = 0.36, 0.42
# Sizes looked up (docs/world/stations.md has where): a 29 inch tube with 27 inches seen (549 x 411 mm,
# NEC's XM29 Plus: an A68 tube of 108 degrees, stripes 0.60 mm apart at its middle); drive bays
# 146.1 x 41.3 and 101.6 x 26.1 mm; a PS/2 power supply's 150 x 86 mm back. The monitor's case is
# that monitor's own, 645 x 533 x 518 mm; the tower a mini tower's 180 x 360 x 420.
GLASS = (0.549, 0.411, 0.3625, -0.14)     # what shows of the tube: wide, high, its middle's height, its front
BULGE = 1.8                               # the glass is part of a ball of this radius: a flat square tube's face
OUT_W, OUT_H = 0.645, 0.533
LOW = 0.07                                # the case's underside, on its foot
OUT_Y = LOW + OUT_H / 2
CHIN = (LOW + GLASS[2] - GLASS[1] / 2 - 0.012) / 2
LED_X = 0.155   # the monitor's lamp, by its power key
BAY_525 = (0.1461, 0.0413)
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


def panel_uv(points, tile):
    """A face's picture as a panel's: laid flat along the axis its normal is nearest, upright along y (on a
    face that looks up or down, its far side up), so that one panel of a skin is one piece of cloth or
    moulding, unstretched, and its edges are seams where another panel begins."""
    n = len(points)
    normal = [sum((points[k][(a + 1) % 3] - points[k - 1][(a + 1) % 3]) * (points[k][(a + 2) % 3] + points[k - 1][(a + 2) % 3])
                  for k in range(n)) for a in range(3)]   # (Newell's)
    a = max(range(3), key=lambda i: abs(normal[i]))
    size = math.sqrt(sum(c * c for c in normal)) or 1.0
    if abs(normal[a]) < lib.OBLIQUE * size:   # (between the axes: in its own plane)
        return [(x / tile, y / tile) for x, y in lib.plane_uv(points, [c / size for c in normal])]
    s = 1.0 if normal[a] >= 0 else -1.0
    if a == 0:
        return [(-s * p[2] / tile, p[1] / tile) for p in points]
    if a == 1:
        return [(p[0] / tile, -s * p[2] / tile) for p in points]
    return [(s * p[0] / tile, p[1] / tile) for p in points]


def loft(b, rings, mat, first=True, last=True, normal=None, tile=0.3, ringwise=False):
    """A skin over rings of as many points each; first and last close its ends (closed unless told:
    world/check_closed.py wants every solid watertight). Its picture is laid in panels (panel_uv), or
    with `ringwise`, by length: round each ring by the ring's own outline, from ring to ring by how far apart
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
    along = [[0.0] * n]   # (each column along its own track: one mean for all sheared the narrow side's picture)
    for i in range(len(rings) - 1):
        along.append([along[-1][k] + apart(rings[i][k], rings[i + 1][k]) for k in range(n)])
    for i in range(len(rings) - 1):
        for k in range(n):
            l = (k + 1) % n
            faces.append((i * n + k, i * n + l, (i + 1) * n + l, (i + 1) * n + k))
            uvs.append([(round_ring[i][k] / tile, along[i][k] / tile), (round_ring[i][k + 1] / tile, along[i][l] / tile),
                        (round_ring[i + 1][k + 1] / tile, along[i + 1][l] / tile), (round_ring[i + 1][k] / tile, along[i + 1][k] / tile)])
    for closed, ring in ((first, 0), (last, len(rings) - 1)):
        if closed:   # an end is flat: its picture is laid flat on it, in its own plane (a ring standing up had its picture in a line)
            faces.append(tuple(ring * n + k for k in range(n)))
            points = [vertices[i] for i in faces[-1]]
            normal_ = [sum((points[k][(a + 1) % 3] - points[k - 1][(a + 1) % 3]) * (points[k][(a + 2) % 3] + points[k - 1][(a + 2) % 3])
                           for k in range(n)) for a in range(3)]   # (Newell's: the ring's plane)
            size = math.sqrt(sum(c * c for c in normal_)) or 1.0
            normal_ = [c / size for c in normal_]
            first_axis = (1.0, 0.0, 0.0) if abs(normal_[0]) < 0.9 else (0.0, 1.0, 0.0)
            e1 = [first_axis[a] - normal_[a] * sum(first_axis[k] * normal_[k] for k in range(3)) for a in range(3)]
            size = math.sqrt(sum(c * c for c in e1))
            e1 = [c / size for c in e1]
            e2 = [normal_[(a + 1) % 3] * e1[(a + 2) % 3] - normal_[(a + 2) % 3] * e1[(a + 1) % 3] for a in range(3)]
            uvs.append([(sum(p[a] * e1[a] for a in range(3)) / tile, sum(p[a] * e2[a] for a in range(3)) / tile) for p in points])
    if not ringwise:   # (panels: a skin that narrows and swells round two ways has no picture laid round and along it unsheared;
        # world/check_uv.py found the chair's weave sheared 40 times where its thin end ring met a thick one)
        uvs = [panel_uv([vertices[i] for i in face], tile) for face in faces]
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
             at_z(rrect(OUT_W - 0.03, OUT_H - 0.03, 0.01, 0, OUT_Y), front + block)], skin("PlasticBeige"), first=False, last=MODE != "low")   # (open at the tube's opening: the slope down to the glass goes on from it; the shell has no use for a face nobody sees)
    if MODE == "low":   # the shell is one closed skin: the seam has a floor, which the bake finds dark
        loft(b, [at_z(rrect(OUT_W - 0.03, OUT_H - 0.03, 0.01, 0, OUT_Y), front + block), at_z(rrect(OUT_W - 0.03, OUT_H - 0.03, 0.01, 0, OUT_Y), z0)], skin("PlasticBeige"))
    else:   # what shows in the seam
        loft(b, [at_z(rrect(OUT_W - 0.02, OUT_H - 0.02, 0.01, 0, OUT_Y), front + block - 0.004),
                 at_z(rrect(OUT_W - 0.02, OUT_H - 0.02, 0.01, 0, OUT_Y), z0 + 0.004)], "PlasticDark")
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
    opening = [at_z(rrect(w + 0.024, h + 0.024, 0.03, 0, mid), front), at_z(rrect(w + 0.008, h + 0.008, 0.025, 0, mid), front + 0.014),
               on_glass(rrect(w, h, 0.022, 0, mid))]
    # (the shell ends at the glass, a sheet of the place's own that covers the opening: a plate
    # closed it 5 cm behind, a second screen to whoever came close, and 7 parts in a hundred of the baked picture)
    loft(b, opening, skin("PlasticGrey"), first=False, last=False, normal=(0, 0, -1))   # (from the bezel's ring to the glass: it joins both, it closes nothing)
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
        # the remote's window, and six keys (NEC's RGB 2/+ to PROCEED; named here for what they do, textures.py); and
        # behind grilles in the chin's corners, its two oval loudspeakers of 9 x 5.5 cm
        round_button(b, 0.18, CHIN, front, 0.011, "PlasticBeige")
        b.cyl((LED_X, CHIN, front - 0.0005), 0.0035, 0.003, "Led", segs=10, rot=(90, 0, 0))
        for i in range(6):
            b.box((OSD_X + i * OSD_PITCH, CHIN, front - 0.0012), (0.016, 0.008, 0.0036), "PlasticGrey", bevel=0.0014, segs=2)
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
    loft(b, [d_ring(x, y, z + 0.0008, top, 0.00836), d_ring(x, y, z + 0.0066, top, 0.00836)], "MetalSteel", first=False, last=False)   # (a sleeve: its mouth open on the insert)
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


NIC_LAMP = (0.005, 0.003)   # the network card's link lamp: a dull lens, which Unity lights
LAN_PLUG = 0.024            # a network lead's plug, from the panel to where the lead leaves it


def lan_socket(b, x, y, z):
    """A twisted pair socket (8P8C) in a face that looks towards +z: a dark opening, its latch's notch below."""
    b.box((x, y, z + 0.0006), (0.0158, 0.0132, 0.0012), "MetalSteel", bevel=0.0004, segs=1)
    # (the opening 0.6 mm before the frame, and its sides clear of a plug's: 0.2 and 0.3 mm flickered)
    b.box((x, y + 0.0006, z + 0.0015), (0.0124, 0.0086, 0.0006), "Rubber")
    b.box((x, y - 0.0049, z + 0.0015), (0.006, 0.0022, 0.0006), "Rubber")


def lan_plug(b, x, y, z, mat):
    """A network lead's plug in such a socket: the clear plug, and a boot of the lead's colour."""
    b.box((x, y + 0.0006, z + 0.004), (0.0114, 0.0078, 0.008), "Ceramic", bevel=0.0008, segs=1)
    b.box((x, y + 0.0006, z + 0.015), (0.0128, 0.0098, 0.018), mat, bevel=0.003, segs=2)


def jack(b, x, y, z):
    """A 3.5 mm socket and its nut."""
    b.cyl((x, y, z + 0.001), 0.004, 0.002, "MetalSteel", segs=6, rot=(90, 0, 0))
    b.cyl((x, y, z + 0.0022), 0.00175, 0.0004, "Rubber", segs=10, rot=(90, 0, 0))


JACK_LONG, RCA_LONG, DC_LONG = 0.028, 0.042, 0.036   # such plugs, from the panel to where the lead leaves them


def jack_plug(b, x, y, z):
    """A 3.5 mm plug in such a socket (z its face): the sleeve's root at the nut, a ribbed grip 6.4 mm across, a strain relief."""
    b.cyl((x, y, z + 0.0008), 0.0024, 0.0052, "MetalSteel", segs=12, rot=(90, 0, 0))   # (from the panel: the baked case has no nut)
    b.lathe((x, y, z), [(0.0024, 0.0034), (0.0032, 0.004), (0.0032, 0.009), (0.003, 0.0094), (0.0032, 0.0098), (0.0032, 0.014), (0.0029, 0.0175),
                        (0.0024, 0.019)], "PlasticDark", segs=14, rot=(90, 0, 0))
    b.lathe((x, y, z), [(0.0024, 0.0185), (0.0022, 0.022), (0.00215, 0.0255), (0.0021, JACK_LONG)], "Rubber", segs=10, rot=(90, 0, 0))


def rca_plug(b, x, y, z, mat):
    """A phono plug on a phono socket of a face that looks towards +z: its steel shell over the
    socket's sleeve to the panel, a ribbed grip of its channel's colour, 10.4 mm across, a strain relief."""
    b.lathe((x, y, z), [(0.0043, -0.0014), (0.0049, -0.0014), (0.0049, 0.0102)], "MetalSteel", segs=16, rot=(90, 0, 0), cap=False)
    b.lathe((x, y, z), [(0.0044, 0.0098), (0.0052, 0.0104), (0.0052, 0.0135), (0.0049, 0.014), (0.0052, 0.0145), (0.0052, 0.0175), (0.0049, 0.018),
                        (0.0052, 0.0185), (0.0052, 0.0255), (0.0047, 0.029), (0.0036, 0.0305)], mat, segs=16, rot=(90, 0, 0))
    b.lathe((x, y, z), [(0.0036, 0.03), (0.0032, 0.0335), (0.0029, 0.0345), (0.0029, 0.0365), (0.0026, 0.0375), (0.0024, RCA_LONG)], "Rubber", segs=12, rot=(90, 0, 0))


def dc_inlet(b, x, y, z):
    """A low voltage supply's barrel socket in a face that looks towards +z: a collar, the opening, its pin."""
    b.lathe((x, y, z), [(0.0052, 0), (0.0052, 0.0012), (0.0046, 0.0018), (0.0031, 0.0018)], "PlasticDark", segs=14, rot=(90, 0, 0), cap=False)
    b.cyl((x, y, z + 0.0008), 0.0031, 0.0016, "Rubber", segs=12, rot=(90, 0, 0))
    b.cyl((x, y, z + 0.0018), 0.001, 0.001, "Brass", segs=6, rot=(90, 0, 0))


def dc_plug(b, x, y, z):
    """The supply's barrel plug in such a socket: the steel barrel's root, a ribbed grip 9.6 mm across, a strain relief."""
    b.cyl((x, y, z + 0.0027), 0.00275, 0.0022, "MetalSteel", segs=12, rot=(90, 0, 0))
    b.lathe((x, y, z), [(0.003, 0.0036), (0.0046, 0.0044), (0.0048, 0.006), (0.0048, 0.0095), (0.0045, 0.01), (0.0048, 0.0105), (0.0048, 0.014),
                        (0.0045, 0.0145), (0.0048, 0.015), (0.0048, 0.021), (0.0042, 0.0245), (0.0033, 0.026)], "PlasticDark", segs=14, rot=(90, 0, 0))
    b.lathe((x, y, z), [(0.0033, 0.0255), (0.003, 0.029), (0.0026, 0.033), (0.0023, DC_LONG)], "Rubber", segs=10, rot=(90, 0, 0))


Y_TOP, Y_LOW = 0.0125, 0.0195   # a lead's moulded Y: where its two tails go in above its middle, and the one lead leaves below


def y_split(b, x, y, z):
    """Where a lead parts in two: a moulded body standing upright, a sleeve for each tail on top, a strain relief below."""
    b.box((x, y, z), (0.0125, 0.019, 0.0078), "Rubber", bevel=0.0032, segs=3)
    for side in (-1, 1):
        b.cyl((x + side * 0.0028, y + 0.0105, z), 0.0021, 0.004, "Rubber", segs=10, bevel=0.0005)
    b.lathe((x, y - 0.0094, z), [(0.0034, 0), (0.0031, 0.004), (0.0026, 0.008), (0.0024, Y_LOW - 0.0094)], "Rubber", segs=10, rot=(180, 0, 0))


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


# The speed's window on the tower's front: two rows of three digits, which Unity lights
# (SevenSeg.shader: millions of instructions a second, every core over core 0). Its middle from
# the tower's own, and its size: ShaderEmuStations.cs reads them from Models/stations.json.
SPEED_X, SPEED_DOWN, SPEED_W, SPEED_H = 0.052, 0.176, 0.044, 0.026


def speed_display(b, x, y):
    """The window alone, dark, with a legend a row: the digits are not the model's."""
    b.box((x, y, FRONT - 0.0008), (SPEED_W + 0.004, SPEED_H + 0.004, 0.0016), "PlasticDark", bevel=0.0006, segs=1)
    legend(b, "mips_all", x, y + SPEED_H / 2 + 0.0055, FRONT - 0.0011, back=False)
    legend(b, "mips_cpu", x, y - SPEED_H / 2 - 0.0055, FRONT - 0.0011, back=False)


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
            "video": (-0.02, first - SLOT_PITCH), "inlet": (-0.052, py + 0.022),
            "lan": (0.020, first - 5 * SLOT_PITCH), "nic": (0.040, first - 5 * SLOT_PITCH)}   # the network card's socket, and its link lamp


def rear(b, w, h, back):
    """The tower's back, which a visitor behind the table sees: bare steel; the power supply's
    own plate (PS/2 form, 150 x 86 mm) with its fan, the mains inlet, the monitor's outlet and
    the voltage switch; the board's sockets under their legends; seven slots' brackets, a video
    card's socket in the second, a sound card's in the fourth, a network card's in the sixth; screws."""
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
        elif i == 5:    # the network card: thin coax, twisted pair, and a lamp for its link
            bnc(b, -0.026, y, z + 0.0012)
            lan_socket(b, at["lan"][0], y, z + 0.0012)
            b.box((at["nic"][0], y, z + 0.0018), NIC_LAMP + (0.0012,), "LampGreen")
            legend(b, "lan", -0.051, y, z + 0.0016)
        else:
            b.box((-0.005, y, z + 0.0015), (0.098, 0.0036, 0.0004), "PlasticDark")   # a blank's slot for air
    for x in (-0.084, 0.084):   # the cover's screws down each side
        for y in (0.03, h * 0.4, h - 0.16):
            b.cyl((x, y, z + 0.0006), 0.0038, 0.0022, "MetalSteel", segs=8, rot=(90, 0, 0))


# the tower's keys a visitor presses, from its middle line (and the power key's depth below its top)
RESET_X, POWER_KEY = 0.020, (0.045, 0.226)
LAMPS_X = (-0.041, -0.027, -0.013)   # the tower's lamps: power, turbo, disk
# the monitor's six keys under the glass, left to right as their legend names them
OSD_X, OSD_PITCH = 0.02, 0.022


def tower(b):
    """The computer itself, standing on the floor: its foot at the frame's origin, its front at FRONT."""
    w, h, deep = 0.18, TOWER_HIGH, TOWER_DEEP
    back = FRONT + deep
    # the steel cover, the plastic front with rounded edges, the feet
    # (the cover is open where the front closes it, and the front's back is only its rim: a face
    # of each was hidden inside the other, 12 cm2 in a hundred of the baked picture, and seen from within)
    cover = rrect(w, h - 0.012, 0.0025, 0, h / 2 + 0.006)
    loft(b, [at_z(cover, FRONT + 0.016), at_z(cover, back - 0.0025), at_z(rrect(w - 0.005, h - 0.017, 0.0025, 0, h / 2 + 0.006), back)], skin("PlasticCase"), first=False, last=True)   # a folded steel cover (open where the front's back meets it)
    loft(b, [at_z(rrect(w + 0.004, h, 0.008, 0, h / 2 + 0.006), FRONT + 0.016), at_z(rrect(w + 0.004, h, 0.01, 0, h / 2 + 0.006), FRONT + 0.004),
             at_z(rrect(w - 0.002, h - 0.006, 0.01, 0, h / 2 + 0.006), FRONT)], skin("PlasticBeige"), first=False, last=True, normal=(0, 0, -1))
    # the front's back, where it stands out round the cover
    loft(b, [at_z(rrect(w + 0.004, h, 0.008, 0, h / 2 + 0.006), FRONT + 0.016), at_z(cover, FRONT + 0.016)], skin("PlasticBeige"), first=False, last=False, normal=(0, 0, 1))   # (a rim between the front and the cover: it joins both)
    for x in (-0.07, 0.07):
        for z in (FRONT + 0.05, back - 0.05):
            b.cyl((x, 0.003, z), 0.014, 0.006, skin("Rubber"), segs=14)
    if MODE == "low":
        return
    rear(b, w, h, back)
    for i in range(12):   # the slots in its side
        b.box((-w / 2 - 0.0003, 0.12, FRONT + 0.16 + i * 0.014), (0.001, 0.12, 0.006), "PlasticDark")
    # from the top: the CD-ROM drive and a blank bay of its size on the middle line; under them the
    # front is plain where Unity lays the speed slider (ShaderEmuStations.cs, TowerSpeedBay)
    cd_drive(b, 0, h - 0.04)
    drive_gap(b, 0, h - 0.088, BAY_525[0] + 0.002, BAY_525[1] + 0.001)
    b.box((0, h - 0.088, FRONT - 0.0006), (BAY_525[0], BAY_525[1] - 0.001, 0.0016), "PlasticBeige", bevel=0.0008, segs=1)
    # the panel: the lock, three lamps over their names, reset over its name, the speed
    cy = h - SPEED_DOWN
    b.box((0, cy, FRONT - 0.0004), (0.156, 0.046, 0.001), "PlasticCase", bevel=0.0004, segs=1)
    b.cyl((-0.06, cy, FRONT - 0.002), 0.008, 0.004, "MetalSteel", segs=20, bevel=0.001, rot=(90, 0, 0))
    b.box((-0.06, cy, FRONT - 0.0042), (0.002, 0.008, 0.0006), "PlasticDark")
    for x, mat, name in ((LAMPS_X[0], "Led", "power"), (LAMPS_X[1], "Amber", "turbo"), (LAMPS_X[2], "LampRed", "hdd")):   # (the disk's: a dull red lens)
        b.box((x, cy + 0.005, FRONT - 0.0016), (0.007, 0.003, 0.002), mat, bevel=0.0006, segs=1)
        legend(b, name, x, cy - 0.005, FRONT - 0.0011, back=False)
    b.box((RESET_X, cy + 0.004, FRONT - 0.003), (0.013, 0.008, 0.004), "PlasticBeige", bevel=0.0016, segs=2)
    legend(b, "reset", RESET_X, cy - 0.007, FRONT - 0.0011, back=False)
    speed_display(b, SPEED_X, cy)
    # under it the power key in its well, at the right, over its name; the badge at the left
    px, py = POWER_KEY[0], h - POWER_KEY[1]
    b.box((px, py, FRONT - 0.0004), (0.04, 0.03, 0.001), "PlasticCase", bevel=0.0004, segs=1)
    b.box((px, py, FRONT - 0.004), (0.034, 0.024, 0.006), "PlasticBeige", bevel=0.003, segs=3)
    b.box((px, py, FRONT - 0.0072), (0.016, 0.0016, 0.0006), "PlasticGrey")
    legend(b, "power", px, py - 0.019, FRONT - 0.0003, back=False)
    b.box((-0.045, py, FRONT - 0.0012), (0.036, 0.014, 0.0016), "MetalSteel", bevel=0.0006, segs=1)
    b.quad((-0.045, py, FRONT - 0.0022), (0.032, 0.0128), "Details", decal="pc_badge")
    for row in range(2):   # the grille: two rows of slots, centred
        for i in range(16):
            b.box((-0.06375 + i * 0.0085, 0.04 + row * 0.038, FRONT - 0.0002), (0.0045, 0.028, 0.0012), "PlasticDark", bevel=0.0004, segs=1)


KEY_SCALE = 1.3          # the keyboard against a real one (keys 19.05 mm apart): 25 mm, for a beam from a chair
KEYBOARD_Z = -0.31       # its middle, from the place's
KEY_UNIT = KEY_SCALE * 0.01905 / 60       # the layout's keys are 60 apart: 19.05 mm, the standard pitch
KEYS_HIGH = 437 * KEY_UNIT                # the layout's height (world/keyboard.json)
TRAY_BACK = 0.016        # the tray's rim behind the keys, more than at their other sides: the lamps and their names are on it
TRAY_LOW, KEYBOARD_SLOPE = 0.008, 0.0     # the tray's underside below its own frame; it lies flat on the table
TRAY_LID = -0.0084        # where the tray's loft is closed, 2 mm under the well (in its plane it flickered)
TRAY_AT, TRAY_TURN = (0, TRAY_LOW, KEYBOARD_Z), (90 - KEYBOARD_SLOPE, 0, 0)   # the tray's own frame in the keyboard's: -z of it is up from the keys
CAPS_AT, CAP_HIGH = 0.012, 0.0078         # the caps' feet over the tray's frame, and a cap's height
KEY_TOPS = CAPS_AT + CAP_HIGH + 0.0012    # where Unity lays the keys' rectangles: a hair over the caps' tops
LEAD_X = 0.12            # where the lead leaves the tray's back, right of its middle


def keyboard(b):
    """The keyboard: world/keyboard.json's layout at KEY_SCALE times a real keyboard's size, caps
    of the console keyboard's shape (pc_faces.CAP_...), all as high, in a tray that lies flat."""
    layout = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "keyboard.json"), encoding="utf-8"))
    wide, tall, s = layout["width"], layout["height"], KEY_UNIT
    vertices, faces, uvs, normals, caps = [], [], [], [], []

    def ring(x0, y0, x1, y1, z):
        first = len(vertices)
        for px, py in ((x0, y1), (x1, y1), (x1, y0), (x0, y0)):
            vertices.append(((px - wide / 2) * s, (tall / 2 - py) * s, z))
        return first

    def cap_chart(place, normal):
        """A cap is a chart by itself, and the chart is its top alone: its sides are drawn in to the
        top's edge, and take the top's light. A lightmap's texels are blurred into each other:
        sides with a place of their own darkened the top, and the well darkened both. Its foot,
        which faces the well's floor, is a part by itself, small: drawn in like the sides it lay
        over the whole top, and the bake gave the top the foot's dark (every station's keys black)."""
        if normal.z > 0.7:
            return ("feet", place.x * 0.05, place.y * 0.05)
        px, py = place.x / s + wide / 2, tall / 2 - place.y / s
        which = min(range(len(caps)), key=lambda k: max(caps[k][0] - px, px - caps[k][0] - caps[k][2], caps[k][1] - py, py - caps[k][1] - caps[k][3]))
        x0, y0, w, h, high = caps[which]
        return (which, (min(max(px, x0 + top_in[0]), x0 + w - top_in[0]) - wide / 2) * s, (tall / 2 - min(max(py, y0 + top_in[1]), y0 + h - top_in[2])) * s)

    def face(corners, normal, uv=None):
        faces.append(tuple(corners))
        uvs.append(uv or [pc_faces.canvas_uv(vertices[i][0] / s + wide / 2, tall / 2 - vertices[i][1] / s, wide, tall) for i in corners])
        normals.append(normal)
    shoulder_in, top_in = pc_faces.CAP_SHOULDER, pc_faces.CAP_TOP
    # The well's floor is under no key: it is the well less every key's own square, and a key
    # rises out of the edge of its square. So the keys and the floor are one surface with no
    # part of it over another, and one chart of the lightmap seen from above (no seam a key).
    floor_z = 0.0016                                  # under the caps' feet, in the caps' frame
    cells = [(x0, y0, x0 + w, y0 + h) for x0, y0, w, h, legend in layout["keys"]]
    edge = 0.004 / s                                  # the well is so much wider than the layout
    xs = sorted({-edge, wide + edge} | {c[0] for c in cells} | {c[2] for c in cells})
    ys = sorted({-edge, tall + edge} | {c[1] for c in cells} | {c[3] for c in cells})
    floor_v, floor_f, floor_uv, floor_n = [], [], [], []

    def floor_face(points, normal, flat=None):
        first = len(floor_v)
        floor_v.extend(((px - wide / 2) * s, (tall / 2 - py) * s, z) for px, py, z in points)
        floor_f.append(tuple(range(first, first + len(points))))
        floor_uv.append([pc_faces.canvas_uv(px, py, wide, tall) for px, py, _ in (flat or points)])   # the caps' picture: dark between them
        floor_n.append(normal)
    for i in range(len(xs) - 1):
        for j in range(len(ys) - 1):
            mx, my = (xs[i] + xs[i + 1]) / 2, (ys[j] + ys[j + 1]) / 2
            if not any(c[0] < mx < c[2] and c[1] < my < c[3] for c in cells):
                floor_face([(xs[i], ys[j + 1], floor_z), (xs[i + 1], ys[j + 1], floor_z), (xs[i + 1], ys[j], floor_z), (xs[i], ys[j], floor_z)], (0, 0, -1))
    for x0, y0, x1, y1 in cells:
        outer = [(x0, y1), (x1, y1), (x1, y0), (x0, y0)]
        # (a hair under the cap's own foot, not on it: Bakery blends the light of two faces that meet at an edge)
        inner = [(x0 + 1.8, y1 - 1.8), (x1 - 1.8, y1 - 1.8), (x1 - 1.8, y0 + 1.8), (x0 + 1.8, y0 + 1.8)]
        for k in range(4):
            a, b2 = k, (k + 1) % 4
            ramp = [(outer[a][0], outer[a][1], floor_z), (outer[b2][0], outer[b2][1], floor_z), (inner[b2][0], inner[b2][1], 0.0), (inner[a][0], inner[a][1], 0.0)]
            # its picture unrolled from its outer edge, as long up it as the ramp is (from above it was 1.8 across for 4.3 up)
            across = 1 if k in (0, 2) else 0
            slant = math.hypot(1.8, floor_z / s) / 1.8
            flat = ramp[:2] + [tuple(c if axis != across else ramp[1 if n == 2 else 0][axis] + (c - ramp[1 if n == 2 else 0][axis]) * slant
                                     for axis, c in enumerate(ramp[n][:2])) + (0.0,) for n in (2, 3)]
            floor_face(ramp, (0, 0, -1), flat)
    for x0, y0, w, h, legend in layout["keys"]:   # (the capture key too, where the console keyboard has it)
        rects = [(x0 + 1.5, y0 + 1.5, x0 + w - 1.5, y0 + h - 1.5),
                 (x0 + shoulder_in[0], y0 + shoulder_in[1], x0 + w - shoulder_in[0], y0 + h - shoulder_in[2]),
                 (x0 + top_in[0], y0 + top_in[1], x0 + w - top_in[0], y0 + h - top_in[2])]
        rises = (0.0, CAP_HIGH * (1 - pc_faces.CAP_STEP) / s, CAP_HIGH / s)
        base, shoulder, top = (ring(*r, -rise * s) for r, rise in zip(rects, rises))
        corners = [((r[0], r[3]), (r[2], r[3]), (r[2], r[1]), (r[0], r[1])) for r in rects]
        caps.append((x0, y0, w, h, -CAP_HIGH))
        face((top, top + 1, top + 2, top + 3), (0, 0, -1))
        face((base + 3, base + 2, base + 1, base), (0, 0, 1))   # its foot, closing it (world/check_closed.py)
        for k, normal in enumerate(((0, -1, 0), (1, 0, 0), (0, 1, 0), (-1, 0, 0))):   # its sides, unrolled into their band of the picture
            sides = pc_faces.cap_side_uvs(corners, k, rises, pc_faces.cap_named(legend), wide, tall)
            for (lower, upper), uv in zip(((base, shoulder), (shoulder, top)), sides):
                face((lower + k, lower + (k + 1) % 4, upper + (k + 1) % 4, upper + k), normal, uv)
    kw, kh = wide * s, tall * s
    back = TRAY_BACK / 2   # the outline's middle is so much behind the keys': the rim is wider there

    def from_above(place, normal):
        """The keyboard's chart: as seen from over the keys, and the tray's upright side folded
        out round it. What it stands on is a part by itself, small: nobody sees it."""
        if normal.z > 0.7 and place.z > TRAY_LOW - 0.001:
            return ("under", place.x * 0.05, place.y * 0.05)
        if abs(normal.z) > 0.7 and abs(place.z - TRAY_LID) < 1e-4:   # (the tray's lid under the well: seen from above it lay on the well's floor, and the bake gave the floor its dark)
            return ("lid", place.x * 0.05, place.y * 0.05)
        down = max(0.0, place.z + 0.006)
        if down > 0:
            half_w, half_h = (kw + 0.03) / 2 - 0.011, (kh + 0.03 + TRAY_BACK) / 2 - 0.011
            dx = place.x - min(max(place.x, -half_w), half_w)
            dy = (place.y - back) - min(max(place.y - back, -half_h), half_h)
            d = math.hypot(dx, dy)
            if d > 1e-6:
                return (place.x + dx / d * down, place.y + dy / d * down)
        return (place.x, place.y)
    with b.at(TRAY_AT, TRAY_TURN):
        with b.chart("caps", cap_chart):
            with b.at((0, 0, -CAPS_AT)):
                b.mesh(vertices, faces, uvs, "Keyboard", normal=normals)
        with b.chart("keyboard", from_above):
            with b.at((0, 0, -CAPS_AT)):
                b.mesh(floor_v, floor_f, floor_uv, "Keyboard", normal=floor_n, sheet=True)   # the well, and each key's foot out of it: a sheet over the tray
            loft(b, [at_z(rrect(kw + 0.03, kh + 0.03 + TRAY_BACK, 0.01, cy=back), TRAY_LOW), at_z(rrect(kw + 0.03, kh + 0.03 + TRAY_BACK, 0.012, cy=back), -0.006),
                     at_z(rrect(kw + 0.022, kh + 0.022 + TRAY_BACK, 0.012, cy=back), -0.0105), at_z(rrect(kw + 0.008, kh + 0.008, 0.006), -0.0104),
                     at_z(rrect(kw + 0.004, kh + 0.004, 0.004), TRAY_LID)], "PlasticBeige",   # (closed 2 mm under the well, not in its plane: it flickered)
                 first=True)
        # the three lamps over the number pad and their names under them, all on the rim's flat top
        rim = kh / 2 + 0.004   # where the well ends: the flat is from here to rim + 0.007 + TRAY_BACK
        for i, mat in enumerate(("LampGreen", "PlasticDark", "PlasticDark")):   # (unlit: a lamp that glowed on a machine that was off)
            b.box((kw / 2 - 0.02 - i * 0.012, rim + 0.016, -0.0108), (0.006, 0.002, 0.0008), mat)
        b.quad((kw / 2 - 0.032, rim + 0.0085, -0.0107), (0.04, 0.005), "Details", decal="kb_lamps")
        # where the lead leaves, behind: a sleeve moulded on the tray's back
        b.box((LEAD_X, kh / 2 + 0.015 + TRAY_BACK + 0.004, 0.001), (0.016, 0.01, 0.01), "PlasticCase", bevel=0.002, segs=1)


def mouse(b):
    """A mouse with two buttons on a mat."""
    b.box((0, 0.0015, 0), (0.21, 0.003, 0.25), "Rubber", bevel=0.001, segs=1)   # the mat, lying flat
    mouse_body(b)


MOUSE_TURN = 192      # its buttons away from the sitter, turned a little as a hand leaves it
MOUSE_LONG = 0.108
MOUSE_LEAD = (0.0, 0.0125, -MOUSE_LONG / 2 - 0.011)   # the end of the sleeve its lead leaves by, in the body's own frame


def mouse_body(b):
    """The mouse itself, standing on y = 0.004: sections of a pebble with a flat base and a low
    wall round it, humped under the palm, its two buttons away from the sitter (+z)."""
    def section(t, lift=0.0, around=18):
        z = MOUSE_LONG / 2 - t * MOUSE_LONG               # from its tail (under the wrist) to the buttons' edge
        ends = (1 - (2 * t - 1) ** 6) ** 0.5              # rounded over at both ends, not cut off
        half = (0.0300 * (1 - (2 * t - 1) ** 4) ** 0.5 + 0.0015) * (0.35 + 0.65 * ends) + lift
        # highest under the palm, a third of the way from the tail; lower and flatter at the buttons
        hump = 0.0105 + 0.0215 * math.sin(math.pi * (0.10 + 0.74 * (1 - t))) ** 1.25
        high = hump * (0.3 + 0.7 * ends) + lift
        wall = 0.0055 * (0.4 + 0.6 * ends)                # the wall round the base, which the hump rises from
        ring = [(half * 0.955, 0.0040, z), (half * 0.985, 0.0046, z), (half, 0.0040 + wall * 0.6, z)]
        for a in range(1, around):                        # over the top, right to left: squarer than a circle
            angle = math.pi * a / around
            x, y = math.cos(angle), math.sin(angle)
            ring.append((half * (1 if x > 0 else -1) * abs(x) ** 0.82, 0.0040 + wall + (high - wall) * y ** 0.88, z))
        ring += [(-half, 0.0040 + wall * 0.6, z), (-half * 0.985, 0.0046, z), (-half * 0.955, 0.0040, z)]
        return ring
    def unrolled(place, normal):
        """The mouse's chart: where along it a point is, and how far round its section from the
        top (so its sides are not on edge, as from above they would be). Its flat underside
        and its two small ends are parts by themselves."""
        if normal.y < -0.7 and place.y < 0.0047:
            return ("under", place.x * 0.05, place.z * 0.05)
        if abs(normal.z) > 0.95:
            return ("end", place.x, place.y)
        ring = section(min(1.0, max(0.0, (MOUSE_LONG / 2 - place.z) / MOUSE_LONG)))
        top = len(ring) // 2
        near = min(range(len(ring)), key=lambda k: (ring[k][0] - place.x) ** 2 + (ring[k][1] - place.y) ** 2)
        low, high = min(near, top), max(near, top)
        round_it = sum(math.hypot(ring[k + 1][0] - ring[k][0], ring[k + 1][1] - ring[k][1]) for k in range(low, high))
        return (place.z, round_it if near > top else -round_it)
    with b.at((0.0, 0, 0.0), (0, MOUSE_TURN, 0)):
        steps = (0.0, 0.012, 0.035, 0.07, 0.12, 0.19, 0.27, 0.36, 0.45, 0.54, 0.63, 0.72, 0.80, 0.87, 0.93, 0.965, 0.988, 1.0)
        with b.chart("mouse", unrolled):
            loft(b, [section(t) for t in steps], "PlasticBeige", first=True, last=True, tile=0.2)
        # the seams are ribbons a hair over the skin: across behind the buttons, and between them
        near, far = section(0.535, 0.0003), section(0.548, 0.0003)
        n = len(near)
        top = n // 2   # the ring's point over its middle
        ridge = [section(t, 0.0003)[top] for t in (0.548, 0.63, 0.72, 0.80, 0.87, 0.93, 0.965, 0.988)]
        left = [(-0.0006, y, z) for x, y, z in ridge]
        right = [(0.0006, y, z) for x, y, z in ridge]
        with b.chart("mouse seams", unrolled):
            faces = [(k, k + 1, n + k + 1, n + k) for k in range(3, n - 4)]   # (each face's picture its own panel's: one square on each had stretched it 8 times)
            b.mesh(near + far, faces, [panel_uv([(near + far)[i] for i in f], 0.2) for f in faces], "PlasticDark", normal=(0, 1, 0), sheet=True)
            n = len(ridge)
            faces = [(k, k + 1, n + k + 1, n + k) for k in range(n - 1)]
            b.mesh(left + right, faces, [panel_uv([(left + right)[i] for i in f], 0.2) for f in faces], "PlasticDark", normal=(0, 1, 0), sheet=True)
        # the sleeve the lead leaves by, under the buttons' edge
        b.cyl((MOUSE_LEAD[0], MOUSE_LEAD[1], MOUSE_LEAD[2] + 0.007), 0.0036, 0.014, "PlasticCase", segs=10, bevel=0.001, rot=(90, 0, 0))


# The office chair, after Steelcase's Criterion task chair (1990s; its brochure and spec sheet): a seat
# 495 mm wide and 413 mm deep, set 406 to 533 mm high (here its middle at CHAIR_SEAT), arms 190 to
# 292 mm over it, 699 x 648 x 1130 mm over all; on a five-star base of 25 in (635 mm) and 50 mm
# twin-wheel casters, as task chairs of the decade had. Its seat and back are upholstered in a wool
# herringbone (Poly Haven's poly_wool_herringbone, 0.27 m a repeat: FabricChair), the rest nylon.
CHAIR_SEAT, CHAIR_WEAVE = 0.50, 0.27   # the seat's top at its middle; the weave's repeat


def chair_section(w, t, dish, centre, plane, n=28):
    """A cushion's cross section: a squared-off ellipse w wide and t thick, its face dished by
    `dish` in the middle and rising at the sides. plane(x, up) puts a point in the frame."""
    points = []
    for k in range(n):
        a = 2 * math.pi * k / n
        c, s = math.cos(a), math.sin(a)
        x = w / 2 * math.copysign(abs(c) ** 0.45, c)
        up = t / 2 * math.copysign(abs(s) ** 0.45, s)
        if up > 0:   # the face: dished in the middle, a bolster at each side
            u = 2 * x / w
            up += (-dish * (1 - u * u) + 0.4 * dish * u ** 6) * (up / (t / 2))
        points.append(plane(x, centre + up))
    return points


def carried(path, outline, side, steps=4):
    """Rings of `outline` (across, up) carried along a path in a plane: `side` is across it (the
    plane's normal). The path is first smoothed through its points (Catmull-Rom)."""
    points = [Vector(p) for p in path]
    smooth = []
    for i in range(len(points) - 1):
        p0, p1, p2, p3 = points[max(i - 1, 0)], points[i], points[i + 1], points[min(i + 2, len(points) - 1)]
        for k in range(steps if i < len(points) - 2 else steps + 1):
            t = k / steps
            smooth.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t))
    side = Vector(side).normalized()
    rings = []
    for i, p in enumerate(smooth):
        tangent = (smooth[min(i + 1, len(smooth) - 1)] - smooth[max(i - 1, 0)]).normalized()
        up = tangent.cross(side).normalized()
        rings.append([tuple(p + side * a + up * b) for a, b in outline])
    return rings


def office_chair(b, kind="seat"):
    """An office chair of the decade at the frame's origin, its seat's front towards +z (see
    CHAIR_SEAT): a five-star base on twin-wheel casters, a gas lift in a telescoping shroud, the
    tilt mechanism, a contoured seat with a waterfall front, a back curved round the sitter with a
    lumbar swell on a bent spine, and T-arms."""
    b.pose(kind, (0.70, 1.13, 0.65), at=(0, 0.565, 0), name="Office chair")   # (a seat faces its desk: world/check_placement.py)
    import toys
    radius = 0.3175   # the base's
    for i in range(5):
        with b.at((0, 0, 0), (0, i * 72 + 18, 0)):
            # a spoke: tapering and falling from the hub to its tip, crowned on top, the socket under its tip
            rings = []
            for k in range(9):
                d = 0.045 + (radius - 0.045 - 0.012) * k / 8
                f = k / 8
                w, h = 0.058 - 0.018 * f, 0.044 - 0.014 * f
                y = 0.112 - 0.03 * f
                rings.append([(x, y + yy, d) for x, yy in rrect(w, h, 0.4 * h, n=4)])
            rings.append([(x * 0.7, y + yy * 0.75, radius - 0.004) for x, yy in rrect(w, h, 0.4 * h, n=4)])
            loft(b, rings, "PlasticDark", last=True, tile=0.3)
            tip = radius - 0.016
            b.lathe((0, 0, tip), [(0, 0.056), (0.019, 0.056), (0.021, 0.06), (0.021, 0.075), (0, 0.075)], "PlasticDark", segs=16)
            b.lathe((0, 0, tip), [(0.0055, 0.046), (0.0055, 0.057)], "MetalSteel", segs=8, cap=False)   # the caster's stem
            with b.at((0, 0, tip), (0, 25 + i * 31, 0)):   # each caster trails its own way
                # the body between the wheels, its hood over them, and the two wheels on their axle
                toys.soft(b, toys.rounded(0.016, 0.048, 0.007), 0.034, 0.006, "PlasticDark", at=(0, 0.012, 0.012))
                toys.soft(b, toys.rounded(0.05, 0.03, 0.01), 0.008, 0.004, "PlasticDark", at=(0, 0.046, 0.006))
                for side in (-1, 1):
                    tyre = [(0, -0.0062), (0.016, -0.0062), (0.022, -0.0058), (0.0248, -0.0035), (0.025, 0), (0.0248, 0.0035), (0.022, 0.0058), (0.016, 0.0062), (0, 0.0062)]
                    b.lathe((side * 0.0145, 0.025, 0.02), tyre, "Rubber", segs=20, rot=(0, 0, 90))
                    b.lathe((side * 0.0145, 0.025, 0.02), [(0.009, side * 0.0064), (0, side * 0.0066)], "PlasticGrey", segs=12, rot=(0, 0, 90), cap=False)
    # the hub, the shroud over the lift (three sleeves), the bright piston, the mechanism and its lever
    b.lathe((0, 0.07, 0), [(0, 0), (0.05, 0), (0.06, 0.014), (0.06, 0.05), (0.046, 0.064), (0, 0.064)], "PlasticDark", segs=28)
    b.lathe((0, 0, 0), [(0, 0.13), (0.037, 0.13), (0.037, 0.215), (0.033, 0.222), (0.033, 0.285), (0.029, 0.292), (0.029, 0.335), (0, 0.335)],
            "PlasticDark", segs=24)
    b.lathe((0, 0, 0), [(0, 0.334), (0.0135, 0.334), (0.0135, 0.395), (0, 0.395)], "MetalSteel", segs=16)
    toys.soft(b, toys.rounded(0.2, 0.25, 0.035), 0.036, 0.01, "MetalBlack", at=(0, 0.388, 0.0))
    b.tube([(0.09, 0.405, 0.06), (0.2, 0.4, 0.07), (0.26, 0.395, 0.075)], 0.0055, "MetalBlack", segs=8, smooth=3)
    toys.soft(b, toys.rounded(0.05, 0.028, 0.012), 0.012, 0.005, "PlasticDark", at=(0.282, 0.389, 0.075))
    # the seat: its pan, and over it the cushion in sections from the back to the waterfall front
    toys.soft(b, toys.rounded(0.46, 0.40, 0.06), 0.02, 0.008, "PlasticDark", at=(0, 0.424, 0.005))
    bottom = 0.441
    seat = [   # (z, wide, thick, how much it falls at the front, its dish)
        (-0.214, 0.40, 0.024, 0.0, 0.0), (-0.207, 0.445, 0.05, 0.0, 0.004), (-0.192, 0.47, 0.068, 0.0, 0.01),
        (-0.14, 0.482, 0.074, 0.0, 0.014), (-0.05, 0.492, 0.076, 0.0, 0.016), (0.04, 0.495, 0.076, 0.0, 0.015),
        (0.11, 0.494, 0.075, 0.003, 0.012), (0.16, 0.49, 0.073, 0.012, 0.008), (0.195, 0.484, 0.068, 0.028, 0.004),
        (0.218, 0.474, 0.058, 0.048, 0.0), (0.231, 0.455, 0.042, 0.066, 0.0), (0.237, 0.42, 0.022, 0.08, 0.0)]
    rings = []
    for z, w, t, fall, dish in seat:
        rings.append(chair_section(w, t, dish, bottom + 0.038 - fall + 0.012 * (z + 0.2) / 0.4, lambda x, y, z=z: (x, y, z)))
    loft(b, rings, "FabricChair", first=True, last=True, tile=CHAIR_WEAVE)
    # the back: sections from its foot up, each curved round the sitter (its edges forward), leaning
    # back 13 degrees, swelling forward at the small of the back; its shell behind it
    foot, top = 0.6, 1.13
    back = [(0.0, 0.40, 0.022), (0.012, 0.42, 0.046), (0.04, 0.435, 0.062), (0.15, 0.448, 0.066), (0.3, 0.462, 0.066),
            (0.5, 0.476, 0.064), (0.7, 0.48, 0.062), (0.86, 0.47, 0.06), (0.95, 0.45, 0.054), (0.985, 0.425, 0.04), (1.0, 0.395, 0.02)]

    def middle(f):   # the back's middle line in z, at a fraction f of its height
        return -0.285 - 0.12 * f + 0.032 * math.exp(-((f - 0.22) / 0.17) ** 2)
    for mat, out, thin in (("FabricChair", 0.0, 1.0), ("PlasticDark", -0.04, 0.25)):
        rings = []
        for f, w, t in back:
            y, mid = foot + (top - foot) * f, middle(f)
            w, t = (w if out == 0 else w - 0.012), t * thin

            def plane(x, across, y=y, mid=mid, w=w):
                return (x, y, mid + out + 0.032 * (2 * x / w) ** 2 + across)
            rings.append(chair_section(w, t, 0.006 if out == 0 else 0.0, 0.0, plane))
        loft(b, rings, mat, first=True, last=True, tile=CHAIR_WEAVE if out == 0 else 0.3)
    # the spine: a flat bar bent up from the mechanism to the shell's back
    shell = middle(0.28) - 0.04 - 0.008
    loft(b, carried([(0, 0.405, -0.06), (0, 0.405, -0.2), (0, 0.44, -0.3), (0, 0.56, shell - 0.012), (0, 0.78, shell - 0.004)],
                    rrect(0.062, 0.02, 0.008, n=3), (1, 0, 0)), "MetalBlack", first=True, last=True)
    # the arms: a flat bar out from under the seat and up, a soft pad on it
    for side in (-1, 1):
        loft(b, carried([(side * 0.08, 0.402, -0.03), (side * 0.25, 0.402, -0.03), (side * 0.29, 0.45, -0.03), (side * 0.29, 0.6, -0.025), (side * 0.29, 0.672, -0.02)],
                        rrect(0.05, 0.02, 0.008, n=3), (0, 0, 1)), "PlasticDark", first=True, last=True)
        toys.soft(b, toys.rounded(0.085, 0.27, 0.035), 0.028, 0.011, "PlasticDark", at=(side * 0.29, 0.672, 0.01))


PLUG_LONG = 0.05   # a D-sub plug from the panel it is in to the end of its sleeve


def dsub_plug(b, x, y, z, wide, sleeve=0.006):
    """A lead's D-sub plug in a socket of a face that looks towards +z: its steel shell in the
    socket, the hood on the panel's own sockets' rims, two thumbscrews, the sleeve the lead leaves by."""
    b.box((x, y, z + 0.004), (wide - 0.013, 0.0095, 0.008), "MetalSteel", bevel=0.001, segs=1)
    b.box((x, y, z + 0.0215), (wide, 0.0155, 0.029), "PlasticCase", bevel=0.003, segs=2)
    for k in (-1, 1):
        b.cyl((x + k * (wide / 2 - 0.0035), y, z + 0.0385), 0.0032, 0.006, "PlasticCase", segs=10, rot=(90, 0, 0))
    b.cyl((x, y, z + 0.043), sleeve, 0.014, "PlasticCase", segs=12, bevel=0.001, rot=(90, 0, 0))


STRIP = (0.36, 0.04, 0.06)               # a strip of sockets: long, high, wide
STRIP_SOCKETS, STRIP_PITCH = 5, 0.056    # its sockets, from 0 at the end its own lead leaves by
PLUG_HIGH = 0.034                        # a plug in it, to where its lead leaves it


def strip_socket(k, at=(0, 0, 0), turn=0.0, up=0.0):
    """The middle of socket k of a strip whose middle stands on `at`, turned `turn`: on its top, or `up` over it."""
    return tuple(lib.place(at, (0, turn, 0)) @ Vector((-0.134 + k * STRIP_PITCH, STRIP[1] + up, 0)))


def strip_box(at, turn=0.0):
    """The strip as a box for leads to fall on (drape.settle)."""
    return ((at[0], at[1] + STRIP[1] / 2, at[2]), STRIP, turn)


ADAPTER = (0.044, 0.058, 0.05)          # a plug-in supply (a hub's): along the strip, high, across
ADAPTER_HIGH = 0.072                     # from the strip's top to where its lead leaves it


def adapter(b, x, y):
    """A plug-in supply standing on a socket at x of a strip whose top is at y: a moulded block
    narrowing to its top, its rating plate, the strain relief its lead leaves by upwards."""
    w, h, d = ADAPTER

    def ring(dw, dd, r, at):
        return [(x + px, at, pz) for px, pz in rrect(w - dw, d - dd, r)]
    loft(b, [ring(0, 0, 0.005, y), ring(0, 0, 0.005, y + 0.003), ring(0.003, 0.003, 0.005, y + 0.004), ring(0.004, 0.004, 0.006, y + h - 0.008),
             ring(0.01, 0.01, 0.008, y + h)], "PlasticDark", first=True, last=True)
    b.box((x, y + 0.03, d / 2 - 0.0016), (0.03, 0.026, 0.0006), "PlasticGrey", bevel=0.0002, segs=1)
    b.lathe((x, y + h, 0), [(0.0046, 0), (0.0042, 0.003), (0.0032, 0.008), (0.0026, 0.012), (0.0023, ADAPTER_HIGH - h)], "Rubber", segs=12)


def adapter_box(at, k, turn=0.0):
    """A supply on socket k of a strip as a box for leads to fall on (drape.settle)."""
    return (strip_socket(k, at, turn, up=ADAPTER[1] / 2), ADAPTER, turn)


def strip(b, at, turn=0.0, plugs=(), adapters=()):
    """A strip of sockets lying on `at`: every socket drawn, a plug standing on each of `plugs`, a
    supply on each of `adapters`, a lit rocker at one end, the sleeve its own lead leaves by at the other (-x)."""
    long, high, wide = STRIP
    with b.at(at, (0, turn, 0)):
        b.box((0, high / 2, 0), STRIP, "PlasticBeige", bevel=0.005, segs=2)
        for k in range(STRIP_SOCKETS):
            x = -0.134 + k * STRIP_PITCH
            b.quad((x, high + 0.0003, 0), (0.044, 0.044), "Details", rot=(90, 0, 0), decal="strip_socket")
            if k in plugs:
                b.box((x, high + 0.011, 0), (0.034, 0.022, 0.034), "Rubber", bevel=0.005, segs=2)
                b.cyl((x, high + 0.027, 0), 0.0065, 0.014, "Rubber", segs=10, bevel=0.001)
            if k in adapters:
                adapter(b, x, high)
        b.box((0.151, high + 0.0008, 0), (0.03, 0.0016, 0.022), "PlasticDark", bevel=0.0005, segs=1)
        b.box((0.151, high + 0.0034, 0), (0.022, 0.005, 0.013), "Amber", bevel=0.0012, segs=1, rot=(0, 0, 7))   # the rocker, on
        b.cyl((-long / 2 - 0.006, high / 2, 0), 0.0075, 0.016, "PlasticBeige", segs=10, bevel=0.001, rot=(0, 0, 90))


HUB = (0.22, 0.036, 0.12)                # a small hub: wide, high, deep
HUB_PORTS, HUB_PITCH, HUB_LAMP = 8, 0.02, (0.005, 0.003)
HUB_POWER = 0.094                        # its supply's socket on its back, beyond the eighth port


def hub_lamp(k):
    """Port k's lamp on the hub's front, in the hub's own frame: the middle of its lens's face."""
    return (-0.07 + k * HUB_PITCH, 0.0245, -HUB[2] / 2 - 0.0018)


def hub_port(k):
    """Port k's socket on the hub's back: its middle, in the panel's face."""
    return (-0.07 + k * HUB_PITCH, 0.016, HUB[2] / 2)


def hub_power(power=HUB_POWER):
    """The hub's supply socket on its back, at x `power`: its middle, in the panel's face."""
    return (power, 0.016, HUB[2] / 2)


def hub(b, power=HUB_POWER):
    """An eight port hub of the decade at the frame's origin, on four feet: its lettered front
    with a dull lens a port towards -z (Unity lights them), its sockets behind, and its supply's
    socket beyond them at x `power` (a hub on a tower has it at the end towards the tower's back)."""
    w, h, d = HUB
    b.box((0, h / 2 + 0.002, 0), (w, h - 0.004, d), "PlasticGrey", bevel=0.004, segs=2)
    for sx in (-1, 1):
        for sz in (-1, 1):
            b.cyl((sx * (w / 2 - 0.02), 0.002, sz * (d / 2 - 0.02)), 0.008, 0.004, "Rubber", segs=10)
    b.quad((0, 0.02, -d / 2 - 0.0006), (0.21, 0.028), "Details", decal="hub_front")   # (0.3 mm off flickered)
    for k in range(HUB_PORTS):
        x, y, z = hub_lamp(k)
        b.box((x, y, z + 0.0008), HUB_LAMP + (0.0016,), "LampGreen")
        with b.at((0, 0, d / 2)):
            lan_socket(b, hub_port(k)[0], hub_port(k)[1], 0)
    b.box((-0.095, 0.0245, -d / 2 - 0.001), (0.004, 0.003, 0.0016), "Led")     # its power lamp: a hub is always on
    dc_inlet(b, *hub_power(power))
    for i in range(9):   # slots for its heat, across the top
        b.box((-0.08 + i * 0.02, h + 0.0002, 0.01), (0.008, 0.0008, 0.06), "PlasticDark")


def stickers(b, rng, count, skip=()):
    """`count` stickers (pc_faces.STICKERS) at chance places and turns on a place's blank faces, a
    fraction of a millimetre proud: no two over each other. skip: faces left bare."""
    e = 0.0004
    tz, top = TOWER_Z + FRONT + 0.216, LOW + OUT_H
    z0 = GLASS[3] + 0.113
    # a face: its middle, its turn (the picture's own up), half its width and height that are bare
    faces = {"tower right": ((TOWER_X + 0.09 + e, 0.185, tz), (0, -90), 0.17, 0.14), "tower left": ((TOWER_X - 0.09 - e, 0.27, tz), (0, 90), 0.17, 0.05),
             "tower top": ((TOWER_X, TOWER_HIGH + e, tz), (90, 0), 0.07, 0.17), "tower front": ((TOWER_X - 0.025, 0.1025, TOWER_Z + FRONT - e), (0, 0), 0.045, 0.0075),
             "bezel left": ((SET_X - 0.301, GLASS[2], GLASS[3] - e), (0, 0), 0.0115, 0.19), "bezel right": ((SET_X + 0.301, GLASS[2], GLASS[3] - e), (0, 0), 0.0115, 0.19),
             "monitor top": ((SET_X, top + e, GLASS[3] + 0.06), (90, 0), 0.29, 0.042), "monitor back top": ((SET_X, top - 0.003 + e, z0 + 0.045), (90, 0), 0.28, 0.034),
             "monitor left": ((SET_X - OUT_W / 2 - e, 0.335, GLASS[3] + 0.06), (0, 90), 0.042, 0.22), "monitor right": ((SET_X + OUT_W / 2 + e, 0.335, GLASS[3] + 0.06), (0, -90), 0.042, 0.22)}
    names, stuck = sorted(pc_faces.STICKERS), {}
    while count > 0:
        name, face = rng.choice(names), rng.choice(sorted(faces))
        w, h, round_ = pc_faces.STICKERS[name]
        spin = rng.choice((0, 0, 0, 90, -90)) + (rng.uniform(-25, 25) if rng.random() < 0.2 else rng.uniform(-9, 9))
        if face in skip:
            continue
        middle, (rx, ry), half_w, half_h = faces[face]
        c, s = abs(math.cos(math.radians(spin))), abs(math.sin(math.radians(spin)))
        reach = (w * c + h * s) / 2, (w * s + h * c) / 2
        if reach[0] > half_w or reach[1] > half_h:
            continue
        u, v = rng.uniform(-half_w + reach[0], half_w - reach[0]), rng.uniform(-half_h + reach[1], half_h - reach[1])
        if any(math.hypot(u - pu, v - pv) < (math.hypot(w, h) + size) / 2 for pu, pv, size in stuck.get(face, [])):
            continue
        stuck.setdefault(face, []).append((u, v, math.hypot(w, h)))
        with b.at(middle, (rx, ry, 0)):
            if round_:
                u0, v0, u1, v1 = b.atlas[name]
                ring = [(math.cos(2 * math.pi * k / 20), math.sin(2 * math.pi * k / 20)) for k in range(20)]
                b.mesh([(x * w / 2, y * h / 2, 0) for x, y in ring], [tuple(range(20))],
                       [[((u0 + u1) / 2 + x * (u1 - u0) * 0.49, (v0 + v1) / 2 + y * (v1 - v0) * 0.49) for x, y in ring]], "Details",
                       at=(u, v, 0), rot=(0, 0, spin), normal=(0, 0, -1))
            else:
                b.quad((u, v, 0), (w, h), "Details", rot=(0, 0, spin), decal=name)
        count -= 1


LEADS = []   # the last station()'s leads: [name, the points of its middle, its radius, its material, its sides]


def lead(b, name, corners, radius, mat, segs=8, bend=0.06, rng=None, up=0.04):
    """A lead as it is laid out before it falls: from corner to corner, each corner rounded within
    `bend` of it, then ruffled by chance. station() lets them all fall together and makes their tubes."""
    points = [Vector(c) for c in corners]
    path = [points[0]]
    for i in range(1, len(points) - 1):
        into, out = points[i] - points[i - 1], points[i + 1] - points[i]
        d = min(bend, into.length / 2, out.length / 2)
        start, end = points[i] - into.normalized() * d, points[i] + out.normalized() * d
        for k in range(6):
            t = k / 5
            path.append(start * (1 - t) ** 2 + points[i] * (2 * t * (1 - t)) + end * t ** 2)
    path.append(points[-1])
    if rng is not None:
        path = drape.ruffled(path, rng, up=up * rng.uniform(0.3, 1.0))
    LEADS.append([name, path, radius, mat, segs])


TABLE_HIGH = 0.765   # the classroom's table top over the floor (furniture.TABLE_TOP): the mains leads reach the floor
SET_X, TOWER_X, TOWER_Z = -0.11, 0.38, 0.05   # the monitor and keyboard a little left, the tower at their right


STRIP_AT = [None]   # where the last station() laid its strip of sockets
HUB_SOCKET = 4      # the strip's socket a hub's supply is plugged into, at a place that has one


def station(b, which="all", lie=None, desk=None, seed=0, stuck=0, bare=(), supply=False, lan=None):
    """A whole place on the table at the frame's origin: the monitor and keyboard, the tower
    beside them, the mouse before the tower, and their leads. which: "shell" is the monitor and
    the tower alone, "rest" all but them (with the tube's glass, which no mode but "full" gives).
    lie: how this place's keyboard, mouse and mat lie off their places, each (x, z, turn).
    desk: the boxes its leads fall on besides its own things, the table's top first (centre, size,
    turn), in this frame; seed: the chance its leads are laid out by, a place its own.
    stuck: how many stickers it has, on any face but those of `bare` (stickers()).
    supply: a hub's supply stands on its strip's socket HUB_SOCKET (its lead is the network's).
    lan: the network's socket in a wall behind the table (x, y, z of its face, which looks towards
    -z): a lead from the card falls into it with the others, and the wall is what they fall on too."""
    lie = lie or {"keyboard": (0, 0, 0), "mouse": (0, 0, 0), "mat": (0, 0, 0)}
    kx, kz, kturn = lie["keyboard"]
    mx, mz, mturn = lie["mouse"]
    px, pz, pturn = lie["mat"]
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
        with b.at((kx, 0, KEYBOARD_Z + kz)):
            with b.at((0, 0, 0), (0, kturn, 0)):
                with b.at((0, 0, -KEYBOARD_Z)):
                    keyboard(b)
    mat_at = (TOWER_X - 0.01 + px, 0, -0.31 + pz)
    with b.at(mat_at, (0, pturn, 0)):
        b.box((0, 0.0015, 0), (0.21, 0.003, 0.25), "Rubber", bevel=0.001, segs=1)   # the mat, lying flat
    mouse_at = (mat_at[0] + mx, 0, mat_at[2] + mz)
    if stuck:
        stickers(b, random.Random(7000 + seed), stuck, bare)
    with b.at(mouse_at, (0, mturn, 0)):
        mouse_body(b)
    behind = TOWER_Z + FRONT + TOWER_DEEP   # the tower's back
    floor = -TABLE_HIGH
    desk = desk or [((0, -0.022, 0), (2.4, 0.044, 0.9), 0)]
    # the table's back edge where this place's things are: nearest and furthest (a place is turned on its table)
    top = lib.place(desk[0][0], (0, desk[0][2], 0))
    left, right = top @ Vector((-desk[0][1][0] / 2, 0, desk[0][1][2] / 2)), top @ Vector((desk[0][1][0] / 2, 0, desk[0][1][2] / 2))
    behind_at = [left.z + (right.z - left.z) * (x - left.x) / (right.x - left.x) for x in (SET_X - 0.35, TOWER_X + 0.15)]
    near, edge = min(behind_at), max(behind_at)
    at = tower_back()
    # The leads. Each is laid out behind the units clear of the others, by this place's own chance,
    # and then they fall together where cables fall (drape.settle, Blender's cloth): held only at
    # their plugs, lying on the table, on each other and over its back edge. drape.check after.
    rng = random.Random(1000 + seed)
    del LEADS[:]
    # the picture's lead: from RGB 1 on the monitor's board to the video card's socket, a plug at each end
    mx, my = SET_X + BOARD_X + 0.085, plug_y - BOARD_DOWN + ROW_TOP
    vx, vy = TOWER_X + at["video"][0], at["video"][1]
    r = 0.0045
    on_table = near - 0.009 >= back + PLUG_LONG + 0.005   # room for it behind the monitor's plug: else it hangs behind the table
    lane_picture, low = (near - 0.009, r + 0.0005) if on_table else (edge + 0.014, rng.uniform(-0.12, 0.02))
    lead(b, "picture", [(mx, my, back + PLUG_LONG - 0.01), (mx, my, lane_picture), (mx, low, lane_picture), (vx, low, lane_picture),
                        (vx, vy, lane_picture), (vx, vy, behind + PLUG_LONG - 0.01)], r, "PlasticCase", rng=rng)
    dsub_plug(b, mx, my, back, 0.032)
    dsub_plug(b, vx, vy, behind, 0.032)
    # the sound's lead lies on the table before the picture's where there is room behind the monitor, else it hangs behind
    lane_sound = min(back + 0.046, lane_picture - 0.012 if on_table else near - 0.008)
    sound_on_table = lane_sound >= back + 0.03
    most = min(near - 0.012, lane_picture - 0.016, lane_sound - 0.012 if sound_on_table else 1)   # the furthest back a lane on the table may be
    lane_keys, lane_mouse = (rng.uniform(behind + 0.035, most) for _ in range(2))
    round_left = right.x < TOWER_X + 0.14   # the table ends by the tower: the mouse's lead goes round its other side
    if round_left:   # (inside the keyboard's there, and in a nearer lane, or they would be laid through each other)
        lane_keys = max(lane_keys, behind + 0.052)
        lane_mouse = rng.uniform(behind + 0.035, lane_keys - 0.014)
    # a strip of sockets on the floor behind the table: both mains leads end in it, its own goes on under the table
    lies = (0.06 + rng.uniform(-0.04, 0.04), floor, edge + 0.07 + rng.uniform(0, 0.04))
    out, end = lies[2], lies[0] - STRIP[0] / 2 - 0.012
    strip(b, lies, plugs=(1, 3), adapters=(HUB_SOCKET,) if supply else ())   # (the others are empty)
    STRIP_AT[0] = lies
    lead(b, "strip", [(end, floor + 0.02, out), (end - 0.05, floor + 0.005, out), (end - 0.10, floor + 0.005, out),
                      (end - 0.10, floor + 0.005, near - 0.3 + 0.03), (end - 0.10, floor + 0.03, near - 0.3)], 0.004, "Rubber", bend=0.02, rng=rng, up=0.02)
    # and ends in a socket let into the floor under the table: a steel plate, the plug in it
    b.box((end - 0.10, floor + 0.002, near - 0.3), (0.12, 0.004, 0.12), "MetalSteel", bevel=0.001, segs=1)
    b.box((end - 0.10, floor + 0.017, near - 0.3), (0.036, 0.026, 0.036), "Rubber", bevel=0.006, segs=2)
    # each unit's mains lead: out of its inlet, in an arc over the table's edge, down behind it and along to the strip
    for name, x, y, z, k in (("monitor mains", SET_X - 0.14, plug_y - BOARD_DOWN, back, 1),
                             ("tower mains", TOWER_X + at["inlet"][0], at["inlet"][1], behind, 3)):
        # (with length to spare: it comes down to the floor, lies there, and rises into its plug)
        socket = strip_socket(k, lies)[0]
        over, lying = max(edge + 0.035, z + 0.07) + rng.uniform(0, 0.03), out + rng.uniform(0.05, 0.12)   # (no further: the next row's chairs stand there)
        lead(b, name, [(x, y, z + 0.02), (x, y, z + 0.05), (x, y - 0.05, over), (x, -0.12, over + 0.01), (x, floor + 0.05, over + 0.02),
                       ((x + socket) / 2 + rng.uniform(-0.08, 0.08), floor + 0.012, lying), (socket + rng.uniform(-0.03, 0.03), floor + 0.03, lying - 0.04),
                       (socket, floor + 0.14, out + 0.02), strip_socket(k, lies, up=PLUG_HIGH - 0.004)], 0.0035, "Rubber", bend=0.05, rng=rng, up=0.02)
        b.box((x, y, z + 0.014), (0.03, 0.022, 0.028), "Rubber", bevel=0.004, segs=2)   # its moulded plug (IEC C13)
    # the keyboard's lead: from the sleeve on the tray's back (wherever this place's keyboard lies and however
    # it is turned), before the monitor's foot, down the tower's left side and round behind it to the DIN socket
    din_x, din_y = TOWER_X + at["keyboard"][0], at["keyboard"][1]
    tray = (lib.place((SET_X + kx, 0, KEYBOARD_Z + kz), (0, kturn, 0)) @ lib.place((0, 0, -KEYBOARD_Z)) @ lib.place(TRAY_AT, TRAY_TURN))
    far = KEYS_HIGH / 2 + 0.015 + TRAY_BACK
    k0, k1 = tray @ Vector((LEAD_X, far + 0.006, 0.001)), tray @ Vector((LEAD_X, far + 0.024, 0.002))
    # (the foot is 0.208 round, 0.1 behind the set's middle: this is clear of it)
    r, side, before = 0.0028, TOWER_X - 0.124 + rng.uniform(-0.008, 0.006), -0.125 + rng.uniform(-0.015, 0.01)
    lead(b, "keyboard", [tuple(k0), tuple(k1), (k1.x + 0.012, r + 0.0005, before), (side, r + 0.0005, before), (side, r + 0.0005, lane_keys),
                         (din_x, r + 0.0005, lane_keys), (din_x, din_y, lane_keys), (din_x, din_y, behind + 0.02)], r, "PlasticCase", segs=6, rng=rng, up=0.025)
    b.cyl((din_x, din_y, behind + 0.004), 0.0062, 0.008, "MetalSteel", segs=14, rot=(90, 0, 0))                    # the DIN plug: its shell in the socket,
    b.cyl((din_x, din_y, behind + 0.02), 0.0078, 0.026, "PlasticCase", segs=14, bevel=0.002, rot=(90, 0, 0))     # its grip,
    b.cyl((din_x, din_y, behind + 0.038), 0.0045, 0.012, "PlasticCase", segs=10, rot=(90, 0, 0))                 # and its sleeve
    # the mouse's: from the sleeve under its buttons (however this place's mouse is turned), off the mat past
    # the tower's front corner, down its right side and round behind it to COM 1
    cx, cy = TOWER_X + at["com1"][0], at["com1"][1]
    body = lib.place(mouse_at, (0, mturn, 0)) @ lib.place((0, 0, 0), (0, MOUSE_TURN, 0))
    m0, m1 = body @ Vector(MOUSE_LEAD), body @ Vector((MOUSE_LEAD[0], 0.0075, MOUSE_LEAD[2] - 0.03))
    r, side = 0.0022, TOWER_X - 0.101 if round_left else TOWER_X + 0.105 + rng.uniform(0, 0.02)
    lead(b, "mouse", [tuple(m0), tuple(m1), (side, 0.0065, TOWER_Z + FRONT - 0.03), (side, r + 0.0005, TOWER_Z + FRONT + 0.06), (side, r + 0.0005, lane_mouse),
                      (cx, r + 0.0005, lane_mouse), (cx, cy, lane_mouse), (cx, cy, behind + PLUG_LONG - 0.01)], r, "PlasticCase", segs=6, rng=rng, up=0.025)
    dsub_plug(b, cx, cy, behind, 0.031, sleeve=0.004)
    # the sound: from the card's line out (the jack by its joystick socket) to the monitor's two phono
    # sockets, parting behind the monitor in a moulded Y that hangs from a white tail and a red one
    face, sy = back + 0.0004, plug_y - BOARD_DOWN + ROW_LOW
    sockets = [(SET_X + BOARD_X - BOARD[name][0], name) for name in ("audio_l", "audio_r")]
    yx, yz = sum(x for x, _ in sockets) / 2, face + RCA_LONG + 0.016
    yy = sy - 0.05 - Y_TOP
    y_split(b, yx, yy, yz)
    for x, name in sockets:
        rca_plug(b, x, sy, face, "Ceramic" if name == "audio_l" else "RedPaint")
        tx = yx + (0.0028 if x > yx else -0.0028)
        lead(b, "sound " + name[-1], [(x, sy, face + RCA_LONG - 0.002), (x, sy, yz), (tx, yy + Y_TOP + 0.03, yz), (tx, yy + Y_TOP - 0.002, yz)],
             0.0017, "Rubber", segs=6)
    jx, jy = TOWER_X + 0.012, at["first"] - 3 * SLOT_PITCH
    jack_plug(b, jx, jy, behind + 0.0018)
    r, start = 0.002, [(yx, yy - Y_LOW + 0.002, yz), (yx, yy - Y_LOW - 0.03, yz)]
    if sound_on_table:
        way = [(yx, r + 0.0005, lane_sound), (jx, r + 0.0005, lane_sound)]
    else:   # over the table's back edge, along behind it, and up again behind the tower
        lane_sound, low = edge + 0.028, rng.uniform(-0.14, -0.03)
        way = [(yx, 0.012, edge + 0.012), (yx, low, lane_sound), (jx, low, lane_sound)]
    # (it rises left of the jack: the network's lead leaves the card below it to the right)
    way[-1] = (jx - 0.02,) + way[-1][1:]
    lead(b, "sound", start + way + [(jx - 0.02, jy - 0.03, lane_sound), (jx, jy, behind + 0.06), (jx, jy, behind + 0.0018 + JACK_LONG - 0.002)], r, "Rubber",
         segs=6, rng=rng, up=0.02)
    if lan is not None:
        # the network: from the card's socket back over the table's edge, down behind it and into the wall's socket
        lx, ly, lz = lan
        nx, ny = TOWER_X + at["lan"][0], at["lan"][1]
        lan_plug(b, nx, ny, behind, "CableBlue")
        with b.at((lx, ly, lz), (0, 180, 0)):
            lan_plug(b, 0, 0, 0, "CableBlue")
        r, down = 0.003, nx + rng.uniform(0.015, 0.03)
        lead(b, "lan", [(nx, ny + 0.0006, behind + LAN_PLUG - 0.004), (nx, ny, behind + 0.05), (down, 0.012, edge - 0.01), (down, 0.006, edge + 0.012), (down, -0.03, edge + 0.03),
                        (down, -0.3, edge + 0.04), (lx, ly - 0.06, lz - 0.05), (lx, ly + 0.0006, lz - 0.03), (lx, ly + 0.0006, lz - LAN_PLUG + 0.004)],
             r, "CableBlue", bend=0.012, rng=rng, up=0.01)
    # what the leads fall on: the desk, the floor, the tower, the monitor (case, foot, neck), the keyboard, the mat, the strip
    boxes = list(desk) + [((0, floor - 0.05, 0), (4, 0.1, 4), 0),
                          ((TOWER_X, TOWER_HIGH / 2, TOWER_Z + FRONT + TOWER_DEEP / 2), (0.184, TOWER_HIGH, TOWER_DEEP), 0),
                          ((SET_X, LOW + OUT_H / 2, (GLASS[3] - 0.02 + back) / 2), (OUT_W, OUT_H, back - GLASS[3] + 0.02), 0),
                          ((SET_X + kx, 0.015, KEYBOARD_Z + kz), (0.58, 0.03, 2 * (KEYS_HIGH / 2 + 0.015 + TRAY_BACK) - 0.006), kturn),
                          ((mat_at[0], 0.0015, mat_at[2]), (0.21, 0.003, 0.25), pturn),
                          strip_box(lies)] + ([adapter_box(lies, HUB_SOCKET)] if supply else []) +         ([((0, 0, lan[2] + 0.05), (4, 4, 0.1), 0)] if lan is not None else [])   # (the wall)
    cylinders = [((SET_X, 0.017, 0.1), 0.208, 0.034), ((SET_X, 0.05, 0.1), 0.08, 0.04)]
    fallen = drape.settle([(name, path, radius) for name, path, radius, _, _ in LEADS], boxes, cylinders)
    for one, path in zip(LEADS, fallen):
        one[1] = path   # (kept: the room's network leads are laid clear of these)
        b.tube([tuple(q) for q in path], one[2], one[3], segs=one[4], smooth=0)
    drape.check([(name, path, radius) for name, path, radius, _, _ in LEADS], boxes, cylinders)
