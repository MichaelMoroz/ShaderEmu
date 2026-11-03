"""Draws the icons no set has a picture for, into linux/apps/icons (64 x 64, which
tools/make_icons.py brings down): glxgears.png, the demo's three gears in its own colours,
nxeyes.png, two eyes looking the same way, halflife.png, an orange lambda in a ring, and jk2.png, a blue
light sword drawn up the diagonal (nothing of Jedi Outcast's own); and the programs and games written for
the machine, each a picture of what it shows (DRAWN below: nxtetris, nxmine, nxpinball, nxsolitaire, nxgame,
nxphys, nxray, nxpath, imdemo, nxide, nes), where the icon sets gave them another game's picture or a
joystick.

    python tools/draw_icons.py [NAME ...]      (names given: only those)
"""
import math, os
from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'linux', 'apps', 'icons')
SIZE, UP = 64, 8   # drawn eight times as large, then brought down


def gear(draw, cx, cy, outer, teeth, turn, colour):
    """A gear's outline of flat-topped teeth, its darker rim, and its hole."""
    inner, points = outer * 0.74, []
    for k in range(teeth):
        a = turn + 2 * math.pi * k / teeth
        step = 2 * math.pi / teeth
        for r, f in ((inner, 0.0), (outer, 0.14), (outer, 0.40), (inner, 0.54)):
            points.append((cx + r * math.cos(a + f * step), cy + r * math.sin(a + f * step)))
    dark = tuple(c * 2 // 5 for c in colour)
    light = tuple(min(255, c + 70) for c in colour)
    draw.polygon(points, fill=colour + (255,), outline=dark + (255,))
    draw.line(points + points[:1], fill=dark + (255,), width=UP * 3 // 2, joint='curve')
    ring = inner * 0.78
    draw.ellipse((cx - ring, cy - ring, cx + ring, cy + ring), fill=light + (255,), outline=dark + (255,), width=UP)
    hole = outer * 0.24
    draw.ellipse((cx - hole, cy - hole, cx + hole, cy + hole), fill=(0, 0, 0, 0), outline=dark + (255,), width=UP)


def gears():
    picture = Image.new('RGBA', (SIZE * UP, SIZE * UP), (0, 0, 0, 0))
    draw = ImageDraw.Draw(picture)
    u = UP
    gear(draw, 23 * u, 38 * u, 21 * u, 10, 0.0, (204, 26, 26))    # the demo's red, green and blue
    gear(draw, 49 * u, 16 * u, 13 * u, 7, 0.35, (20, 160, 46))
    gear(draw, 50 * u, 49 * u, 12 * u, 7, 0.10, (40, 60, 204))
    return picture


def eyes():
    picture = Image.new('RGBA', (SIZE * UP, SIZE * UP), (0, 0, 0, 0))
    draw = ImageDraw.Draw(picture)
    u = UP
    for cx in (17, 47):
        draw.ellipse(((cx - 14) * u, 8 * u, (cx + 14) * u, 56 * u), fill=(255, 255, 255, 255), outline=(0, 0, 0, 255), width=3 * u)
        px, py = cx + 4, 38   # both look down and to the right
        draw.ellipse(((px - 6) * u, (py - 8) * u, (px + 6) * u, (py + 8) * u), fill=(0, 0, 0, 255))
        draw.ellipse(((px - 4) * u, (py - 6) * u, (px - 1) * u, (py - 2) * u), fill=(255, 255, 255, 255))
    return picture


def lambda_ring():
    picture = Image.new('RGBA', (SIZE * UP, SIZE * UP), (0, 0, 0, 0))
    draw = ImageDraw.Draw(picture)
    u, orange = UP / 16, (244, 132, 28, 255)   # (in 1,024ths of the picture)
    draw.ellipse((24 * u, 24 * u, 1000 * u, 1000 * u), fill=orange)
    draw.ellipse((100 * u, 100 * u, 924 * u, 924 * u), fill=(34, 30, 28, 255))

    def stroke(a, b, width):
        draw.line(((a[0] * u, a[1] * u), (b[0] * u, b[1] * u)), fill=orange, width=round(width * u))
        for x, y in (a, b):
            draw.ellipse(((x - width / 2) * u, (y - width / 2) * u, (x + width / 2) * u, (y + width / 2) * u), fill=orange)
    stroke((300, 262), (420, 262), 96)    # the top's hook
    stroke((420, 262), (712, 790), 112)   # the long stroke
    stroke((545, 488), (330, 790), 104)   # the short leg
    draw.line(((712 * u, 790 * u), (790 * u, 790 * u)), fill=orange, width=round(96 * u))   # the foot
    return picture


def light_sword():
    picture = Image.new('RGBA', (SIZE * UP, SIZE * UP), (0, 0, 0, 0))
    draw = ImageDraw.Draw(picture)
    u = UP / 16
    tip, guard, end = (900, 124), (330, 694), (150, 874)   # up the diagonal, the hilt at the lower left
    for width, colour in ((150, (40, 110, 255, 70)), (110, (60, 140, 255, 140)), (70, (120, 190, 255, 255)), (34, (235, 245, 255, 255))):
        draw.line(((tip[0] * u, tip[1] * u), (guard[0] * u, guard[1] * u)), fill=colour, width=round(width * u))
        r = width / 2
        draw.ellipse(((tip[0] - r) * u, (tip[1] - r) * u, (tip[0] + r) * u, (tip[1] + r) * u), fill=colour)
    draw.line(((guard[0] * u, guard[1] * u), (end[0] * u, end[1] * u)), fill=(70, 70, 76, 255), width=round(96 * u))
    for k in (0.2, 0.45, 0.7):   # the grip's rings
        x, y = guard[0] + (end[0] - guard[0]) * k, guard[1] + (end[1] - guard[1]) * k
        draw.line((((x - 34) * u, (y - 34) * u), ((x + 34) * u, (y + 34) * u)), fill=(30, 30, 34, 255), width=round(22 * u))
    draw.line((((guard[0] - 60) * u, (guard[1] - 60) * u), ((guard[0] + 60) * u, (guard[1] + 60) * u)), fill=(170, 170, 180, 255), width=round(40 * u))
    return picture


# The programs and games written for the machine (linux/apps/icons.txt and icons7.txt): each a
# picture of what it shows, in few, large shapes with a dark edge, so that it still reads at 16
# pixels and in the classic look, which draws a pixel or not (an alpha of 128 or more).
INK = (24, 24, 32, 255)


def canvas():
    picture = Image.new('RGBA', (SIZE * UP, SIZE * UP), (0, 0, 0, 0))
    return picture, ImageDraw.Draw(picture)


def box(draw, x0, y0, x1, y1, colour, edge=INK, width=2, bevel=True):
    """A square of the 64-pixel grid with a dark edge and, if bevel, a light top left and a dark
    bottom right (a block, a key, a tile)."""
    u = UP
    draw.rectangle((x0 * u, y0 * u, x1 * u - 1, y1 * u - 1), fill=colour + (255,))
    if bevel:
        light = tuple(min(255, c + 90) for c in colour) + (255,)
        dark = tuple(c * 3 // 5 for c in colour) + (255,)
        b = max(2, (x1 - x0) // 6)
        draw.polygon([(x0 * u, y0 * u), (x1 * u, y0 * u), ((x1 - b) * u, (y0 + b) * u), ((x0 + b) * u, (y0 + b) * u),
                      ((x0 + b) * u, (y1 - b) * u), (x0 * u, y1 * u)], fill=light)
        draw.polygon([(x1 * u, y1 * u), (x0 * u, y1 * u), ((x0 + b) * u, (y1 - b) * u), ((x1 - b) * u, (y1 - b) * u),
                      ((x1 - b) * u, (y0 + b) * u), (x1 * u, y0 * u)], fill=dark)
    if edge:
        draw.rectangle((x0 * u, y0 * u, x1 * u - 1, y1 * u - 1), outline=edge, width=width * u // 2)


def tetris():
    """A T falling toward the gap it fits, over a well's last rows, in blocks of 16 pixels (nxtetris):
    the holes let the pieces' shapes read at 16 pixels, where a full grid was only colours."""
    picture, draw = canvas()
    cells = {(1, 0): 'T', (2, 0): 'T', (3, 0): 'T', (2, 1): 'T',
             (0, 1): 'L', (0, 2): 'L', (0, 3): 'L', (1, 3): 'L',
             (3, 2): 'S', (3, 3): 'S', (2, 3): 'I'}
    colours = {'T': (170, 60, 220), 'I': (30, 200, 230), 'L': (250, 150, 20), 'S': (60, 200, 70)}
    for (x, y), piece in cells.items():
        box(draw, x * 16, y * 16, x * 16 + 16, y * 16 + 16, colours[piece])
    return picture


def mine():
    """A mine on an opened square, and a flag in its corner (nxmine)."""
    picture, draw = canvas()
    u = UP
    box(draw, 0, 0, 64, 64, (192, 192, 192))
    cx, cy, r = 30, 34, 15
    for k in range(4):   # the spikes: across, down and the two diagonals
        a = math.pi * k / 4
        dx, dy = math.cos(a) * (r + 9), math.sin(a) * (r + 9)
        draw.line(((cx - dx) * u, (cy - dy) * u, (cx + dx) * u, (cy + dy) * u), fill=INK, width=5 * u)
    draw.ellipse(((cx - r) * u, (cy - r) * u, (cx + r) * u, (cy + r) * u), fill=INK)
    draw.ellipse(((cx - 9) * u, (cy - 9) * u, (cx - 2) * u, (cy - 2) * u), fill=(255, 255, 255, 255))
    draw.line((52 * u, 6 * u, 52 * u, 26 * u), fill=INK, width=3 * u)   # the flag
    draw.polygon([(51 * u, 5 * u), (51 * u, 17 * u), (38 * u, 11 * u)], fill=(220, 20, 20, 255))
    return picture


def shaded_ball(draw, cx, cy, r, dark, light, shift):
    """A ball lit from the upper left: discs from the rim inward, each lighter and moved toward the light."""
    u = UP
    for k in range(r * u, 0, -u // 2):
        t = k / (r * u)
        f = (1 - t) ** 1.4
        off = (1 - t) * shift * u
        draw.ellipse((cx * u - k - off, cy * u - k - off, cx * u + k - off, cy * u + k - off),
                     fill=tuple(int(d + (l - d) * f) for d, l in zip(dark, light)) + (255,))
    draw.ellipse(((cx - r) * u, (cy - r) * u, (cx + r) * u, (cy + r) * u), outline=INK, width=2 * u)


def pinball():
    """A steel ball over a red flipper, with a pop bumper behind (nxpinball)."""
    picture, draw = canvas()
    u = UP
    draw.ellipse((34 * u, 4 * u, 60 * u, 30 * u), fill=(250, 200, 20, 255), outline=INK, width=2 * u)   # the bumper
    draw.ellipse((41 * u, 11 * u, 53 * u, 23 * u), fill=(230, 40, 40, 255), outline=INK, width=u)
    # the flipper: a rounded bar from its pivot at the lower left, up to the right
    a, b, r0, r1 = (12, 52), (54, 42), 9, 5
    ang = math.atan2(b[1] - a[1], b[0] - a[0])
    nx, ny = -math.sin(ang), math.cos(ang)
    for grow, colour in ((2, INK), (0, (220, 30, 40, 255))):
        hull = [(a[0] + nx * (r0 + grow), a[1] + ny * (r0 + grow)), (b[0] + nx * (r1 + grow), b[1] + ny * (r1 + grow)),
                (b[0] - nx * (r1 + grow), b[1] - ny * (r1 + grow)), (a[0] - nx * (r0 + grow), a[1] - ny * (r0 + grow))]
        draw.polygon([(x * u, y * u) for x, y in hull], fill=colour)
        for (x, y), r in ((a, r0), (b, r1)):
            draw.ellipse(((x - r - grow) * u, (y - r - grow) * u, (x + r + grow) * u, (y + r + grow) * u), fill=colour)
    draw.ellipse(((a[0] - 3) * u, (a[1] - 3) * u, (a[0] + 3) * u, (a[1] + 3) * u), fill=(255, 220, 220, 255))
    shaded_ball(draw, 21, 23, 15, (70, 72, 84), (250, 250, 255), 6)
    return picture


def screen(draw, x0, y0, x1, y1, inside):
    """A small computer's screen on its foot."""
    u = UP
    draw.rectangle((x0 * u, y0 * u, x1 * u, y1 * u), fill=(200, 200, 205, 255), outline=INK, width=2 * u)
    draw.rectangle(((x0 + 4) * u, (y0 + 4) * u, (x1 - 4) * u, (y1 - 4) * u), fill=inside + (255,), outline=INK, width=u)
    m = (x0 + x1) / 2
    draw.rectangle(((m - 3) * u, y1 * u, (m + 3) * u, (y1 + 4) * u), fill=(150, 150, 155, 255), outline=INK, width=u)
    draw.rectangle(((m - 9) * u, (y1 + 4) * u, (m + 9) * u, (y1 + 7) * u), fill=(150, 150, 155, 255), outline=INK, width=u)


def network_game():
    """Two computers joined by a cable, a game on each screen (nxgame: a game with the other computers)."""
    picture, draw = canvas()
    u = UP
    draw.line((15 * u, 39 * u, 15 * u, 58 * u, 49 * u, 58 * u, 49 * u, 57 * u), fill=(250, 200, 20, 255), width=4 * u, joint='curve')
    screen(draw, 1, 4, 29, 28, (40, 120, 220))
    screen(draw, 35, 22, 63, 46, (40, 160, 70))
    draw.polygon([(9 * u, 22 * u), (15 * u, 10 * u), (21 * u, 22 * u)], fill=(250, 220, 40, 255))   # a ship, and a ball in play
    draw.ellipse((45 * u, 29 * u, 53 * u, 37 * u), fill=(255, 255, 255, 255))
    return picture


def physics():
    """Boxes stacked, one tumbling down onto them (nxphys)."""
    picture, draw = canvas()
    for x0, y0, colour in ((4, 44, (210, 140, 60)), (24, 44, (60, 130, 220)), (44, 44, (90, 190, 80)), (14, 25, (220, 70, 60))):
        box(draw, x0, y0, x0 + 18, y0 + 18, colour)
    u = UP
    cx, cy, h, a = 46, 15, 10, math.radians(30)   # the one in the air, turned
    corners = [(cx + h * (dx * math.cos(a) - dy * math.sin(a)), cy + h * (dx * math.sin(a) + dy * math.cos(a)))
               for dx, dy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
    draw.polygon([(x * u, y * u) for x, y in corners], fill=(250, 200, 30, 255), outline=INK, width=2 * u)
    for k in (0, 1):   # its fall's lines
        draw.line(((26 - 3 * k) * u, (4 + 7 * k) * u, (32 - 3 * k) * u, (8 + 7 * k) * u), fill=INK, width=2 * u)
    return picture


def ray_sphere():
    """A shiny sphere over a chequered floor, a ray tracer's first picture (nxray)."""
    picture, draw = canvas()
    u = UP
    for row in range(3):   # the floor in perspective: three rows of squares
        y0, y1 = 40 + row * 8, 48 + row * 8
        w0, w1 = 22 + row * 9, 31 + row * 9
        for k in range(-3, 3):
            x = lambda w, kk: 32 + w * kk / 3
            colour = (235, 235, 235, 255) if (k + row) % 2 else (200, 40, 40, 255)
            draw.polygon([(x(w0, k) * u, y0 * u), (x(w0, k + 1) * u, y0 * u), (x(w1, k + 1) * u, y1 * u), (x(w1, k) * u, y1 * u)], fill=colour)
    draw.ellipse((14 * u, 52 * u, 50 * u, 60 * u), fill=(0, 0, 0, 120))   # its shadow
    shaded_ball(draw, 32, 27, 20, (20, 50, 150), (220, 240, 255), 7)
    draw.line((2 * u, 2 * u, 22 * u, 18 * u), fill=(255, 220, 40, 255), width=2 * u)   # a ray coming in, and its bounce
    draw.line((22 * u, 18 * u, 8 * u, 34 * u), fill=(255, 220, 40, 255), width=2 * u)
    return picture


def teapot():
    """A teapot lit softly, the path tracer's mesh (nxpath)."""
    picture, draw = canvas()
    u = UP
    body = (215, 120, 55)
    draw.polygon([(44 * u, 36 * u), (59 * u, 17 * u), (63 * u, 20 * u), (52 * u, 44 * u)], fill=body + (255,), outline=INK, width=2 * u)   # the spout
    draw.ellipse((1 * u, 24 * u, 21 * u, 48 * u), outline=INK, width=7 * u)   # the handle
    draw.ellipse((3 * u, 26 * u, 19 * u, 46 * u), outline=body + (255,), width=3 * u)
    cx, cy, rx, ry = 32, 40, 21, 16
    for k in range(ry * u, 0, -u // 2):   # the body, lighter to the upper left
        t = k / (ry * u)
        f = (1 - t) ** 1.5
        draw.ellipse((cx * u - k * rx / ry - (1 - t) * 6 * u, cy * u - k - (1 - t) * 5 * u,
                      cx * u + k * rx / ry - (1 - t) * 6 * u, cy * u + k - (1 - t) * 5 * u),
                     fill=(int(body[0] * 0.55 + 110 * f), int(body[1] * 0.55 + 125 * f), int(body[2] * 0.55 + 125 * f), 255))
    draw.ellipse(((cx - rx) * u, (cy - ry) * u, (cx + rx) * u, (cy + ry) * u), outline=INK, width=2 * u)
    draw.rectangle((17 * u, 19 * u, 47 * u, 25 * u), fill=body + (255,), outline=INK, width=2 * u)   # the lid and its knob
    draw.ellipse((27 * u, 11 * u, 37 * u, 21 * u), fill=body + (255,), outline=INK, width=2 * u)
    return picture


def imgui_window():
    """A small window in Dear ImGui's dark blue look: a title, a slider, a check box and a button (imdemo)."""
    picture, draw = canvas()
    u = UP
    draw.rectangle((2 * u, 4 * u, 62 * u, 60 * u), fill=(30, 32, 40, 255), outline=INK, width=2 * u)
    draw.rectangle((3 * u, 5 * u, 61 * u, 15 * u), fill=(41, 74, 122, 255))
    draw.polygon([(6 * u, 7 * u), (13 * u, 7 * u), (9.5 * u, 13 * u)], fill=(230, 230, 230, 255))   # its fold arrow
    draw.rectangle((17 * u, 9 * u, 46 * u, 11 * u), fill=(230, 230, 230, 255))
    draw.rectangle((8 * u, 20 * u, 56 * u, 29 * u), fill=(41, 74, 122, 255))   # a slider
    draw.rectangle((30 * u, 20 * u, 37 * u, 29 * u), fill=(66, 150, 250, 255))
    draw.rectangle((8 * u, 34 * u, 18 * u, 44 * u), fill=(41, 74, 122, 255))   # a check box, ticked
    draw.line((10 * u, 39 * u, 13 * u, 42 * u, 17 * u, 36 * u), fill=(120, 190, 255, 255), width=2 * u)
    draw.rectangle((22 * u, 38 * u, 52 * u, 40 * u), fill=(200, 200, 200, 255))
    draw.rectangle((8 * u, 48 * u, 36 * u, 56 * u), fill=(66, 150, 250, 255))   # a button
    return picture


def card(draw, x0, y0, w, h, turn, face):
    """A playing card turned about its centre; face(draw, to) draws on it, to(x, y) placing a point of
    the card's own frame, 0 to 1 across and down."""
    u = UP
    a = math.radians(turn)
    cx, cy = x0 + w / 2, y0 + h / 2

    def to(x, y):
        dx, dy = (x - 0.5) * w, (y - 0.5) * h
        return ((cx + dx * math.cos(a) - dy * math.sin(a)) * u, (cy + dx * math.sin(a) + dy * math.cos(a)) * u)
    draw.polygon([to(0, 0), to(1, 0), to(1, 1), to(0, 1)], fill=(255, 255, 255, 255), outline=INK, width=2 * u)
    face(draw, to)


def heart_points(at, flip):
    """A heart's outline (the usual curve of sines), point down, or up with flip."""
    out = []
    for k in range(64):
        t = k * 2 * math.pi / 64
        y = -(13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)) / 32
        out.append(at(math.sin(t) ** 3 * 0.5, -y - 0.08 if flip else y + 0.04))
    return out


def solitaire():
    """Two cards fanned: a red heart behind, a black spade in front (nxsolitaire)."""
    picture, draw = canvas()
    red, black = (210, 20, 30, 255), INK

    def heart(d, to):
        d.polygon(heart_points(lambda px, py: to(0.5 + px * 0.75 * 48 / 34 * 0.7, 0.45 + py * 0.75 * 0.7), False), fill=red)

    def spade(d, to):
        at = lambda px, py: to(0.5 + px * 0.8 * 48 / 34 * 0.7, 0.42 + py * 0.8 * 0.7)
        d.polygon(heart_points(at, True), fill=black)
        d.polygon([at(0, 0.1), at(0.2, 0.55), at(-0.2, 0.55)], fill=black)
    card(draw, 3, 5, 34, 48, -14, heart)
    card(draw, 26, 11, 34, 48, 9, spade)
    return picture


def ide():
    """A page of code in colours and the green run button (nxide)."""
    picture, draw = canvas()
    u = UP
    draw.rectangle((2 * u, 4 * u, 54 * u, 60 * u), fill=(250, 250, 250, 255), outline=INK, width=2 * u)
    rows = [(7, 18, (40, 80, 200)), (7, 40, (40, 40, 40)), (13, 30, (200, 60, 40)), (13, 36, (40, 40, 40)),
            (13, 24, (40, 150, 60)), (7, 14, (40, 80, 200))]
    for k, (x, length, colour) in enumerate(rows):
        y = 10 + k * 8
        draw.rectangle((x * u, y * u, (x + length) * u, (y + 4) * u), fill=colour + (255,))
    draw.ellipse((34 * u, 34 * u, 63 * u, 63 * u), fill=(40, 170, 60, 255), outline=INK, width=2 * u)
    draw.polygon([(44 * u, 40 * u), (44 * u, 57 * u), (57 * u, 48.5 * u)], fill=(255, 255, 255, 255))
    return picture


def cartridge():
    """A grey game cartridge with its label (nes: the NES's games)."""
    picture, draw = canvas()
    u = UP
    draw.polygon([(8 * u, 4 * u), (56 * u, 4 * u), (56 * u, 54 * u), (52 * u, 54 * u), (52 * u, 60 * u), (12 * u, 60 * u),
                  (12 * u, 54 * u), (8 * u, 54 * u)], fill=(150, 150, 155, 255), outline=INK, width=2 * u)
    draw.rectangle((14 * u, 10 * u, 50 * u, 40 * u), fill=(230, 60, 50, 255), outline=INK, width=u)
    draw.rectangle((18 * u, 14 * u, 46 * u, 26 * u), fill=(255, 210, 60, 255))
    for x in range(16, 49, 6):   # its grip's ridges
        draw.line((x * u, 45 * u, x * u, 52 * u), fill=(90, 90, 95, 255), width=2 * u)
    return picture


def music():
    """A record and a note on it (nxplay)."""
    picture, draw = canvas()
    u = UP
    draw.ellipse((2 * u, 6 * u, 58 * u, 62 * u), fill=(28, 28, 34, 255), outline=INK, width=2 * u)
    for r in (22, 16):
        draw.ellipse(((30 - r) * u, (34 - r) * u, (30 + r) * u, (34 + r) * u), outline=(70, 70, 80, 255), width=u)
    draw.ellipse((21 * u, 25 * u, 39 * u, 43 * u), fill=(40, 120, 220, 255))
    draw.ellipse((28 * u, 32 * u, 32 * u, 36 * u), fill=(250, 250, 250, 255))
    # the note: two heads joined by a beam, white with a dark edge over the record
    for grow, colour in ((2, INK), (0, (255, 255, 255, 255))):
        for x, y in ((38, 50), (56, 44)):
            draw.ellipse(((x - 6 - grow) * u, (y - 4.5 - grow) * u, (x + 6 + grow) * u, (y + 4.5 + grow) * u), fill=colour)
            draw.rectangle(((x + 3 - grow) * u, (y - 34 - grow) * u, (x + 6 + grow) * u, y * u), fill=colour)
        draw.polygon([((41 - grow) * u, (13 - grow) * u), ((62 + grow) * u, (5 - grow) * u), ((62 + grow) * u, (13 + grow) * u),
                      ((41 - grow) * u, (21 + grow) * u)], fill=colour)
    return picture


def film():
    """A strip of film with the play sign over it (nxvideo)."""
    picture, draw = canvas()
    u = UP
    draw.rectangle((2 * u, 8 * u, 62 * u, 56 * u), fill=(34, 34, 40, 255), outline=INK, width=2 * u)
    for x in range(6, 60, 10):   # the holes along both sides
        for y in (11, 49):
            draw.rectangle((x * u, y * u, (x + 5) * u, (y + 4) * u), fill=(230, 230, 230, 255))
    for x0, colour in ((5, (90, 160, 230)), (33, (230, 150, 60))):   # two frames
        draw.rectangle((x0 * u, 18 * u, (x0 + 26) * u, 46 * u), fill=colour + (255,))
    draw.ellipse((18 * u, 14 * u, 46 * u, 50 * u), fill=(255, 255, 255, 255), outline=INK, width=2 * u)
    draw.polygon([(27 * u, 22 * u), (27 * u, 42 * u), (41 * u, 32 * u)], fill=(220, 40, 40, 255))
    return picture


def framed_picture():
    """A picture in a frame: hills, a sun and the sky (nxview)."""
    picture, draw = canvas()
    u = UP
    draw.rectangle((2 * u, 6 * u, 62 * u, 58 * u), fill=(180, 120, 50, 255), outline=INK, width=2 * u)
    draw.rectangle((8 * u, 12 * u, 56 * u, 52 * u), fill=(120, 190, 250, 255), outline=INK, width=u)
    draw.ellipse((36 * u, 16 * u, 48 * u, 28 * u), fill=(255, 220, 40, 255))
    draw.polygon([(8 * u, 44 * u), (24 * u, 24 * u), (38 * u, 42 * u), (46 * u, 34 * u), (56 * u, 44 * u), (56 * u, 52 * u), (8 * u, 52 * u)],
                 fill=(50, 150, 60, 255))
    draw.rectangle((8 * u, 12 * u, 56 * u, 52 * u), outline=INK, width=u)
    return picture


DRAWN = (('glxgears', gears), ('nxeyes', eyes), ('halflife', lambda_ring), ('jk2', light_sword),
         ('nxtetris', tetris), ('nxmine', mine), ('nxpinball', pinball), ('nxgame', network_game), ('nxphys', physics),
         ('nxray', ray_sphere), ('nxpath', teapot), ('imdemo', imgui_window), ('nxsolitaire', solitaire), ('nxide', ide),
         ('nes', cartridge), ('nxplay', music), ('nxvideo', film), ('nxview', framed_picture))

if __name__ == '__main__':
    import sys
    for name, draw_it in DRAWN:   # (names given: only those)
        if len(sys.argv) > 1 and name not in sys.argv[1:]:
            continue
        draw_it().resize((SIZE, SIZE), Image.LANCZOS).save(os.path.join(OUT, name + '.png'))
        print('linux/apps/icons/%s.png' % name)


