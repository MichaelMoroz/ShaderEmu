# A desktop computer of the middle of the 1990s, for the classroom's eight places: a monitor of
# twice a 15 inch tube's diagonal, a keyboard and a mouse on the table, the tower on the floor.
# Everything is in a place's own frame: x to the sitter's right, y up from the table, the sitter
# at -z. The glass is a material of its own ("TubeGlass"), its picture's corners at the corners
# of what the bezel shows of it.
import json
import math
import os

FRONT = -0.150                            # the tower's front face, in its own frame
S = 2.0                                   # the tube against a 15 inch one: a visitor at a desktop must be able to read it
GLASS = (0.288 * S, 0.218 * S, 0.457, -0.12)   # what shows of the tube: wide, high, its middle's height, its front
BULGE = 0.95 * S                          # the glass is part of a ball of this radius
OUT_W, OUT_H, OUT_Y = 0.372 * S, 0.336 * S, GLASS[2] - 0.018 * S   # the bezel's outside: a deeper chin than brow
LOW = OUT_Y - OUT_H / 2
CHIN = (LOW + GLASS[2] - GLASS[1] / 2 - 0.006 * S) / 2


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
    run, taper = 0.07, 0.25                     # its straight part, then its narrowing
    back_w, back_h, back_y = OUT_W * 0.62, OUT_H * 0.66, OUT_Y - 0.012
    # the bezel: its front face in to the opening, a chamfer, flat sides, closed behind
    loft(b, [at_z(rrect(w + 0.024, h + 0.024, 0.02, 0, mid), front), at_z(rrect(OUT_W - 0.014, OUT_H - 0.014, 0.01, 0, OUT_Y), front),
             at_z(rrect(OUT_W, OUT_H, 0.012, 0, OUT_Y), front + 0.009), at_z(rrect(OUT_W, OUT_H, 0.012, 0, OUT_Y), front + block),
             at_z(rrect(OUT_W - 0.03, OUT_H - 0.03, 0.01, 0, OUT_Y), front + block)], "PlasticBeige", last=True)
    # what shows in the seam
    loft(b, [at_z(rrect(OUT_W - 0.02, OUT_H - 0.02, 0.01, 0, OUT_Y), front + block - 0.004),
             at_z(rrect(OUT_W - 0.02, OUT_H - 0.02, 0.01, 0, OUT_Y), z0 + 0.004)], "PlasticDark")
    # the housing
    loft(b, [at_z(rrect(OUT_W - 0.03, OUT_H - 0.03, 0.01, 0, OUT_Y), z0), at_z(rrect(OUT_W - 0.006, OUT_H - 0.006, 0.012, 0, OUT_Y), z0),
             at_z(rrect(OUT_W - 0.006, OUT_H - 0.006, 0.012, 0, OUT_Y), z0 + run), at_z(rrect(back_w, back_h, 0.012, 0, back_y), z0 + run + taper),
             at_z(rrect(back_w - 0.016, back_h - 0.016, 0.008, 0, back_y), z0 + run + taper + 0.01)], "PlasticCase", first=True, last=True)
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
    for i in range(6):   # and a grille in the straight part, low on each side
        for side in (-1, 1):
            b.box((side * (side0 - 0.0006), LOW + 0.07, z0 + 0.012 + i * 0.01), (0.003, 0.07, 0.005), "PlasticDark")
    # behind: the sockets the leads go into, and the maker's label
    b.box((0.0, back_y - 0.08, z0 + run + taper + 0.011), (0.05, 0.02, 0.004), "MetalSteel", bevel=0.001, segs=1)
    b.box((-0.1, back_y - 0.08, z0 + run + taper + 0.011), (0.03, 0.026, 0.004), "PlasticDark", bevel=0.001, segs=1)
    b.quad((0, back_y + 0.05, z0 + run + taper + 0.0105), (0.16, 0.09), "Paper", rot=(0, 180, 0))
    for sx in (-1, 1):
        for sy in (-1, 1):
            b.cyl((sx * (back_w / 2 - 0.035), back_y + sy * (back_h / 2 - 0.035), z0 + run + taper + 0.0102), 0.0045, 0.003, "MetalSteel", segs=8, rot=(90, 0, 0))
    for i in range(8):
        b.box((0, back_y - 0.14 - i * 0.012, z0 + run + taper + 0.0102), (back_w * 0.6, 0.005, 0.0016), "PlasticDark")
    for x in (0.0, -0.1):   # where a lead leaves: a ribbed sleeve
        b.lathe((x, back_y - 0.08, z0 + run + taper + 0.012), [(0.009, 0), (0.009, 0.004), (0.007, 0.006), (0.0075, 0.012), (0.006, 0.014), (0.0065, 0.02), (0.005, 0.022)],
                "Rubber", segs=12, rot=(90, 0, 0), cap=False)
    # the opening's own slope, down to the glass
    loft(b, [at_z(rrect(w + 0.024, h + 0.024, 0.02, 0, mid), front), at_z(rrect(w + 0.008, h + 0.008, 0.016, 0, mid), front + 0.014),
             at_z(rrect(w, h, 0.014, 0, mid), front + 0.03)], "PlasticGrey", normal=(0, 0, -1))
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
    # under the glass: the power button and its lamp, four keys for the picture
    round_button(b, OUT_W / 2 - 0.06, CHIN, front, 0.014, "PlasticBeige")
    b.cyl((OUT_W / 2 - 0.095, CHIN, front - 0.0005), 0.004, 0.003, "Led", segs=10, rot=(90, 0, 0))
    for i in range(4):
        round_button(b, 0.05 + i * 0.034, CHIN, front, 0.008, "PlasticGrey", 0.002)
    b.box((-OUT_W / 2 + 0.09, CHIN, front - 0.0008), (0.07, 0.016, 0.0016), "MetalSteel", bevel=0.0006, segs=1)   # the maker's plate
    # the foot it turns and tilts on
    b.lathe((0, 0, 0.1), [(0, 0), (0.2, 0), (0.208, 0.008), (0.2, 0.02), (0.13, 0.034), (0.11, 0.042), (0, 0.042)], "PlasticBeige", segs=40)
    b.lathe((0, 0.04, 0.1), [(0.105, 0), (0.115, 0.014), (0.13, LOW - 0.04 - 0.012), (0.14, LOW - 0.04 + 0.03), (0, LOW - 0.04 + 0.03)], "PlasticCase", segs=40)
    return z0 + run + taper + 0.011, back_y   # where its back is


def drive_gap(b, x, y, w, h):
    """The dark line round a drive's face, where it meets the case."""
    for dx, dy, sw, sh in ((0, h / 2, w, 0.0012), (0, -h / 2, w, 0.0012), (w / 2, 0, 0.0012, h), (-w / 2, 0, 0.0012, h)):
        b.box((x + dx, y + dy, FRONT - 0.0002), (sw + 0.0012, sh, 0.0012), "PlasticDark")


def cd_drive(b, cx, cy):
    """A CD-ROM drive's face: its tray, the key that opens it, a lamp, the wheel and the socket for headphones."""
    cw, ch = 0.148, 0.042
    drive_gap(b, cx, cy, cw, ch)
    b.box((cx, cy, FRONT - 0.0008), (cw - 0.002, ch - 0.002, 0.002), "PlasticBeige", bevel=0.0008, segs=1)
    b.box((cx, cy + 0.008, FRONT - 0.0028), (cw - 0.012, 0.016, 0.003), "PlasticBeige", bevel=0.0012, segs=2)       # the tray
    b.box((cx, cy + 0.0005, FRONT - 0.0018), (cw - 0.012, 0.0008, 0.001), "PlasticDark")
    b.box((cx + 0.054, cy - 0.012, FRONT - 0.003), (0.018, 0.006, 0.003), "PlasticCase", bevel=0.0012, segs=2)       # eject
    b.box((cx + 0.034, cy - 0.012, FRONT - 0.002), (0.006, 0.003, 0.002), "Amber")
    b.cyl((cx - 0.058, cy - 0.012, FRONT - 0.0012), 0.0032, 0.002, "PlasticDark", segs=12, rot=(90, 0, 0))            # headphones
    b.cyl((cx - 0.058, cy - 0.012, FRONT - 0.0016), 0.0048, 0.0012, "MetalSteel", segs=12, rot=(90, 0, 0))
    b.cyl((cx - 0.038, cy - 0.012, FRONT - 0.001), 0.006, 0.004, "PlasticDark", segs=16, rot=(0, 0, 90))              # the volume's wheel, on edge
    b.cyl((cx + 0.012, cy - 0.012, FRONT - 0.0008), 0.0008, 0.001, "PlasticDark", segs=6, rot=(90, 0, 0))             # the hole for a paper clip


def floppy_drive(b, fx, fy):
    """A floppy drive's face: the slot with its flap, the eject key, a lamp."""
    fw, fh = 0.102, 0.026
    drive_gap(b, fx, fy, fw, fh)
    b.box((fx, fy, FRONT - 0.0008), (fw - 0.002, fh - 0.002, 0.002), "PlasticBeige", bevel=0.0008, segs=1)
    b.box((fx - 0.004, fy + 0.004, FRONT - 0.0019), (0.09, 0.0045, 0.001), "PlasticDark")
    b.box((fx - 0.004, fy + 0.0052, FRONT - 0.0022), (0.088, 0.0016, 0.001), "PlasticGrey")
    b.box((fx + 0.034, fy - 0.006, FRONT - 0.0045), (0.012, 0.006, 0.005), "PlasticCase", bevel=0.0012, segs=2)
    b.box((fx - 0.04, fy - 0.006, FRONT - 0.002), (0.005, 0.0025, 0.002), "Led")


def rear(b, w, h, back):
    """The tower's back, which a visitor behind the table sees: bare steel, the power supply with
    its fan and sockets, the board's sockets, seven slots' covers (one with a card in it), screws."""
    z = back + 0.0006
    b.box((0, h / 2 + 0.006, back + 0.0002), (w - 0.006, h - 0.02, 0.001), "MetalSteel")
    # the power supply, at the top: a fan's grille of rings and spokes, the mains socket, the switch
    fan = (0.03, h - 0.075)
    b.cyl((fan[0], fan[1], z), 0.043, 0.0012, "PlasticDark", segs=28, rot=(90, 0, 0))
    for radius in (0.012, 0.022, 0.032, 0.042):
        b.lathe((fan[0], fan[1], z + 0.0004), [(radius - 0.0012, 0), (radius - 0.0012, 0.0016), (radius + 0.0012, 0.0016), (radius + 0.0012, 0)], "MetalSteel",
                segs=28, rot=(90, 0, 0), cap=False)
    for k in range(4):
        b.box((fan[0], fan[1], z + 0.0012), (0.086, 0.0024, 0.0016), "MetalSteel", rot=(0, 0, k * 45))
    b.box((-0.055, h - 0.05, z + 0.001), (0.032, 0.024, 0.004), "PlasticDark", bevel=0.002, segs=1)      # the mains socket
    b.box((-0.055, h - 0.095, z + 0.0015), (0.022, 0.014, 0.005), "RedPaint", bevel=0.002, segs=2)       # the switch
    for x, y in ((-0.085, h - 0.02), (0.085, h - 0.02), (-0.085, h - 0.13), (0.085, h - 0.13)):
        b.cyl((x, y, z + 0.0006), 0.0035, 0.002, "MetalSteel", segs=8, rot=(90, 0, 0))
    # the board's own sockets: keyboard (round), two serial, a parallel
    b.cyl((-0.06, h - 0.165, z + 0.001), 0.008, 0.004, "PlasticDark", segs=16, rot=(90, 0, 0))
    b.lathe((-0.06, h - 0.165, z), [(0.008, 0), (0.008, 0.005), (0.0105, 0.005), (0.0105, 0)], "MetalSteel", segs=16, rot=(90, 0, 0), cap=False)
    for x, wide in ((-0.015, 0.03), (0.03, 0.03)):
        b.box((x, h - 0.165, z + 0.0015), (wide, 0.012, 0.005), "MetalSteel", bevel=0.002, segs=1)
        b.box((x, h - 0.165, z + 0.0042), (wide - 0.008, 0.006, 0.0006), "PlasticDark")
    b.box((0.02, h - 0.19, z + 0.0015), (0.054, 0.012, 0.005), "MetalSteel", bevel=0.002, segs=1)
    b.box((0.02, h - 0.19, z + 0.0042), (0.046, 0.006, 0.0006), "GamepadBlue")
    # seven slots, their covers held by a screw each; the second has the video card's socket
    for i in range(7):
        y = h - 0.225 - i * 0.0204
        b.box((0.0, y, z + 0.0006), (0.118, 0.0165, 0.0014), "MetalSteel", bevel=0.0005, segs=1)
        b.box((0.0, y, z + 0.0015), (0.1, 0.004, 0.0004), "PlasticDark")
        b.cyl((0.07, y, z + 0.0012), 0.003, 0.002, "MetalSteel", segs=8, rot=(90, 0, 0))
        if i == 1:
            b.box((-0.01, y, z + 0.003), (0.034, 0.011, 0.006), "MetalSteel", bevel=0.002, segs=1)
            b.box((-0.01, y, z + 0.0062), (0.026, 0.006, 0.0006), "GamepadBlue")
    for x in (-0.088, 0.088):   # the cover's screws down each side
        for y in (0.03, h * 0.4, h - 0.16):
            b.cyl((x, y, z + 0.0006), 0.0038, 0.0022, "MetalSteel", segs=8, rot=(90, 0, 0))


def tower(b):
    """The computer itself, standing on the floor: its foot at the frame's origin, its front at FRONT."""
    w, h, deep = 0.19, 0.43, 0.42
    back = FRONT + deep
    # the steel cover, the plastic front with rounded edges, the feet
    b.box((0, h / 2 + 0.006, FRONT + 0.012 + (deep - 0.012) / 2), (w, h - 0.012, deep - 0.012), "PlasticCase", bevel=0.004, segs=2)
    loft(b, [at_z(rrect(w + 0.004, h, 0.008, 0, h / 2 + 0.006), FRONT + 0.016), at_z(rrect(w + 0.004, h, 0.01, 0, h / 2 + 0.006), FRONT + 0.004),
             at_z(rrect(w - 0.002, h - 0.006, 0.01, 0, h / 2 + 0.006), FRONT)], "PlasticBeige", last=True, normal=(0, 0, -1))
    for x in (-0.07, 0.07):
        for z in (FRONT + 0.05, back - 0.05):
            b.cyl((x, 0.003, z), 0.014, 0.006, "Rubber", segs=14)
    rear(b, w, h, back)
    for i in range(12):   # the slots in its side
        b.box((-w / 2 - 0.0003, 0.12, FRONT + 0.16 + i * 0.014), (0.001, 0.12, 0.006), "PlasticDark")
    # from the top: the CD-ROM drive, a blank bay of its size, the floppy drive
    cd_drive(b, 0, h - 0.04)
    drive_gap(b, 0, h - 0.088, 0.148, 0.042)
    b.box((0, h - 0.088, FRONT - 0.0006), (0.146, 0.04, 0.0016), "PlasticBeige", bevel=0.0008, segs=1)
    floppy_drive(b, 0.02, h - 0.132)
    # then the big power key in its well, reset and turbo, three lamps, the key lock, a badge, and the grille below
    px, py = -0.045, h - 0.19
    b.box((px, py, FRONT - 0.0004), (0.04, 0.03, 0.001), "PlasticCase", bevel=0.0004, segs=1)
    b.box((px, py, FRONT - 0.004), (0.034, 0.024, 0.006), "PlasticBeige", bevel=0.003, segs=3)
    b.box((px, py, FRONT - 0.0072), (0.016, 0.0016, 0.0006), "PlasticGrey")
    for i in range(2):   # reset, turbo
        b.box((px + 0.045 + i * 0.024, py - 0.006, FRONT - 0.003), (0.014, 0.008, 0.004), "PlasticCase", bevel=0.0016, segs=2)
    for i, mat in enumerate(("Led", "Amber", "Led")):
        b.box((px + 0.04 + i * 0.014, py + 0.008, FRONT - 0.0016), (0.007, 0.003, 0.002), mat, bevel=0.0006, segs=1)
    b.cyl((px + 0.1, py - 0.03, FRONT - 0.002), 0.009, 0.004, "MetalSteel", segs=20, bevel=0.001, rot=(90, 0, 0))       # the lock
    b.box((px + 0.1, py - 0.03, FRONT - 0.0042), (0.002, 0.009, 0.0006), "PlasticDark")
    b.box((px, py - 0.045, FRONT - 0.0012), (0.036, 0.014, 0.0016), "MetalSteel", bevel=0.0006, segs=1)                 # the badge
    b.box((px, py - 0.045, FRONT - 0.0021), (0.03, 0.009, 0.0004), "GamepadBlue")
    for row in range(3):
        for i in range(16):
            b.box((-0.064 + i * 0.0085, 0.05 + row * 0.042, FRONT - 0.0002), (0.0045, 0.03, 0.0012), "PlasticDark", bevel=0.0004, segs=1)


def keyboard(b):
    """The keyboard: world/keyboard.json's layout at a third of the console's size, caps that
    narrow to their tops, in a tray that slopes."""
    layout = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "keyboard.json"), encoding="utf-8"))
    wide, tall, s = layout["width"], layout["height"], 0.00033
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
    for x0, y0, w, h, legend in layout["keys"]:
        if legend.startswith("Use my"):
            continue   # the console's own switch
        base = ring(x0 + 1.5, y0 + 1.5, x0 + w - 1.5, y0 + h - 1.5, 0)
        shoulder = ring(x0 + 4.0, y0 + 3.0, x0 + w - 4.0, y0 + h - 6.0, -0.0065)
        top = ring(x0 + 6.0, y0 + 4.5, x0 + w - 6.0, y0 + h - 8.0, -0.0078)
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
    for x in (-0.18, 0.18):   # the feet that fold out behind
        b.box((x, 0.008, -0.242), (0.03, 0.016, 0.008), "PlasticCase", bevel=0.002, segs=1)


def mouse(b):
    """A mouse with two buttons on a mat."""
    b.box((0, 0.0015, 0), (0.21, 0.003, 0.25), "Rubber", bevel=0.001, segs=1)   # the mat, lying flat
    mouse_body(b)


def mouse_body(b):
    """The mouse itself, standing on y = 0.004: sections of a pebble, flat below, humped under the
    palm, its buttons away from the sitter (+z)."""
    with b.at((0, 0, 0)):
        rings = []
        for k, t in enumerate((0.0, 0.06, 0.2, 0.4, 0.6, 0.8, 0.94, 1.0)):
            z = 0.052 - t * 0.104                         # from its tail (towards the sitter's wrist) to the buttons' edge
            half = 0.031 * (1 - (2 * t - 1) ** 4) ** 0.5 + 0.002
            high = 0.012 + 0.024 * math.sin(math.pi * (0.12 + 0.72 * (1 - t))) ** 1.2
            ring = []
            for a in range(14):
                angle = 2 * math.pi * a / 14
                x, y = math.cos(angle), math.sin(angle)
                ring.append((x * half, 0.004 + (high * max(y, 0) ** 0.8 if y > 0 else 0.002 * y), z))
            rings.append(ring)
        with b.at((0.0, 0, 0.0), (0, 192, 0)):   # its buttons away from the sitter, turned a little as a hand leaves it
            loft(b, rings, "PlasticBeige", first=True, last=True, normal='out', tile=0.2)
            b.box((0, 0.031, -0.03), (0.0012, 0.004, 0.034), "PlasticDark")       # between the buttons
            b.box((0, 0.0375, -0.006), (0.058, 0.004, 0.0012), "PlasticDark", rot=(8, 0, 0))   # and behind them


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


SET_X, TOWER_X, TOWER_Z = -0.11, 0.38, 0.05   # the monitor and keyboard a little left, the tower at their right
TOWER_HIGH, TOWER_DEEP = 0.43, 0.42


def station(b):
    """A whole place on the table at the frame's origin: the monitor and keyboard, the tower
    beside them, the mouse before the tower, and their leads."""
    with b.at((SET_X, 0, 0)):
        back, plug_y = monitor(b)
        keyboard(b)
    with b.at((TOWER_X, 0, TOWER_Z)):
        tower(b)
    with b.at((TOWER_X - 0.01, 0, -0.31)):
        mouse(b)
    behind = TOWER_Z + FRONT + TOWER_DEEP   # the tower's back
    edge = 0.385                            # the table's, behind
    # the picture's lead from the monitor to the tower, and each one's mains lead over the table's edge
    b.tube([(SET_X, plug_y - 0.08, back), (SET_X + 0.03, plug_y - 0.16, back + 0.03), (SET_X + 0.1, 0.012, min(back + 0.02, edge - 0.01)),
            (TOWER_X - 0.16, 0.008, edge - 0.02), (TOWER_X - 0.03, 0.03, behind + 0.04), (TOWER_X - 0.03, 0.14, behind + 0.004)], 0.0045, "PlasticCase",
           segs=8, smooth=5)
    b.box((TOWER_X - 0.03, 0.14, behind + 0.006), (0.04, 0.016, 0.012), "PlasticCase", bevel=0.002, segs=1)   # its plug, with two screws
    for x, y in ((SET_X - 0.1, plug_y - 0.08), (TOWER_X + 0.04, 0.36)):
        b.tube([(x, y, (back if x < 0 else behind)), (x, y - 0.03, edge + 0.01), (x + 0.01, 0.0, edge + 0.02), (x + 0.02, -0.25, edge + 0.025),
                (x + 0.02, -0.5, edge + 0.02)], 0.0035, "Rubber", segs=8, smooth=5)
    # the keyboard's lead round the monitor's foot, and the mouse's round the tower, to the sockets behind it
    b.tube([(SET_X + 0.12, 0.02, -0.24), (SET_X + 0.2, 0.006, -0.2), (0.25, 0.004, -0.05), (0.262, 0.004, behind - 0.05),
            (TOWER_X - 0.07, 0.03, behind + 0.03), (TOWER_X - 0.06, 0.07, behind + 0.004)], 0.0028, "PlasticCase", segs=6, smooth=5)
    b.tube([(TOWER_X - 0.0, 0.014, -0.258), (TOWER_X + 0.01, 0.006, -0.22), (TOWER_X + 0.1, 0.004, -0.15), (TOWER_X + 0.108, 0.004, behind - 0.05),
            (TOWER_X + 0.07, 0.03, behind + 0.03), (TOWER_X + 0.05, 0.09, behind + 0.004)], 0.0022, "PlasticCase", segs=6, smooth=5)
