"""Low-poly models made by script for Tiberian Dawn's 3D view (docs/plans/td3d-plan.md, "The models").

A model is parts (a hull, a turret...), each a list of triangles, each triangle three points and a
material. Units are the game's pixels: a cell is 24 across. Axes: x to the model's right, y ahead
(a unit is made facing north, the game's facing 0), z up. A part's points are about its own pivot
(a turret turns about 0, 0), and the part says where that pivot is on the model.

The primitives leave out faces that look straight down: the camera never sees under the ground.
Nothing here comes from the game's art or anyone's model: every shape is the numbers in a script.
"""
import math

# Materials: four bits of a vertex's tag (the other four are its normal: pack.py). The game makes
# a table of colours for each house from these (gl.cpp's model table): HOUSE and HOUSE_DARK are
# the house's own colours (its remap of the palette), the rest are fixed.
HOUSE, HOUSE_DARK, METAL, DARK, BLACK, GLASS, CONCRETE, CONCRETE_DARK, GREEN, WOOD, SAND, LIGHT, RUST, ROOF, WHITE = range(15)
MATERIALS = {
    HOUSE: None, HOUSE_DARK: None,
    METAL: (150, 150, 140), DARK: (70, 70, 64), BLACK: (24, 24, 24), GLASS: (90, 150, 190),
    CONCRETE: (176, 168, 150), CONCRETE_DARK: (120, 114, 100), GREEN: (60, 110, 50), WOOD: (110, 80, 50),
    SAND: (190, 170, 120), LIGHT: (230, 60, 40), RUST: (140, 90, 60), ROOF: (96, 100, 108), WHITE: (225, 225, 215),
}


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def normal(a, b, c):
    """The face's normal, unit length (counter-clockwise seen from outside)."""
    n = _cross(_sub(b, a), _sub(c, a))
    length = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2)
    return (0.0, 0.0, 1.0) if length == 0 else (n[0] / length, n[1] / length, n[2] / length)


class Part:
    def __init__(self, name, pivot=(0.0, 0.0, 0.0), turns=False):
        self.name = name
        self.pivot = pivot      # where its 0, 0, 0 lies on the model
        self.turns = turns      # True: a turret, turned by the unit's second facing
        self.triangles = []     # ((a, b, c), material)

    def triangle(self, a, b, c, material):
        n = normal(a, b, c)
        if n[2] < -0.7:
            return              # (looks down: never seen)
        self.triangles.append(((tuple(a), tuple(b), tuple(c)), material))

    def quad(self, a, b, c, d, material):
        self.triangle(a, b, c, material)
        self.triangle(a, c, d, material)

    def polygon(self, points, material):
        """A convex polygon as a fan, its points counter-clockwise seen from outside."""
        for i in range(1, len(points) - 1):
            self.triangle(points[0], points[i], points[i + 1], material)

    # ---- primitives ----

    def prism(self, footprint, z0, z1, material, top=None, shrink=1.0, shift=(0.0, 0.0)):
        """A footprint (x, y points, counter-clockwise from above) from z0 up to z1. The top may be
        smaller (shrink about its middle) and moved (shift): a hull's sloped sides, a roof."""
        top = material if top is None else top
        mx = sum(p[0] for p in footprint) / len(footprint)
        my = sum(p[1] for p in footprint) / len(footprint)
        low = [(x, y, z0) for x, y in footprint]
        high = [(mx + (x - mx) * shrink + shift[0], my + (y - my) * shrink + shift[1], z1) for x, y in footprint]
        n = len(footprint)
        for i in range(n):
            j = (i + 1) % n
            self.quad(low[i], low[j], high[j], high[i], material)
        self.polygon(high, top)
        self.polygon(list(reversed(low)), material)

    def box(self, x0, y0, z0, x1, y1, z1, material, top=None):
        self.prism([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], z0, z1, material, top)

    def wedge(self, x0, y0, z0, x1, y1, z1, material, front_drop, top=None):
        """A box whose top falls by front_drop toward its front (+y): a glacis, a cab's hood."""
        top = material if top is None else top
        p = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
             (x0, y0, z1), (x1, y0, z1), (x1, y1, z1 - front_drop), (x0, y1, z1 - front_drop)]
        self.quad(p[0], p[1], p[5], p[4], material)   # back
        self.quad(p[1], p[2], p[6], p[5], material)   # right
        self.quad(p[2], p[3], p[7], p[6], material)   # front
        self.quad(p[3], p[0], p[4], p[7], material)   # left
        self.quad(p[4], p[5], p[6], p[7], top)        # top

    def cylinder(self, start, end, radius, sides, material, caps=True, end_radius=None):
        """A tube from start to end (a barrel, a tank, a chimney), `sides` round; a cone if end_radius."""
        end_radius = radius if end_radius is None else end_radius
        axis = _sub(end, start)
        length = math.sqrt(sum(a * a for a in axis))
        w = tuple(a / length for a in axis)
        helper = (0.0, 0.0, 1.0) if abs(w[2]) < 0.9 else (1.0, 0.0, 0.0)
        u = _cross(w, helper)
        ul = math.sqrt(sum(a * a for a in u))
        u = tuple(a / ul for a in u)
        v = _cross(w, u)
        ring0, ring1 = [], []
        for i in range(sides):
            a = 2 * math.pi * i / sides
            d = tuple(math.cos(a) * u[k] + math.sin(a) * v[k] for k in range(3))
            ring0.append(tuple(start[k] + radius * d[k] for k in range(3)))
            ring1.append(tuple(end[k] + end_radius * d[k] for k in range(3)))
        for i in range(sides):
            j = (i + 1) % sides
            self.quad(ring0[i], ring0[j], ring1[j], ring1[i], material)
        if caps:
            self.polygon(list(reversed(ring0)), material)
            if end_radius > 0:
                self.polygon(ring1, material)

    def octagon(self, cx, cy, half_w, half_l, cut):
        """A footprint of a rectangle with its corners cut (a turret's, a hull's), counter-clockwise."""
        return [(cx - half_w + cut, cy - half_l), (cx + half_w - cut, cy - half_l), (cx + half_w, cy - half_l + cut),
                (cx + half_w, cy + half_l - cut), (cx + half_w - cut, cy + half_l), (cx - half_w + cut, cy + half_l),
                (cx - half_w, cy + half_l - cut), (cx - half_w, cy - half_l + cut)]


class Model:
    def __init__(self, name, kind="unit", cells=(1, 1)):
        self.name = name        # the game's ini name: MTNK, NUKE...
        self.kind = kind        # unit, building, aircraft, infantry, terrain
        self.cells = cells      # a building's cells across and down (its footprint)
        self.parts = []

    def part(self, name, pivot=(0.0, 0.0, 0.0), turns=False):
        p = Part(name, pivot, turns)
        self.parts.append(p)
        return p

    def triangles(self):
        return sum(len(p.triangles) for p in self.parts)

    def bounds(self):
        points = [tuple(q[k] + p.pivot[k] for k in range(3)) for p in self.parts for t, _ in p.triangles for q in t]
        return [min(q[k] for q in points) for k in range(3)], [max(q[k] for q in points) for k in range(3)]
