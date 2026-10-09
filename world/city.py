# The city outside the window, seen from 42 m up: unlit boxes whose vertex colour shades a face
# and whose alpha is the haze (City.shader). Only faces the room can see are made.
import math
import random

from room import HALF_W, WINDOW_Y, WINDOW_Z

EYE = (HALF_W + 0.2, WINDOW_Y, WINDOW_Z)
GROUND = WINDOW_Y - 42.0
BLOCK, ROWS, ACROSS = 16.0, 58, 44
BAY, STOREY, CELLS = 2.6, 3.4, 16
RIVER = (9, 12)                       # rows of blocks that are water
WATER = (0.012, 0.018, 0.030)
WATER_Y = GROUND - 1.5


class Batch:
    def __init__(self):
        self.vertices, self.faces, self.uvs, self.colours, self.normals = [], [], [], [], []

    def quad(self, corners, uv, colour, normal):
        """`colour` is one for the face or one a corner."""
        first = len(self.vertices)
        self.vertices.extend(corners)
        self.faces.append((first, first + 1, first + 2, first + 3))
        self.uvs.append(uv)
        each = colour if isinstance(colour, list) else [colour] * 4
        shaded = []
        for (x, y, z), c in zip(corners, each):
            haze = 1.0 - math.exp(-math.dist((x, EYE[1], z), EYE) / 480.0)   # rain: the far towers are lost in it
            shaded.append((c[0], c[1], c[2], haze))
        self.colours.append(shaded)
        self.normals.append(normal)

    def flat(self, x0, x1, z0, z1, y, colour):
        """A patch of ground; `colour` may be four, for (x0,z0), (x1,z0), (x1,z1), (x0,z1)."""
        self.quad([(x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1)], [(0.5, 0.5)] * 4, colour, (0, 1, 0))


def box(batch, x0, x1, y0, y1, z0, z1, colour, uv=None, roof=None, every=False):
    """The faces of a box that the window can see. `uv` gives a wall's (u0, v0, u1, v1) from its width."""
    def wall(a, b, normal, shade):
        wide = math.dist(a, b)
        u0, v0, u1, v1 = uv(wide, y1 - y0) if uv else (0.5, 0.5, 0.5, 0.5)
        corners = [(a[0], y0, a[1]), (b[0], y0, b[1]), (b[0], y1, b[1]), (a[0], y1, a[1])]
        batch.quad(corners, [(u0, v0), (u1, v0), (u1, v1), (u0, v1)], tuple(c * shade for c in colour), normal)
    wall((x0, z0), (x0, z1), (-1, 0, 0), 1.0)
    if z1 < EYE[2] + 12 or every:
        wall((x0, z1), (x1, z1), (0, 0, 1), 0.62)
    if z0 > EYE[2] - 12 or every:
        wall((x1, z0), (x0, z0), (0, 0, -1), 0.62)
    if every:
        wall((x1, z1), (x1, z0), (1, 0, 0), 0.5)
    if y1 < EYE[1] + 6 or every:
        (roof or batch).quad([(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)], [(0.5, 0.5)] * 4,
                             (0.045, 0.047, 0.055) if roof else tuple(c * 0.8 for c in colour), (0, 1, 0))


def build(b):
    rng = random.Random(11)
    walls = {"CityOffice": Batch(), "CityFlats": Batch(), "CityGlass": Batch()}
    glow, streets = Batch(), Batch()
    warm, cool = (1.0, 0.68, 0.34), (0.62, 0.82, 1.0)

    def window_uv(wide, high):
        bays, floors = max(1, round(wide / BAY)), max(1, round(high / STOREY))
        u0, v0 = rng.randrange(CELLS) / CELLS, rng.randrange(CELLS) / CELLS
        return u0, v0, u0 + bays / CELLS, v0 + floors / CELLS

    def tower(cx, cz, half_x, half_z, y0, y1, mat, tint):
        box(walls[mat], cx - half_x, cx + half_x, y0, y1, cz - half_z, cz + half_z, tint, window_uv, roof=glow)

    def shopfronts(cx, cz, half_x, half_z):
        """Lit ground floors on the side that faces the window."""
        count = rng.randint(2, 5)
        wide = 2 * half_z / count
        for i in range(count):
            if rng.random() < 0.35:
                continue
            base = rng.choice((warm, warm, cool, (1.0, 0.35, 0.3), (0.4, 1.0, 0.6)))
            colour = tuple(c * rng.uniform(0.35, 0.9) for c in base)
            z0 = cz - half_z + i * wide + 0.3
            glow.quad([(cx - half_x - 0.15, GROUND + 0.4, z0), (cx - half_x - 0.15, GROUND + 0.4, z0 + wide - 0.6),
                       (cx - half_x - 0.15, GROUND + 3.4, z0 + wide - 0.6), (cx - half_x - 0.15, GROUND + 3.4, z0)],
                      [(0.5, 0.5)] * 4, colour, (-1, 0, 0))
            # and its light on the pavement
            streets_light = [tuple(c * 0.45 for c in colour)] * 2 + [(0.03, 0.03, 0.03)] * 2
            glow.quad([(cx - half_x - 0.15, GROUND + 0.05, z0), (cx - half_x - 0.15, GROUND + 0.05, z0 + wide - 0.6),
                       (cx - half_x - 2.6, GROUND + 0.05, z0 + wide - 0.6), (cx - half_x - 2.6, GROUND + 0.05, z0)],
                      [(0.5, 0.5)] * 4, streets_light, (0, 1, 0))

    def clutter(cx, cz, half, top):
        dark = (0.07, 0.07, 0.08)
        for _ in range(rng.randint(1, 3)):
            w, d, h = rng.uniform(1.2, 3.0), rng.uniform(1.2, 3.0), rng.uniform(1.0, 2.6)
            x, z = cx + rng.uniform(-half + 1.5, half - 1.5), cz + rng.uniform(-half + 1.5, half - 1.5)
            box(glow, x - w / 2, x + w / 2, top, top + h, z - d / 2, z + d / 2, dark)
        for dx, dz, wx, wz in ((0, -half, half, 0.15), (0, half, half, 0.15), (-half, 0, 0.15, half), (half, 0, 0.15, half)):
            box(glow, cx + dx - wx, cx + dx + wx, top, top + 0.7, cz + dz - wz, cz + dz + wz, dark)
        if rng.random() < 0.4:   # a lamp over the roof's door, and its pool of light
            x, z = cx + rng.uniform(-half + 2, half - 2), cz + rng.uniform(-half + 2, half - 2)
            pool = [(0.30, 0.22, 0.12)] * 4
            glow.flat(x - 1.6, x + 1.6, z - 1.6, z + 1.6, top + 0.06, pool)
            box(glow, x - 0.15, x + 0.15, top + 2.0, top + 2.3, z - 0.15, z + 0.15, (1.0, 0.8, 0.5), every=True)

    def mast(cx, cz, top, high):
        box(glow, cx - 0.2, cx + 0.2, top, top + high, cz - 0.2, cz + 0.2, (0.10, 0.10, 0.11), every=True)
        box(glow, cx - 0.5, cx + 0.5, top + high, top + high + 1.0, cz - 0.5, cz + 0.5, (1.0, 0.10, 0.06), every=True)

    def crown(cx, cz, half_x, half_z, top):
        colour = tuple(c * rng.uniform(0.6, 1.0) for c in rng.choice((warm, cool, (1.0, 0.9, 0.8))))
        box(glow, cx - half_x - 0.2, cx + half_x + 0.2, top - 1.4, top - 0.2, cz - half_z - 0.2, cz + half_z + 0.2, colour)

    def sign(cx, cz, half, top):
        colour = rng.choice(((0.15, 0.9, 1.0), (1.0, 0.2, 0.7), (1.0, 0.8, 0.15), (0.3, 1.0, 0.4), (1.0, 0.3, 0.15)))
        w, h = min(half * 1.6, rng.uniform(5, 9)), rng.uniform(2.2, 4.0)
        x = cx - half + 0.4
        box(glow, x - 0.15, x, top + 1.2, top + 1.6 + h, cz - w / 2 - 0.3, cz + w / 2 + 0.3, (0.05, 0.05, 0.06))
        for k in range(3):   # bands of light: a sign too far off to read
            y = top + 1.5 + k * h / 3
            box(glow, x - 0.2, x - 0.15, y, y + h / 3 - 0.35, cz - w / 2 + rng.uniform(0, w * 0.2), cz + w / 2 - rng.uniform(0, w * 0.3),
                tuple(c * rng.uniform(0.6, 1.0) for c in colour))

    river_x0 = EYE[0] + 2.0 + RIVER[0] * BLOCK
    river_x1 = EYE[0] + 2.0 + RIVER[1] * BLOCK
    downtown = (EYE[0] + 330.0, EYE[2] + 40.0)
    for r in range(ROWS):
        for a in range(-ACROSS, ACROSS):
            cx, cz = EYE[0] + (r + 0.5) * BLOCK + 2.0, EYE[2] + (a + 0.5) * BLOCK
            seed, kind_roll = rng.random(), rng.random()
            if RIVER[0] <= r < RIVER[1]:
                glow.flat(cx - BLOCK / 2, cx + BLOCK / 2, cz - BLOCK / 2, cz + BLOCK / 2, WATER_Y, WATER)
                continue
            lot = BLOCK * 0.36
            built = seed >= 0.2
            tile = (0 if kind_roll < 0.6 else 3) if built else (1 if kind_roll < 0.6 else 2)
            ty, tx = divmod(tile, 2)
            e = 0.002
            streets.quad([(cx - BLOCK / 2, GROUND, cz - BLOCK / 2), (cx + BLOCK / 2, GROUND, cz - BLOCK / 2),
                          (cx + BLOCK / 2, GROUND, cz + BLOCK / 2), (cx - BLOCK / 2, GROUND, cz + BLOCK / 2)],
                         [(tx / 2 + e, 0.5 - ty / 2 + e), (tx / 2 + 0.5 - e, 0.5 - ty / 2 + e),
                          (tx / 2 + 0.5 - e, 1 - ty / 2 - e), (tx / 2 + e, 1 - ty / 2 - e)], (1, 1, 1), (0, 1, 0))
            if not built:
                continue
            away = math.hypot(cx - downtown[0], cz - downtown[1])
            off_axis = abs(cz - EYE[2])
            high = 12 + 62 * seed * seed + 150 * math.exp(-(away / 170.0) ** 2) * seed
            frames = r in (1, 2) and 40 < off_axis < 75
            if frames:
                high = rng.uniform(46, 62)                      # near towers at the view's sides
            elif r < 2:
                high = rng.uniform(9, 26)                       # roofs to look down on
            elif off_axis < 40 and cx - EYE[0] < 220:
                high = min(high, 22 + 0.13 * (cx - EYE[0]))     # a clear view down the middle
            tint = tuple(rng.uniform(0.75, 1.15) * t for t in (1.0, rng.uniform(0.94, 1.04), rng.uniform(0.88, 1.08)))
            mat = rng.choice(("CityGlass", "CityOffice", "CityOffice") if high > 60 else ("CityFlats", "CityOffice", "CityFlats", "CityGlass"))
            hx, hz = lot * rng.uniform(0.6, 1.0), lot * rng.uniform(0.6, 1.0)
            if frames:
                hx, hz = lot, lot
            top = GROUND + high
            near = cx - EYE[0] < 260 and high < 70
            if cx - EYE[0] < 400:
                shopfronts(cx, cz, hx, hz)
            shape = rng.random()
            if high > 55 and shape < 0.45 and not frames:   # setbacks
                tower(cx, cz, hx, hz, GROUND, GROUND + high * 0.35, mat, tint)
                tower(cx, cz, hx * 0.78, hz * 0.78, GROUND + high * 0.35, GROUND + high * 0.78, mat, tint)
                tower(cx, cz, hx * 0.52, hz * 0.52, GROUND + high * 0.78, top, mat, tint)
                hx, hz = hx * 0.52, hz * 0.52
            elif high > 40 and shape < 0.62 and not frames:    # two towers on a podium
                tower(cx, cz, lot, lot, GROUND, GROUND + high * 0.18, mat, tint)
                tower(cx, cz - lot * 0.52, lot * 0.8, lot * 0.4, GROUND + high * 0.18, top, mat, tint)
                tower(cx, cz + lot * 0.52, lot * 0.8, lot * 0.4, GROUND + high * 0.18, top - high * 0.12, mat, tint)
                hx, hz, cz = lot * 0.8, lot * 0.4, cz - lot * 0.52
            elif shape > 0.85 and not frames:                  # a wing lower than the rest
                tower(cx, cz - hz * 0.5, hx, hz * 0.5, GROUND, top, mat, tint)
                tower(cx, cz + hz * 0.5, hx * 0.7, hz * 0.5, GROUND, GROUND + high * 0.6, mat, tint)
                hz, cz = hz * 0.5, cz - hz * 0.5
            else:
                tower(cx, cz, hx, hz, GROUND, top, mat, tint)
            if near:
                clutter(cx, cz, min(hx, hz), top)
            if high > 90:
                crown(cx, cz, hx, hz, top)
            if high > 75 or rng.random() < 0.06:
                mast(cx + rng.uniform(-1, 1), cz + rng.uniform(-1, 1), top, rng.uniform(6, 22))
            if cx - EYE[0] < 420 and rng.random() < 0.07 and high > 20:
                sign(cx, cz, min(hx, hz), top)
            if r == RIVER[1]:   # the far bank: its lit fronts lie on the water
                reach = min(46.0, 12 + high * 0.5)
                lit = tuple(0.16 * t for t in tint)
                glow.flat(river_x1 - reach, river_x1, cz - hz, cz + hz, WATER_Y + 0.3, [WATER, lit, lit, WATER])

    # lamps along both banks, each with a streak on the water
    z = EYE[2] - ACROSS * BLOCK
    while z < EYE[2] + ACROSS * BLOCK:
        lamp = (1.0, 0.66, 0.30)
        dim = tuple(c * 0.5 for c in lamp)
        glow.flat(river_x0 - 2.2, river_x0 - 0.4, z - 0.9, z + 0.9, GROUND + 0.3, lamp)
        glow.flat(river_x0, river_x0 + 20, z - 0.7, z + 0.7, WATER_Y + 0.3, [dim, WATER, WATER, dim])
        glow.flat(river_x1 + 0.4, river_x1 + 2.2, z + 3.1, z + 4.9, GROUND + 0.3, lamp)
        glow.flat(river_x1 - 20, river_x1, z + 3.3, z + 4.7, WATER_Y + 0.6, [WATER, dim, dim, WATER])
        z += 9.0
    # two bridges, lamps along their rails
    for dz in (-78.0, 114.0):
        cz = EYE[2] + dz
        box(glow, river_x0 - 6, river_x1 + 6, GROUND + 3.5, GROUND + 5.0, cz - 5, cz + 5, (0.07, 0.07, 0.08), every=True)
        for px in (river_x0 + 10, (river_x0 + river_x1) / 2, river_x1 - 10):
            box(glow, px - 1.5, px + 1.5, WATER_Y, GROUND + 3.5, cz - 4, cz + 4, (0.05, 0.05, 0.06), every=True)
        x = river_x0 - 4
        while x < river_x1 + 4:
            for side in (-1, 1):
                box(glow, x - 0.3, x + 0.3, GROUND + 7.2, GROUND + 7.8, cz + side * 4.7 - 0.3, cz + side * 4.7 + 0.3, (1.0, 0.85, 0.6), every=True)
            x += 7.0
        soft = (0.22, 0.16, 0.09)
        glow.flat(river_x0, river_x1, cz - 22, cz - 5, WATER_Y + 0.9, [WATER, WATER, soft, soft])
        glow.flat(river_x0, river_x1, cz + 5, cz + 22, WATER_Y + 0.9, [soft, soft, WATER, WATER])

    # three towers that stand over the rest
    for dx, dz, high, mat in ((300, 30, 215, "CityGlass"), (430, -95, 175, "CityOffice"), (250, -150, 140, "CityGlass")):
        cx, cz = EYE[0] + dx, EYE[2] + dz
        steps = ((1.0, 0.0, 0.55), (0.72, 0.55, 0.85), (0.45, 0.85, 1.0))
        for size, lo, hi in steps:
            tower(cx, cz, 11 * size, 11 * size, GROUND + high * lo, GROUND + high * hi, mat, (1.1, 1.1, 1.15))
            crown(cx, cz, 11 * size, 11 * size, GROUND + high * hi)
        box(glow, cx - 5.2, cx + 5.2, GROUND + high, GROUND + high + 2.5, cz - 5.2, cz + 5.2, (0.9, 0.95, 1.0), every=True)
        mast(cx, cz, GROUND + high + 2.5, 38)

    # rain: sheets of streaks at a few distances from the glass (Rain.shader scrolls them)
    rain = Batch()
    for away, bright in ((2.5, 1.0), (6.0, 0.8), (14.0, 0.6), (34.0, 0.45)):
        x = EYE[0] + away
        half, low, high = 10 + away * 1.2, EYE[1] - 12 - away, EYE[1] + 8 + away * 0.6
        shift = away * 0.37
        rain.quad([(x, low, EYE[2] - half), (x, low, EYE[2] + half), (x, high, EYE[2] + half), (x, high, EYE[2] - half)],
                  [(shift, low / 5.0), (shift + 2 * half / 2.5, low / 5.0), (shift + 2 * half / 2.5, high / 5.0), (shift, high / 5.0)],
                  (bright, bright, bright), (-1, 0, 0))
    b.obj("Rain")
    b.mesh(rain.vertices, rain.faces, rain.uvs, "Rain", normal=rain.normals, colours=rain.colours)

    b.obj("City towers")
    for mat, batch in walls.items():
        b.mesh(batch.vertices, batch.faces, batch.uvs, mat, normal=batch.normals, colours=batch.colours)
    b.obj("City roofs")
    b.mesh(glow.vertices, glow.faces, glow.uvs, "CityGlow", normal=glow.normals, colours=glow.colours)
    b.obj("City streets")
    b.mesh(streets.vertices, streets.faces, streets.uvs, "CityStreet", normal=streets.normals, colours=streets.colours)
