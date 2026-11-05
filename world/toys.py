# Small things of the decade that lie about, modelled here: a cube puzzle, dice, a yo-yo, floppy
# disks, a joystick, a cassette, a mug, a can, a pencil. No plain boxes: every body is a loft of
# its own outline with its edges rounded over (soft()), or turned on a lathe. Each stands on the
# frame's origin, its front towards -z.
import functools
import math

from pc import loft


def loose(make):
    """A thing that falls into place when the part is finished (lib.Builder.loose, world/settle.py)."""
    @functools.wraps(make)
    def thing(b, *args, **more):
        with b.loose():
            return make(b, *args, **more)
    return thing


def offset(outline, d):
    """A convex outline (anticlockwise, in x and y) moved in by d along each corner's bisector."""
    n, out = len(outline), []
    for k in range(n):
        (ax, ay), (px, py), (bx, by) = outline[k - 1], outline[k], outline[(k + 1) % n]
        n1, n2 = (py - ay, -(px - ax)), (by - py, -(bx - px))   # outward normals of the two sides
        l1, l2 = math.hypot(*n1) or 1, math.hypot(*n2) or 1
        nx, ny = n1[0] / l1 + n2[0] / l2, n1[1] / l1 + n2[1] / l2
        ln = math.hypot(nx, ny) or 1
        cos_half = max(0.3, (n1[0] * n2[0] + n1[1] * n2[1]) / (l1 * l2) * 0.5 + 0.5) ** 0.5
        out.append((px - nx / ln * d / cos_half, py - ny / ln * d / cos_half))
    return out


def rounded(w, h, r, n=4):
    """A rounded rectangle's outline (pc.rrect's), with fewer points."""
    from pc import rrect
    return rrect(w, h, r, n=n)


def soft(b, outline, high, edge, mat, at=(0, 0, 0), turn=0.0, under=None, steps=3, tile=0.1):
    """A solid standing on the frame's floor: `outline` (x, and z towards the sitter as -y) raised
    `high`, its top edge rounded over by a quarter circle of `edge` (and its foot by `under`, or as much).
    tile: metres its material's picture repeats (pc.loft's)."""
    under = edge if under is None else under
    rings = []
    for k in range(steps + 1):   # the foot, rounding out
        a = math.pi / 2 * k / steps
        rings.append((under * (1 - math.sin(a)), under * (1 - math.cos(a))))
    for k in range(steps + 1):   # the top, rounding in
        a = math.pi / 2 * k / steps
        rings.append((edge * (1 - math.cos(a)), high - edge + edge * math.sin(a)))
    with b.at(at, (0, turn, 0)):
        with b.at((0, 0, 0), (-90, 0, 0)):   # outlines in the floor's plane, stacked upwards
            loft(b, [[(x, y, z) for x, y in offset(outline, inset)] for inset, z in rings], mat, first=True, last=True, tile=tile)


# ---- the cube puzzle
STICKERS = ("Ceramic", "GamepadYellow", "GamepadRed", "ToyOrange", "GamepadBlue", "ToyGreen")


@loose
def puzzle_cube(b, at, turn=0.0, side=0.057, twist=0.0):
    """The cube puzzle of 1980, 57 mm: 27 black pieces rounded at every edge, a tile with rounded
    corners on each outer face, a little mixed; its top layer turned `twist` degrees."""
    piece = side / 3
    tile = rounded(piece * 0.84, piece * 0.84, piece * 0.12)
    with b.at(at, (0, turn, 0)):
        for j in range(3):
            with b.at((0, 0, 0), (0, twist if j == 2 else 0.0, 0)):
                for i in range(3):
                    for k in range(3):
                        x, y, z = (i - 1) * piece, j * piece, (k - 1) * piece
                        soft(b, rounded(piece * 0.985, piece * 0.985, piece * 0.1), piece * 0.985, piece * 0.08, "PlasticDark", at=(x, y, z))
                        mid = (x, y + piece / 2, z)
                        faces = []
                        if j == 2:
                            faces.append(((x, y + piece * 0.985, z), (0, 0, 0), 0))
                        for cond, dx, dz, rot, face in ((i == 0, -1, 0, (0, 0, 90), 1), (i == 2, 1, 0, (0, 0, -90), 2),
                                                         (k == 0, 0, -1, (-90, 0, 0), 3), (k == 2, 0, 1, (90, 0, 0), 4)):
                            if cond:
                                faces.append(((mid[0] + dx * piece * 0.4925, mid[1], mid[2] + dz * piece * 0.4925), rot, face))
                        for centre, rot, face in faces:
                            colour = STICKERS[(face + (2 if (i * 7 + j * 5 + k * 3) % 5 == 0 else 0)) % 6]
                            with b.at(centre, rot):
                                soft(b, tile, 0.0007, 0.0003, colour, steps=1)


# ---- dice
PIPS = {1: [(0, 0)], 2: [(-1, -1), (1, 1)], 3: [(-1, -1), (0, 0), (1, 1)], 4: [(-1, -1), (-1, 1), (1, -1), (1, 1)],
        5: [(-1, -1), (-1, 1), (0, 0), (1, -1), (1, 1)], 6: [(-1, -1), (-1, 0), (-1, 1), (1, -1), (1, 0), (1, 1)]}


@loose
def die(b, at, turn=0.0, side=0.016, mat="Ceramic", pip="Rubber"):
    """A die with well rounded edges, its pips small dished hollows filled dark: 1 on top."""
    with b.at(at, (0, turn, 0)):
        soft(b, rounded(side, side, side * 0.22), side, side * 0.22, mat, steps=4)
        step, r = side * 0.26, side * 0.08
        for face, (rot, centre) in {1: ((0, 0, 0), (0, side, 0)), 6: ((180, 0, 0), (0, 0, 0)), 2: ((-90, 0, 0), (0, side / 2, -side / 2)),
                                    5: ((90, 0, 0), (0, side / 2, side / 2)), 3: ((0, 0, 90), (-side / 2, side / 2, 0)), 4: ((0, 0, -90), (side / 2, side / 2, 0))}.items():
            with b.at(centre, rot):
                for u, v in PIPS[face]:
                    # (its paint a disc standing 0.4 mm out of the face: a cone sunk 0.3 mm into it showed only its rim)
                    b.lathe((u * step, -0.0001, v * step), [(r, 0), (r, 0.0004), (r * 0.8, 0.0005), (0, 0.0005)], pip, segs=12)


# ---- a yo-yo
@loose
def yoyo(b, at, turn=0.0, mat="GamepadRed"):
    """A yo-yo lying on its side, its string wound and a loop of it trailing."""
    with b.at(at, (0, turn, 0)):
        b.lathe((0, 0, 0), [(0, 0), (0.024, 0.0), (0.0275, 0.002), (0.0285, 0.006), (0.028, 0.012), (0.012, 0.014), (0.006, 0.0145), (0.006, 0.0175),
                            (0.012, 0.018), (0.028, 0.02), (0.0285, 0.026), (0.0275, 0.03), (0.024, 0.032), (0.008, 0.0325), (0, 0.032)], mat, segs=32)
        b.lathe((0, 0.0142, 0), [(0.0062, 0), (0.0105, 0.0004), (0.011, 0.0018), (0.0105, 0.0032), (0.0062, 0.0036)], "Paper", segs=24, cap=False)
        b.tube([(0.011, 0.016, 0), (0.035, 0.002, 0.012), (0.07, 0.0012, -0.004), (0.092, 0.0012, 0.018), (0.08, 0.0012, 0.04), (0.06, 0.0012, 0.03)],
               0.0007, "Paper", segs=4, smooth=4)


# ---- floppy disks
# A 3.5 inch disk after ECMA-147 (90 mm flexible disk cartridges, 1990: https://ecma-international.org/wp-content/uploads/
# ECMA-147_1st_edition_december_1990.pdf, its 6.1 and
# figs. 7 to 10), in mm in its own plan: x across its shutter edge from the side of the primary locating hole
# (Reference Axis Y is 5 mm in from that side: the two locating holes 80 mm apart, L5), zz in from the shutter
# edge (Reference Axis X 17.5 mm in, L29). The case 90.0 x 94.0 x 3.3 (L2, L1, E1), three corners 2 mm round
# (R1), the fourth cut at 45 degrees (omega) from 80 mm along Y's edge (L22); the hub's centre 40 mm along
# (L3) and 31 mm in from X (L4), its access hole 26.5 across (D1) on side 0, the steel hub 25 across (D5); the
# shutter's recess from 3.5 to 64.5 mm along (L28, L25); the write-inhibit hole on Y, 67.75 mm in from X (L19),
# the HD identification hole at 80 mm along and 62.25 in (L5, L51), each 4.0 across and 3.5 deep (L21, L20);
# the label area of side 1 from 3.5 to 76.5 mm along Y (L11, L12) and from 20 mm past X (L13) to the back.
# The shutter is 12 mm's window (L33) over the head window when open, its leading edge 53.75 mm from Y (L32),
# and it slides within its recess: so it is 50 mm long and slides 10.5 mm, from which its place shut follows.
# Guesses: the recesses' 0.4 mm depth, the shutter's 0.2 mm steel, how far in from the edge the shutter and
# its recess reach (31.5 and 32 mm: past the head window's 29, L15 + L16 from X), the label's paper; the
# optional handling notches and the profile's small steps (L23 to L31) are not modelled.
FLOPPY = (0.09, 0.0033, 0.094)


def floppy_piece(b, xs, zs, y0, y1, mat, round_at=(), cut=None):
    """A block of the disk: x in xs and zz in zs (mm, its plan), y0 to y1 mm up; round_at: its corners
    (x, zz) that are the case's R1 = 2 mm; cut: the corner cut at 45 degrees, its legs' length."""
    x0, x1 = xs
    z0, z1 = zs
    corners = [(x0, z0), (x1, z0), (x1, z1), (x0, z1)]
    points = []
    for i, c in enumerate(corners):
        before, after = corners[i - 1], corners[(i + 1) % 4]
        din = ((c[0] - before[0]) / (abs(c[0] - before[0]) or 1), (c[1] - before[1]) / (abs(c[1] - before[1]) or 1))
        dout = ((after[0] - c[0]) / (abs(after[0] - c[0]) or 1), (after[1] - c[1]) / (abs(after[1] - c[1]) or 1))
        if c in round_at:   # a quarter round of 2 mm, from the side coming in to the side going out
            centre = (c[0] - din[0] * 2.0 + dout[0] * 2.0, c[1] - din[1] * 2.0 + dout[1] * 2.0)
            for k in range(5):
                t = math.pi / 2 * k / 4
                points.append((centre[0] - dout[0] * 2.0 * math.cos(t) + din[0] * 2.0 * math.sin(t),
                               centre[1] - dout[1] * 2.0 * math.cos(t) + din[1] * 2.0 * math.sin(t)))
        elif cut and c == cut[0]:
            points += [(c[0] - din[0] * cut[1], c[1] - din[1] * cut[1]), (c[0] + dout[0] * cut[1], c[1] + dout[1] * cut[1])]
        else:
            points.append(c)
    flat = [(x / 1000 - 0.045, z / 1000 - 0.047) for x, z in points]
    b.prism((0, (y0 + y1) / 2000, 0), flat, (y1 - y0) / 1000, mat, plane='xz')


@loose
def floppy(b, at, turn=0.0, label="Paper", body="FloppyShell"):
    """A 3.5 inch disk lying side 1 up (the label's, the hub under it), its shutter edge towards -z,
    after ECMA-147 (see FLOPPY): the case in blocks round its recesses and holes, the steel shutter
    shut over the head windows, its label, the hub."""
    with b.at(at, (0, turn, 0)):
        # the case: its full thickness where nothing is let into it; 0.4 mm a side less in the recesses (0.3, check_zfight's
        # depth, had the hidden core's underside counted with the skin's)
        for zs in ((0, 83.5), (83.5, 87), (87, 94)):   # the side of Y, round the write-inhibit hole
            for xs in (((0, 8.5),) if zs != (83.5, 87) else ((0, 3), (7, 8.5))):
                floppy_piece(b, xs, zs, 0, 3.3, body, round_at=[c for c in ((0, 0), (0, 94)) if c[0] == xs[0] and c[1] in zs])
        floppy_piece(b, (69.5, 90), (0, 37.5), 0, 3.3, body, cut=((90, 0), 5.0))   # the far end of the shutter's edge, its corner cut
        for zs in ((37.5, 77.75), (77.75, 81.75), (81.75, 94)):   # the far side, round the identification hole
            for xs in (((81.5, 90),) if zs != (77.75, 81.75) else ((81.5, 83), (87, 90))):
                floppy_piece(b, xs, zs, 0, 3.3, body, round_at=[(90, 94)] if zs == (81.75, 94) else ())
        floppy_piece(b, (8.5, 69.5), (32, 37.5), 2.9, 3.3, body)          # side 1 between the shutter's recess and the label's
        floppy_piece(b, (8.5, 69.5), (0, 37.5), 0.4, 2.9, body)           # the middle, under both recesses
        floppy_piece(b, (69.5, 81.5), (37.5, 94), 0.4, 2.9, body)
        floppy_piece(b, (8.5, 69.5), (37.5, 94), 0.4, 2.9, body)
        # side 0 round the hub's access hole (26.5 mm across), in two halves
        for side in (-1, 1):
            arc = [(45 + side * 13.25 * math.sin(math.pi * k / 12), 48.5 - 13.25 * math.cos(math.pi * k / 12)) for k in range(13)]
            outer = [(45, 61.75), (45, 94), (8.5, 94), (8.5, 32), (45, 32)] if side < 0 else [(45, 61.75), (45, 94), (81.5, 94), (81.5, 37.5), (69.5, 37.5), (69.5, 32), (45, 32)]
            flat = [(x / 1000 - 0.045, z / 1000 - 0.047) for x, z in ([(45, 32), (45, 35.25)] + arc[1:-1] + outer[:-1])]
            b.prism((0, 0.0002, 0), flat, 0.0004, body, plane='xz')
        b.lathe((0.0, 0.0001, 48.5 / 1000 - 0.047), [(0, 0), (0.0125, 0), (0.0125, 0.0003), (0, 0.0003)], "MetalSteel", segs=32)   # the hub
        # the shutter, shut: 50 mm of 0.2 mm steel folded over the edge, its 12 mm windows 10.5 mm past the head windows
        for y0 in (2.9, 0.2):
            for xs, zs in (((19.25, 49.75), (0, 31.5)), ((61.75, 69.25), (0, 31.5)), ((49.75, 61.75), (0, 5)), ((49.75, 61.75), (29.2, 31.5))):
                floppy_piece(b, xs, zs, y0, y0 + 0.2, "FloppyShutter")
        floppy_piece(b, (19.25, 69.25), (-0.2, 0), 0.2, 3.1, "FloppyShutter")
        floppy_piece(b, (9, 81), (38, 93.5), 2.9, 3.15, label)          # the label in its recess


def floppies(b, at, turn, count, rng, labels=("Paper", "LabelYellow", "Paper", "LabelRed", "Paper", "LabelBlue")):
    """A small heap of disks, each a little off the one under it. Gives the heap's top."""
    x, y, z = at
    for i in range(count):
        floppy(b, (x + rng.uniform(-0.006, 0.006), y, z + rng.uniform(-0.006, 0.006)), turn + rng.uniform(-14, 14), labels[i % len(labels)])
        y += FLOPPY[1] + 0.0005   # (flat now: its shutter and label lie in their recesses)
    return y


# ---- a joystick
@loose
def joystick(b, at, turn=0.0):
    """An analogue joystick of the decade: a domed base on a wide foot with two trim wheels and a
    button, a rubber boot, a moulded grip leaning a little with a trigger and a thumb button, its
    lead leaving the back."""
    with b.at(at, (0, turn, 0)):
        soft(b, rounded(0.15, 0.13, 0.03, n=6), 0.016, 0.006, "PlasticDark", under=0.002)
        soft(b, rounded(0.12, 0.10, 0.03, n=6), 0.03, 0.012, "PlasticDark", at=(0, 0.0, 0.006))
        for x in (-0.062, 0.062):   # the trim wheels, in the foot's front corners
            b.lathe((x, 0.012, -0.045), [(0, -0.003), (0.009, -0.003), (0.0095, -0.0015), (0.0095, 0.0015), (0.009, 0.003), (0, 0.003)],
                    "PlasticGrey", segs=18, rot=(0, 0, 90))
        b.lathe((0.04, 0.0295, 0.035), [(0, 0.004), (0.006, 0.0038), (0.0075, 0.002), (0.0078, 0)], "GamepadRed", segs=16, cap=False)
        b.lathe((0, 0.029, 0.006), [(0.024, 0), (0.023, 0.004), (0.017, 0.009), (0.012, 0.012), (0.009, 0.0135), (0.007, 0.014)], "Rubber", segs=20, cap=False)
        with b.at((0, 0.04, 0.006), (-7, 0, 4)):
            b.lathe((0, -0.002, 0), [(0.0055, 0), (0.0055, 0.03), (0.0035, 0.032)], "MetalSteel", segs=12, cap=False)
            b.lathe((0, 0.028, 0), [(0, 0), (0.013, 0), (0.016, 0.006), (0.0175, 0.025), (0.018, 0.055), (0.0165, 0.08), (0.0135, 0.094), (0.008, 0.1),
                                    (0, 0.1015)], "PlasticDark", segs=20)
            b.lathe((0, 0.105, -0.015), [(0, 0), (0.007, 0), (0.0068, 0.003), (0.004, 0.006), (0, 0.0062)], "GamepadRed", segs=10, rot=(-90, 0, 0))
            b.lathe((0, 0.13, -0.0005), [(0, 0.004), (0.006, 0.0035), (0.0068, 0.0015), (0.007, 0)], "GamepadRed", segs=14, cap=False, rot=(-20, 0, 0))
        b.tube([(0, 0.012, 0.07), (0, 0.006, 0.09), (0.03, 0.003, 0.14), (0.02, 0.003, 0.2), (-0.03, 0.003, 0.26)], 0.0025, "Rubber", segs=6, smooth=4)


# ---- a compact cassette
def cassette_outline():
    """A cassette's plan: 100.4 x 63.8 mm (Philips' compact cassette, IEC 60094-7; its figures are not
    public, these are the ones every account of it quotes, its hubs 42.5 mm apart), rounded corners,
    the head area's corners cut towards the near side (their 12 and 8 mm, a guess from photographs)."""
    w, d, r = 0.1004, 0.0638, 0.003
    pts = []
    for cx, cy, a0 in ((w / 2 - r, d / 2 - r, 0), (-w / 2 + r, d / 2 - r, 90)):
        pts += [(cx + r * math.cos(math.radians(a0 + 30 * k)), cy + r * math.sin(math.radians(a0 + 30 * k))) for k in range(4)]
    pts += [(-w / 2, -d / 2 + 0.008), (-w / 2 + 0.012, -d / 2), (w / 2 - 0.012, -d / 2), (w / 2, -d / 2 + 0.008)]
    return pts


@loose
def cassette(b, at, turn=0.0, label="Paper"):
    """A compact cassette lying face up: its two shells, the label, the window over the reels and
    their hubs, five screws."""
    with b.at(at, (0, turn, 0)):
        soft(b, cassette_outline(), 0.0062, 0.0008, "PlasticGrey", steps=2)   # (12 mm in all: two shells of 6)
        soft(b, cassette_outline(), 0.0062, 0.0008, "PlasticDark", at=(0, 0.006, 0), steps=2, under=0.0002)
        # (the label half a millimetre over the shell, the window as much over the label: 0.2 mm flickered)
        soft(b, rounded(0.088, 0.04, 0.003), 0.0006, 0.00015, label, at=(0, 0.0121, -0.006), steps=1)
        soft(b, rounded(0.05, 0.013, 0.0055), 0.0006, 0.0001, "Glass", at=(0, 0.0126, -0.006), steps=1)
        for x in (-0.02125, 0.02125):   # the hubs, 42.5 mm apart
            b.lathe((x, 0.0132, -0.006), [(0.0052, 0), (0.004, 0.0002), (0.0022, 0.0003), (0, 0.0003)], "Ceramic", segs=12, cap=False)
        for x, z in ((-0.044, -0.026), (0.044, -0.026), (-0.044, 0.026), (0.044, 0.026), (0, 0.025)):
            b.lathe((x, 0.0121, z), [(0.0016, 0), (0.0012, 0.0003), (0, 0.00035)], "MetalSteel", segs=8, cap=False)


# ---- things turned on a lathe
@loose
def mug(b, at, mat="Ceramic", turn=0.0):
    with b.at(at, (0, turn, 0)):
        b.lathe((0, 0, 0), [(0, 0.004), (0.036, 0.004), (0.04, 0), (0.041, 0.095), (0.037, 0.095), (0.036, 0.008), (0, 0.008)], mat, segs=24)
        b.tube([(0.04, 0.075, 0), (0.062, 0.07, 0), (0.066, 0.045, 0), (0.058, 0.022, 0), (0.04, 0.02, 0)], 0.006, mat, segs=8, smooth=4)


@loose
def soda_can(b, at, kind="coke", lying=False, turn=0.0):
    """A 12 oz can of the decade's cola (world/can.py: its outline from the makers' sheets, its print
    the 1990s US cans', world/textures.py's soda_cans), standing or lying on its side: one solid of
    aluminium, printed on its body, and on its lid the stay-on tab on its rivet (a 202 end's tab:
    about 20 by 13 mm, a guess from photographs; the score of the opening is not modelled)."""
    import can
    mat = {"coke": "CanCoke", "diet": "CanDiet"}[kind]
    with b.at(at, (0, turn, 0)):
        with b.at((0, 0.033, -0.06) if lying else (0, 0, 0), (90, 0, 0) if lying else (0, 0, 0)):
            b.lathe((0, 0, 0), can.PROFILE, mat, segs=48, uvrect=(0, 0, 1, 1), print_y=can.PRINT)
            top = can.PROFILE[-1][1]
            b.cyl((0, top + 0.0003, 0.004), 0.0022, 0.0006, "CanMetal", segs=12)                    # the rivet, up to the tab
            b.cyl((0, top + 0.00125, 0.004), 0.0018, 0.0004, "CanMetal", segs=12)                   # and its head over it (a guess from photographs;
            # its shank's top 0.3 mm under the tab's lay in the tab's plane: world/check_zfight.py)
            soft(b, rounded(0.013, 0.020, 0.005), 0.0005, 0.0002, "CanMetal", at=(0, top + 0.0006, 0.002))   # the tab, its ring towards +z


@loose
def pencil(b, at, turn=0.0, mat="GamepadYellow"):
    """A pencil lying on a table: six-sided, sharpened, a brass ferrule and an eraser."""
    with b.at(at, (0, turn, 0)):
        b.lathe((0, 0.0037, 0), [(0, -0.095), (0.0030, -0.094), (0.0032, -0.09), (0.0032, -0.085), (0.0039, -0.084), (0.0039, -0.074),   # (under the eraser's sleeve by 0.4 mm)
                                 (0.0037, -0.073), (0.0037, 0.065), (0.0012, 0.084), (0.0006, 0.087), (0, 0.088)], mat, segs=6, rot=(0, 0, -90))
        b.lathe((0, 0.0037, 0), [(0.00372, -0.093), (0.00372, -0.0855)], "RedPaint", segs=6, rot=(0, 0, -90), cap=False)
        b.lathe((0, 0.0037, 0), [(0.004, -0.0845), (0.004, -0.0735)], "Brass", segs=12, rot=(0, 0, -90), cap=False)
        b.lathe((0, 0.0037, 0), [(0.0038, 0.0652), (0.0013, 0.0838)], "WoodOak", segs=12, rot=(0, 0, -90), cap=False)


@loose
def ball(b, at, radius, mat):
    """A plain ball (a bouncy ball, a marble) resting on the frame's floor."""
    with b.at(at):
        b.lathe((0, 0, 0), [(radius * math.sin(math.pi * k / 14), radius - radius * math.cos(math.pi * k / 14)) for k in range(15)], mat, segs=24)
