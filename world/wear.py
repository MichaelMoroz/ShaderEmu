# What use leaves on a surface, laid over its material in Unity (ShaderEmuModels.cs: Wear): a set
# of layers made once here (a moulded or rolled grain, scratches, fingerprints, dust, rings), mixed
# by a recipe for each kind of thing, and written as one detail set a recipe:
#   Wear<NAME>.png     colour overlay: 0.5 changes nothing (Mochie's overlay blend)
#   Wear<NAME>_r.png   roughness overlay: 0.5 changes nothing
#   Wear<NAME>_n.png   normal map
# (an overlay doubles a change on a dark base: 0.05 is a fifth lighter on black plastic)
# and Wear.txt, a line a recipe: material, its set, metres a repeat. Unity lays the set over the
# material's own maps by where a place is in the room (triplanar), so it needs no coordinates of
# the model's and has no seams of them.
#   python world/wear.py
# Every size below is in metres of the surface and comes from what is said beside it: a source, or
# "a guess" where none was found. A recipe says how much of each layer a thing has had of its use.
import math
import os

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "unity", "ShaderEmu", "Textures", "World")


def noise(n, size_px, seed, stretch=(1.0, 1.0)):
    """Periodic noise of features about size_px across (a band of frequencies round n / size_px),
    mean 0, deviation 1; stretch (x, y) draws it out one way (a rolled sheet's streaks)."""
    rng = np.random.default_rng(seed)
    fy, fx = np.meshgrid(np.fft.fftfreq(n) * n, np.fft.fftfreq(n) * n, indexing="ij")
    f = np.hypot(fx * stretch[0], fy * stretch[1])
    centre = n / max(size_px, 1e-3)
    amp = np.exp(-0.5 * (np.log(np.maximum(f, 1e-6) / centre) / 0.6) ** 2)
    amp[0, 0] = 0.0
    field = np.fft.ifft2(amp * np.exp(2j * np.pi * rng.random((n, n)))).real
    return field / (field.std() + 1e-12)


class Layer:
    """What a layer does at each texel: height (metres), roughness and colour changes, all added."""

    def __init__(self, n):
        self.height = np.zeros((n, n))
        self.rough = np.zeros((n, n))
        self.colour = np.zeros((n, n))


def wrapped(draw_at, n):
    """Calls draw_at(dx, dy) at the nine copies of the tile, so what crosses an edge comes back on
    the other side: the set repeats with no seam."""
    for dx in (-n, 0, n):
        for dy in (-n, 0, n):
            draw_at(dx, dy)


# ---- the grain of the surface itself

def vdi_ra(grade):
    """VDI 3400's grades are 20 log10(10 Ra), Ra in micrometres: grade 12 is 0.40 um, 30 is 3.15,
    45 is 18 (VDI 3400, 1975; as Mold-Tech's and every mould maker's charts give it)."""
    return 0.1 * 10 ** (grade / 20.0) * 1e-6


def grain(w, n, tile, grade, seed, stretch=(1.0, 1.0)):
    """A spark-eroded (or rolled) grain of VDI 3400 grade `grade`: its Ra sets how deep it is. Its
    craters' size, about 25 times its Ra across, is a guess (no source gives a spacing); where that is
    under a texel the grain shows as roughness alone, which the material's own already has."""
    texel = tile / n
    across = 25 * vdi_ra(grade)
    h = noise(n, max(across / texel, 1.5), seed, stretch) * vdi_ra(grade)
    w.height += h


# ---- scratches

def scratches(w, n, tile, per_m2, length, width, depth, seed, rough=0.0, colour=0.0, bend=0.0, along=None):
    """Lines worn into the surface: per_m2 of them, their lengths log-normal about `length` (half to
    twice it), `width` and `depth` in metres (a hairline is 10 to 50 micrometres wide: under a texel it
    is drawn a texel wide and as much fainter). rough, colour: what one does to the roughness and
    colour where it is (a scratch in a matte grain is burnished smoother; in lacquer or metal it
    scatters, rougher and lighter). bend: how much a line curves over its length (a wipe's arc);
    along: radians, the way most run (None: any way)."""
    rng = np.random.default_rng(seed)
    texel = tile / n
    count = int(round(per_m2 * tile * tile))
    canvas = Image.new("F", (n, n), 0.0)
    draw = ImageDraw.Draw(canvas)
    px_w = max(1, int(round(width / texel)))
    faint = min(1.0, width / texel)
    for _ in range(count):
        x, y = rng.random(2) * n
        long = length * math.exp(rng.normal(0, 0.45)) / texel
        angle = along + rng.normal(0, 0.15) if along is not None else rng.random() * math.pi
        curve = rng.normal(0, bend)
        strength = faint * rng.uniform(0.3, 1.0)
        points = []
        for k in range(9):
            t = k / 8 - 0.5
            a = angle + curve * t
            points.append((x + math.cos(a) * long * t, y + math.sin(a) * long * t))
        wrapped(lambda dx, dy: draw.line([(px + dx, py + dy) for px, py in points], fill=strength, width=px_w), n)
    s = np.minimum(np.asarray(canvas), 1.0)
    w.height -= s * depth
    w.rough += s * rough
    w.colour += s * colour


# ---- fingerprints

def fingerprints(w, n, tile, per_m2, seed, rough=-0.15, colour=-0.03):
    """Prints of fingers that held the thing: an oval of 15 x 20 mm (an adult fingertip's pad; a
    guess from a ruler on a hand), ridges 0.5 mm apart (about 20 ridges a centimetre, the usual
    figure for adults' ridge density in forensic texts), in loops round a core, only partly left
    (a print is never whole). Skin's oil makes a matte surface glossier (rough < 0) and a polished
    one duller (rough > 0)."""
    rng = np.random.default_rng(seed)
    texel = tile / n
    count = max(1, int(round(per_m2 * tile * tile)))
    yy, xx = np.mgrid[0:n, 0:n].astype(float)
    total = np.zeros((n, n))
    period = 0.0005 / texel
    for _ in range(count):
        cx, cy = rng.random(2) * n
        a, b = 0.0075 / texel, 0.010 / texel
        turn = rng.random() * math.pi
        dx = (xx - cx + n / 2) % n - n / 2
        dy = (yy - cy + n / 2) % n - n / 2
        u = dx * math.cos(turn) + dy * math.sin(turn)
        v = -dx * math.sin(turn) + dy * math.cos(turn)
        oval = np.clip(1.2 - np.hypot(u / a, v / b), 0, 1)
        if not oval.any():
            continue
        core = (u - rng.normal(0, 0.2) * a) ** 2 + (v / 1.3 - 0.2 * b) ** 2   # a loop: rings round a core, drawn out along the finger
        ridges = 0.5 + 0.5 * np.cos(2 * math.pi * np.sqrt(core) / period + 2.0 * noise(n, 40 * period, int(rng.integers(1 << 30))))
        left = np.clip(0.5 + 0.8 * noise(n, a, int(rng.integers(1 << 30))), 0, 1)   # what of it was left
        total = np.maximum(total, oval * ridges * left)
    w.rough += total * rough
    w.colour += total * colour


# ---- dust

def dust(w, n, tile, amount, seed, colour=0.25, rough=0.15):
    """Dust settled since the thing was last wiped: specks of 10 to 100 micrometres (most household
    dust; under a texel each) by `amount`, the share of a texel they cover where it lies thickest,
    thicker in patches (where a cloth missed). It is lighter than a dark surface and rough."""
    rng = np.random.default_rng(seed)
    patches = np.clip(0.7 + 0.3 * noise(n, n / 2, seed), 0, 1)   # (broad: dust lies evenly on a flat face; a cloth's misses are wider than a small thing)
    specks = rng.random((n, n)) ** 6
    total = np.clip(amount * patches * (0.5 + 2.0 * specks), 0, 1)
    w.colour += total * colour
    w.rough += total * rough


# ---- rings

def rings(w, n, tile, per_m2, seed, diameter=0.08, edge=0.0015, colour=0.05, rough=0.08):
    """Where a wet mug stood: a ring of its foot's diameter (a mug's foot is about 80 mm across: a
    guess from common mugs), its edge a line 1.5 mm wide, faint, a water mark in the finish."""
    rng = np.random.default_rng(seed)
    texel = tile / n
    count = int(round(per_m2 * tile * tile))
    yy, xx = np.mgrid[0:n, 0:n].astype(float)
    total = np.zeros((n, n))
    for _ in range(count):
        cx, cy = rng.random(2) * n
        r = diameter * rng.uniform(0.9, 1.1) / 2 / texel
        d = np.hypot((xx - cx + n / 2) % n - n / 2, (yy - cy + n / 2) % n - n / 2)
        ring = np.exp(-0.5 * ((d - r) / (edge / texel / 2)) ** 2)
        broken = np.clip(0.4 + noise(n, r / 2, int(rng.integers(1 << 30))), 0, 1)
        total = np.maximum(total, ring * broken * rng.uniform(0.4, 1.0))
    w.colour += total * colour
    w.rough += total * rough


# ---- recipes: a thing's material and its use

RECIPES = {
    # A 3.5 inch disk's case (world/toys.py: floppy). ECMA-147 gives its sizes, not its plastic;
    # disks' cases were moulded in a fine matte grain (in photographs of Sony's and 3M's of the 1990s it
    # reads as the fine end of the charts: grade 24 is a guess). A disk is handled at its edge and
    # carried in boxes: a print or two, hairlines across it, a little dust.
    "FloppyShell": dict(tile=0.1, n=1024, layers=[
        ("grain", dict(grade=24, seed=1)),
        ("scratches", dict(per_m2=4000, length=0.012, width=30e-6, depth=2e-6, seed=2, rough=-0.25, colour=0.06)),
        ("fingerprints", dict(per_m2=150, seed=3, rough=-0.2, colour=-0.02)),
        ("dust", dict(amount=0.02, seed=4)),
    ]),
    # Its shutter: stainless sheet, 0.2 mm (world/toys.py), as rolled: EN 10088-2's 2B finish (cold
    # rolled, annealed, pickled and skin passed: smooth, a little dull, faint streaks along the
    # rolling), Ra 0.1 to 0.5 um. It slides in the drive and under fingers: hairlines along its
    # travel, and prints, which show on bare metal.
    "FloppyShutter": dict(tile=0.1, n=1024, layers=[
        ("grain", dict(grade=10, seed=11, stretch=(0.08, 1.0))),
        ("scratches", dict(per_m2=20000, length=0.008, width=20e-6, depth=1e-6, seed=12, rough=0.25, colour=0.1, along=0.0)),
        ("fingerprints", dict(per_m2=200, seed=13, rough=0.3, colour=0.04)),
    ]),
    # The side rooms' desks (world/rooms.py: desk): walnut veneer under a satin lacquer (a guess:
    # an office desk of the 1990s). Years of use: hairlines in the lacquer every way, arcs from
    # wiping, rings where mugs stood, prints and a little dust.
    "DeskWalnut": dict(tile=0.5, n=2048, layers=[
        ("scratches", dict(per_m2=2500, length=0.03, width=40e-6, depth=3e-6, seed=21, rough=0.3, colour=0.04)),
        ("scratches", dict(per_m2=150, length=0.12, width=30e-6, depth=1e-6, seed=22, rough=0.2, colour=0.02, bend=1.5)),
        ("rings", dict(per_m2=6, seed=23)),
        ("fingerprints", dict(per_m2=12, seed=24, rough=0.15, colour=0.02)),
        ("dust", dict(amount=0.04, seed=25, colour=0.12)),
    ]),
    # The same desk's upright faces (its pedestal, panel end, drawers' fronts, modesty board): no
    # mug stood on them and no dust lies on them (Unity lays a set from all three axes, so what only
    # an upward face gets is a recipe of its own); knocks and hands' prints on the fronts.
    "DeskWalnutSides": dict(tile=0.5, n=2048, layers=[
        ("scratches", dict(per_m2=1200, length=0.02, width=40e-6, depth=3e-6, seed=26, rough=0.3, colour=0.04)),
        ("fingerprints", dict(per_m2=12, seed=27, rough=0.15, colour=0.02)),
    ]),
}

MAKERS = {"grain": grain, "scratches": scratches, "fingerprints": fingerprints, "dust": dust, "rings": rings}
SLOPE = 0.35   # the normal map's slope a texel for a height step of a texel's width: the bumps' strength (a guess, by eye against the scan)


def make(name, recipe):
    n, tile = recipe["n"], recipe["tile"]
    w = Layer(n)
    for kind, args in recipe["layers"]:
        MAKERS[kind](w, n, tile, **args)
    texel = tile / n
    gy, gx = np.gradient(w.height / texel)
    v = np.stack([-gx * SLOPE * 20, gy * SLOPE * 20, np.ones_like(gx)], -1)   # rows run down, v runs up
    v /= np.linalg.norm(v, axis=-1, keepdims=True)
    save = lambda a, suffix: Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)).save(os.path.join(OUT, "Wear" + name + suffix + ".png"))
    save(v * 0.5 + 0.5, "_n")
    save(np.repeat((0.5 + w.rough)[..., None], 3, 2), "_r")
    save(np.repeat((0.5 + w.colour)[..., None], 3, 2), "")
    print("wear %-14s %d px over %.2f m (%.2f mm a texel): roughness %+.3f mean, colour %+.3f mean"
          % (name, n, tile, texel * 1000, w.rough.mean(), w.colour.mean()))


def main():
    for name, recipe in RECIPES.items():
        make(name, recipe)
    open(os.path.join(OUT, "Wear.txt"), "w").write("".join("%s\tWear%s\t%.3f\n" % (name, name, r["tile"]) for name, r in RECIPES.items()))


if __name__ == "__main__":
    main()
