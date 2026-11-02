"""Draws the icons no set has a picture for, into linux/apps/icons (64 x 64, which
tools/make_icons.py brings down): glxgears.png, the demo's three gears in its own colours,
nxeyes.png, two eyes looking the same way, halflife.png, an orange lambda in a ring, jk2.png, a blue
light sword drawn up the diagonal (nothing of Jedi Outcast's own), and opencnc3d.png, three green crystals on a tilted tile of
ground (nothing of OpenCNC3D's or the cartridge's).

    python tools/draw_icons.py
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


def crystals():
    picture = Image.new('RGBA', (SIZE * UP, SIZE * UP), (0, 0, 0, 0))
    draw = ImageDraw.Draw(picture)
    u = UP / 16
    ground = [(512, 560), (980, 790), (512, 1010), (44, 790)]   # a tile of ground seen tilted
    draw.polygon([(x * u, y * u) for x, y in ground], fill=(120, 96, 58, 255), outline=(62, 48, 28, 255))
    draw.line([(x * u, y * u) for x, y in ground + ground[:1]], fill=(62, 48, 28, 255), width=round(28 * u))
    for cx, base, top, half in ((330, 820, 330, 92), (560, 860, 130, 118), (760, 790, 420, 84)):   # three crystals
        left, right, peak = (cx - half, base), (cx + half, base), (cx, top)
        mid = (cx + half * 0.15, base - (base - top) * 0.25)
        draw.polygon([(p[0] * u, p[1] * u) for p in (left, peak, mid)], fill=(70, 200, 60, 255))
        draw.polygon([(p[0] * u, p[1] * u) for p in (mid, peak, right)], fill=(28, 120, 30, 255))
        draw.line([(p[0] * u, p[1] * u) for p in (left, peak, right, mid, left)], fill=(12, 60, 14, 255), width=round(20 * u), joint='curve')
    return picture


for name, picture in (('glxgears', gears()), ('nxeyes', eyes()), ('halflife', lambda_ring()), ('jk2', light_sword()), ('opencnc3d', crystals())):
    picture.resize((SIZE, SIZE), Image.LANCZOS).save(os.path.join(OUT, name + '.png'))
    print('linux/apps/icons/%s.png' % name)
