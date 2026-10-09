# The room's textures, computed (python world/textures.py). Tiling ones repeat exactly:
# their noise is made in the frequency domain. Albedo alpha is the Standard shader's smoothness.
import json
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "unity", "ShaderEmu", "Textures", "World")
FONTS = "C:/Windows/Fonts/"


def noise(n, beta, seed, stretch=(1.0, 1.0), cut=None):
    """Periodic noise, mean 0 and deviation 1: power falls as frequency^-beta."""
    rng = np.random.default_rng(seed)
    fy, fx = np.meshgrid(np.fft.fftfreq(n) * n, np.fft.fftfreq(n) * n, indexing="ij")
    f = np.hypot(fx * stretch[0], fy * stretch[1])
    f[0, 0] = 1.0
    amp = f ** -beta
    amp[0, 0] = 0.0
    if cut is not None:
        amp *= np.exp(-(f / cut) ** 2)
    field = np.fft.ifft2(amp * np.exp(2j * np.pi * rng.random((n, n)))).real
    return field / field.std()


def unit(a):
    return (a - a.min()) / (a.max() - a.min())


def normal_map(height, strength):
    n = height.shape[1]
    dx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * 0.5 * n * strength
    dy = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * 0.5 * height.shape[0] * strength
    v = np.stack([-dx, dy, np.ones_like(dx)], -1)   # rows run down, v runs up
    v /= np.linalg.norm(v, axis=-1, keepdims=True)
    return v * 0.5 + 0.5


def save(name, rgb, alpha=None):
    a = np.clip(rgb, 0, 1)
    if alpha is not None:
        smooth = np.clip(np.broadcast_to(alpha, a.shape[:2]), 0, 1)
        a = np.dstack([a, smooth])
        packed = np.dstack([np.ones_like(smooth), 1.0 - smooth, np.ones_like(smooth)])   # occlusion, roughness, metal
        Image.fromarray((packed * 255 + 0.5).astype(np.uint8)).save(os.path.join(OUT, name + "_p.png"))
    Image.fromarray((a * 255 + 0.5).astype(np.uint8)).save(os.path.join(OUT, name + ".png"))


def tint(shade, colour):
    return shade[..., None] * np.array(colour)[None, None, :]


def font(name, size):
    return ImageFont.truetype(FONTS + name, size)


# ---------------------------------------------------------------- tiling surfaces

def plaster():
    n = 1024
    h = 0.35 * noise(n, 0.6, 1, cut=50) + 0.65 * noise(n, 2.0, 2)
    shade = 0.93 + 0.012 * noise(n, 2.4, 3) + 0.004 * h
    save("Plaster", tint(shade, (1.0, 0.975, 0.93)), 0.06 + 0.02 * unit(h))
    save("Plaster_n", normal_map(h, 0.00012))


def ceiling():
    n = 1024
    bumps = np.clip(noise(n, 0.4, 4, cut=60) - 0.5, 0, None) ** 0.8
    h = bumps + 0.3 * noise(n, 1.8, 5)
    shade = 0.92 + 0.02 * unit(bumps) + 0.01 * noise(n, 2.2, 6)
    save("Ceiling", tint(shade, (1.0, 0.99, 0.96)), 0.03)
    save("Ceiling_n", normal_map(h, 0.0003))


def wood_grain(n, seed, rings=7):
    v = np.arange(n)[:, None] / n
    warp = 0.9 * noise(n, 2.0, seed, stretch=(0.12, 1.0)) + 0.25 * noise(n, 1.4, seed + 1, stretch=(0.05, 1.0))
    bands = 0.5 + 0.5 * np.sin(2 * np.pi * (v * rings + 0.35 * warp))
    pores = noise(n, 0.6, seed + 2, stretch=(0.03, 1.0), cut=260)
    return 0.45 * bands ** 1.3 + 0.25 * unit(pores) + 0.30 * unit(noise(n, 2.0, seed + 3, stretch=(0.2, 1.0)))


def wood():
    n = 1024
    g = wood_grain(n, 10, rings=5)
    shade = 0.80 + 0.20 * g
    rgb = np.stack([0.96 * shade, 0.80 * shade ** 1.2, 0.60 * shade ** 1.5], -1)
    save("Wood", rgb, 0.30 + 0.12 * g)
    save("Wood_n", normal_map(g, 0.00035))


def parquet():
    n, rows = 2048, 24
    rng = np.random.default_rng(20)
    grain = wood_grain(n, 21, rings=16)
    shade = np.zeros((n, n))
    gap = np.zeros((n, n))
    tone = np.zeros((n, n, 3))
    row_h = n // rows
    for r in range(rows):
        y0, y1 = r * row_h, (r + 1) * row_h if r < rows - 1 else n
        cuts = sorted(rng.choice(np.arange(8) * (n // 8), size=3, replace=False) + rng.integers(0, n // 8))
        for i in range(3):
            x0, x1 = cuts[i] % n, cuts[(i + 1) % 3] % n
            cols = np.arange(x0, x1 if x1 > x0 else x1 + n) % n
            shift = rng.integers(0, n, 2)
            shade[y0:y1, cols] = np.roll(grain, shift, (0, 1))[y0:y1, :][:, cols]
            light = rng.uniform(0.86, 1.04)
            tone[y0:y1, cols] = np.array([light, light * rng.uniform(0.96, 1.02), light * rng.uniform(0.92, 1.0)])
            gap[y0:y1, cols[:2]] = 1.0
        gap[y0:y0 + 2, :] = 1.0
    base = 0.74 + 0.26 * shade
    rgb = np.stack([0.66 * base, 0.49 * base ** 1.2, 0.33 * base ** 1.5], -1) * tone
    rgb *= (1 - 0.5 * gap)[..., None]
    save("Parquet", rgb, (0.42 + 0.08 * shade) * (1 - 0.7 * gap))
    save("Parquet_n", normal_map(0.2 * shade - 0.8 * gap, 0.0003))


def plastic():
    # moulded ABS: an even colour, a fine pebbled grain (a third of a millimetre a texel at this
    # material's 0.3 m a repeat), a soft sheen
    n = 1024
    h = unit(noise(n, 0.9, 30, cut=260)) * 0.7 + unit(noise(n, 0.0, 33, cut=420)) * 0.3
    save("Plastic", tint(0.975 + 0.012 * unit(h), (1, 1, 1)), 0.56 + 0.05 * unit(h))
    save("Plastic_n", normal_map(h, 0.0005))


def metal():
    n = 512
    h = noise(n, 0.5, 40, stretch=(0.01, 1.0))
    save("Metal", tint(0.90 + 0.07 * unit(h), (1, 1, 1)), 0.58 + 0.14 * unit(h))
    save("Metal_n", normal_map(h, 0.00008))


def fabric():
    n, period = 512, 4
    y, x = np.mgrid[0:n, 0:n]
    over = ((x // period) + (y // period)) % 2 == 0
    h = np.where(over, np.abs(np.sin(np.pi * x / period)), np.abs(np.sin(np.pi * y / period)))
    h = h * (0.8 + 0.2 * unit(noise(n, 0.3, 50))) + 0.25 * unit(noise(n, 1.8, 51))
    shade = 0.82 + 0.14 * unit(h) + 0.03 * noise(n, 2.2, 52)
    save("Fabric", tint(shade, (1, 1, 1)), 0.03)
    save("Fabric_n", normal_map(h, 0.0006))


def cardboard():
    n = 512
    fibre = noise(n, 0.8, 60, stretch=(0.04, 1.0))
    flute = 0.5 + 0.5 * np.sin(2 * np.pi * np.arange(n)[:, None] * 48 / n) * np.ones((1, n))
    shade = 0.90 + 0.05 * unit(fibre) + 0.03 * flute + 0.04 * unit(noise(n, 2.0, 61))
    save("Cardboard", tint(shade, (0.80, 0.63, 0.44)), 0.08)
    save("Cardboard_n", normal_map(0.5 * unit(fibre) + 0.6 * flute, 0.0005))


def leaf():
    w, h = 128, 256
    y, x = np.mgrid[0:h, 0:w]
    u, v = (x + 0.5) / w - 0.5, (y + 0.5) / h
    rib = np.exp(-(u / 0.035) ** 2)
    veins = np.exp(-(((v * 9 - np.abs(u) * 5) % 1.0 - 0.5) / 0.09) ** 2) * (np.abs(u) > 0.03)
    shade = 0.85 + 0.15 * np.cos(u * np.pi) - 0.12 * veins + 0.25 * rib
    rgb = tint(shade, (0.20, 0.47, 0.19)) + tint(rib + 0.4 * veins, (0.10, 0.08, 0.02))
    save("Leaf", rgb, 0.35)


# ---------------------------------------------------------------- rugs

RUG = {"red": (0.45, 0.10, 0.10), "navy": (0.10, 0.13, 0.27), "cream": (0.86, 0.79, 0.62), "gold": (0.72, 0.52, 0.22),
       "teal": (0.13, 0.33, 0.34)}


def weave(shape, seed):
    h, w = shape
    y, x = np.mgrid[0:h, 0:w]
    knots = 0.5 + 0.5 * np.sin(np.pi * x / 2) * np.sin(np.pi * y / 2)
    rng = np.random.default_rng(seed)
    return 0.86 + 0.10 * knots + 0.06 * rng.random(shape)


def rug():
    w, h = 1024, 592
    y, x = np.mgrid[0:h, 0:w]
    u, v = (x + 0.5) / w, (y + 0.5) / h
    edge = np.minimum(np.minimum(u, 1 - u) * w / h, np.minimum(v, 1 - v))
    rgb = np.zeros((h, w, 3))

    def fill(mask, colour):
        rgb[mask] = RUG[colour]
    fill(edge >= 0, "red")
    lattice = np.abs((u * 14) % 1.0 - 0.5) + np.abs((v * 8) % 1.0 - 0.5)
    fill((lattice < 0.17) & (edge > 0.16), "navy")
    fill((lattice < 0.08) & (edge > 0.16), "gold")
    d = np.abs(u - 0.5) * w / h / 0.42 + np.abs(v - 0.5) / 0.30
    fill(d < 1.0, "navy")
    fill((d < 0.92) & (d > 0.84), "gold")
    fill(d < 0.78, "cream")
    star = np.abs(((u - 0.5) * w / h * 9) % 1.0 - 0.5) + np.abs(((v - 0.5) * 9) % 1.0 - 0.5)
    fill((d < 0.74) & (star < 0.2), "red")
    fill(d < 0.22, "teal")
    for lo, hi, colour in ((0.0, 0.035, "cream"), (0.035, 0.055, "navy"), (0.055, 0.11, "gold"), (0.11, 0.125, "navy"), (0.125, 0.14, "cream")):
        fill((edge >= lo) & (edge < hi), colour)
    zig = np.abs(((u + v) * 40) % 1.0 - 0.5) < 0.2
    fill((edge >= 0.062) & (edge < 0.103) & zig, "red")
    pile = weave((h, w), 70)
    save("Rug", rgb * pile[..., None], 0.02)
    save("Rug_n", normal_map(pile, 0.0006))


def round_rug():
    n = 512
    y, x = np.mgrid[0:n, 0:n]
    u, v = (x + 0.5) / n - 0.5, (y + 0.5) / n - 0.5
    r, a = np.hypot(u, v) * 2, np.arctan2(v, u)
    rgb = np.zeros((n, n, 3))
    rgb[:] = RUG["teal"]
    for lo, hi, colour in ((0.0, 0.18, "gold"), (0.18, 0.22, "navy"), (0.62, 0.66, "cream"), (0.66, 0.82, "navy"), (0.82, 0.86, "gold"), (0.86, 2.0, "cream")):
        rgb[(r >= lo) & (r < hi)] = RUG[colour]
    petals = np.abs(((a / (2 * np.pi)) * 16) % 1.0 - 0.5)
    rgb[(r > 0.26) & (r < 0.58) & (petals < 0.5 * (0.58 - r) / 0.32 * 0.9)] = RUG["red"]
    rgb[(r >= 0.68) & (r < 0.80) & (np.abs(((a / (2 * np.pi)) * 48) % 1.0 - 0.5) < 0.22)] = RUG["gold"]
    pile = weave((n, n), 71)
    save("RoundRug", rgb * pile[..., None], 0.02)


# ---------------------------------------------------------------- books: one spine a cell

TITLES = ["UNIX INTERNALS", "COMPILERS", "RISC ARCHITECTURE", "COMPUTER GRAPHICS", "OPERATING SYSTEMS", "C IN PRACTICE",
          "ALGORITHMS", "NETWORKS", "DIGITAL DESIGN", "ASSEMBLY", "THE SHELL", "TEXTURES AND LIGHT", "X WINDOWS",
          "FLOATING POINT", "KERNEL HACKING", "RAY TRACING", "LINKERS", "DATA STRUCTURES", "GAME PROGRAMMING",
          "MICROPROCESSORS", "DEBUGGING", "TYPE THEORY", "MODEM MANUAL", "VGA REFERENCE", "SOUND CARDS", "PIXELS",
          "CACHE DESIGN", "USER GUIDE", "VOLUME II", "PASCAL", "MAZES", "ATLAS"]
SPINES = [(0.50, 0.13, 0.12), (0.13, 0.26, 0.47), (0.17, 0.37, 0.23), (0.74, 0.58, 0.20), (0.32, 0.19, 0.40),
          (0.82, 0.79, 0.72), (0.12, 0.12, 0.14), (0.62, 0.33, 0.14), (0.20, 0.42, 0.46), (0.55, 0.52, 0.47)]


def books():
    from bookdesigns import CELLS, CELL_W, CELL_H, DESIGNS
    img = Image.new("RGB", (CELLS * CELL_W, CELL_H), (60, 56, 50))
    rng = np.random.default_rng(80)
    for i, (w, h) in enumerate(DESIGNS):
        base = np.array(SPINES[i % len(SPINES)]) * rng.uniform(0.75, 1.0)
        base = base * 0.8 + base.mean() * 0.2   # cloth and card, not paint
        fill = tuple(int(min(c, 1) * 255) for c in base)
        light = base.mean() > 0.5
        ink = (28, 24, 22) if light else tuple(int(c) for c in rng.choice([[226, 214, 186], [206, 170, 92], [232, 232, 228]]))
        cell = Image.new("RGB", (CELL_W, CELL_H), fill)
        draw = ImageDraw.Draw(cell)
        top = CELL_H - h   # the spine fills the cell's bottom-left w by h pixels
        style = rng.integers(0, 4)
        if style == 0:
            for y in (top + 26, top + 34, CELL_H - 40, CELL_H - 32):
                draw.rectangle((0, y, w, y + 2), fill=ink)
        elif style == 1:
            draw.rectangle((0, top + 40, w, top + 46), fill=ink)
        elif style == 2:
            draw.rectangle((4, top + 30, w - 4, top + 150), fill=tuple(int(c * 0.55) for c in fill))
        size = int(min(24, max(12, w * 0.42)))
        label = Image.new("L", (h - 150, w), 0)
        ImageDraw.Draw(label).text(((h - 150) // 2, w // 2), TITLES[i], fill=255, anchor="mm",
                                   font=font("georgiab.ttf" if i % 3 == 0 else "arialbd.ttf", size if len(TITLES[i]) < 15 else size - 3))
        turned = label.rotate(-90, expand=True)   # read with the head tilted right, as English spines are
        cell.paste(Image.new("RGB", turned.size, ink), (0, top + 60), turned)
        draw.ellipse((w // 2 - 6, CELL_H - 24, w // 2 + 6, CELL_H - 12), outline=ink, width=2)
        img.paste(cell, (i * CELL_W, 0))
    a = np.asarray(img).astype(float) / 255
    wear = 0.90 + 0.10 * np.random.default_rng(81).random(a.shape[:2])
    save("Books", a * wear[..., None], 0.18)


# ---------------------------------------------------------------- the atlas of small printed things

class Atlas:
    def __init__(self, wide, high):
        self.size, self.high = wide, high
        self.img = Image.new("RGB", (wide, high), (128, 128, 128))
        self.x = self.y = self.row = 0
        self.rects = {}

    def add(self, name, picture):
        w, h = picture.size
        if self.x + w > self.size:
            self.x, self.y, self.row = 0, self.y + self.row + 2, 0
        if self.y + h > self.high:
            raise ValueError("the atlas is full at " + name)   # it would be cut off, and nothing would say so
        self.img.paste(picture, (self.x, self.y))
        s, t = float(self.size), float(self.high)
        self.rects[name] = [(self.x + 0.5) / s, 1 - (self.y + h - 0.5) / t, (self.x + w - 0.5) / s, 1 - (self.y + 0.5) / t]
        self.x += w + 2
        self.row = max(self.row, h)


def card(w, h, colour):
    img = Image.new("RGB", (w, h), colour)
    return img, ImageDraw.Draw(img)


def floppy(body, label, lines):
    img, d = card(128, 128, body)
    d.rectangle((0, 0, 127, 127), outline=tuple(int(c * 0.7) for c in body), width=2)
    d.rectangle((30, 0, 96, 40), fill=(176, 180, 186))
    d.rectangle((72, 5, 88, 34), fill=body)
    d.rectangle((14, 52, 114, 122), fill=label)
    for i, text in enumerate(lines):
        d.text((20, 58 + i * 20), text, fill=(30, 40, 110), font=font("segoepr.ttf", 14))
    d.polygon(((118, 0), (128, 0), (128, 10)), fill=(128, 128, 128))
    return img


def bay(kind):
    img, d = card(256, 72, (214, 206, 184))
    d.rectangle((0, 0, 255, 71), outline=(150, 143, 124), width=2)
    if kind == "floppy":
        d.rectangle((34, 26, 222, 38), fill=(40, 38, 36))
        d.rectangle((96, 30, 160, 44), fill=(60, 57, 52))
        d.rectangle((196, 48, 222, 60), fill=(190, 182, 160), outline=(120, 114, 100))
        d.rectangle((36, 50, 46, 56), fill=(60, 200, 80))
    elif kind == "cd":
        d.rectangle((16, 14, 240, 40), fill=(198, 190, 168), outline=(140, 133, 116), width=2)
        d.ellipse((24, 48, 40, 64), fill=(60, 58, 54))
        d.rectangle((60, 52, 70, 58), fill=(230, 160, 40))
        d.rectangle((204, 48, 236, 62), fill=(190, 182, 160), outline=(120, 114, 100))
        d.text((90, 46), "CD-ROM 4x", fill=(110, 104, 92), font=font("arialbd.ttf", 14))
    else:
        for y in range(18, 60, 8):
            d.line((20, y, 236, y), fill=(190, 182, 162), width=2)
    return img


def rack_unit(kind, seed):
    rng = np.random.default_rng(seed)
    img, d = card(512, 76, (52, 54, 58))
    d.rectangle((0, 0, 511, 75), outline=(20, 20, 22), width=2)
    for x in (10, 502):
        for y in (16, 60):
            d.ellipse((x - 5, y - 5, x + 5, y + 5), fill=(150, 152, 156))
    if kind == "drives":
        for i in range(6):
            x = 34 + i * 76
            d.rectangle((x, 12, x + 66, 64), fill=(34, 35, 38), outline=(90, 92, 98))
            d.rectangle((x + 6, 18, x + 60, 30), fill=(70, 72, 78))
            d.rectangle((x + 8, 52, x + 14, 58), fill=(60, 220, 90) if rng.random() < 0.8 else (240, 170, 40))
    elif kind == "switch":
        for i in range(24):
            x = 34 + i * 18 + (i // 8) * 10
            d.rectangle((x, 34, x + 14, 50), fill=(20, 20, 22), outline=(120, 122, 128))
            d.rectangle((x + 4, 24, x + 9, 28), fill=(60, 220, 90) if rng.random() < 0.7 else (40, 60, 40))
        d.text((34, 54), "24 PORT 10BASE-T HUB", fill=(170, 172, 178), font=font("arial.ttf", 11))
    elif kind == "vent":
        for x in range(34, 480, 9):
            d.line((x, 14, x, 62), fill=(22, 22, 24), width=4)
    else:
        d.rectangle((34, 14, 250, 62), fill=(24, 30, 26), outline=(90, 92, 98))
        d.text((44, 22), "LOAD 0.42  UP 211d", fill=(80, 230, 110), font=font("consola.ttf", 16))
        d.text((44, 42), "rv32 node %02d" % rng.integers(1, 20), fill=(80, 230, 110), font=font("consola.ttf", 14))
        for i in range(5):
            d.ellipse((290 + i * 36, 28, 314 + i * 36, 52), fill=(30, 30, 32), outline=(110, 112, 118))
        d.rectangle((470, 30, 480, 40), fill=(230, 60, 50))
    return img


def keyboard_top():
    img, d = card(512, 176, (206, 198, 176))
    rows = ["`1234567890-=", "QWERTYUIOP[]\\", "ASDFGHJKL;'", "ZXCVBNM,./"]
    for r, keys in enumerate(rows):
        x0 = 14 + r * 14 + (30 if r else 0)
        if r:
            d.rounded_rectangle((14, 10 + r * 32, x0 - 4, 38 + r * 32), 3, fill=(176, 168, 148), outline=(120, 114, 100))
        for i, key in enumerate(keys):
            x = x0 + i * 32
            d.rounded_rectangle((x, 10 + r * 32, x + 28, 38 + r * 32), 3, fill=(228, 222, 204), outline=(140, 133, 116))
            d.text((x + 6, 13 + r * 32), key, fill=(60, 56, 50), font=font("arial.ttf", 12))
        d.rounded_rectangle((x0 + len(keys) * 32, 10 + r * 32, 498, 38 + r * 32), 3, fill=(176, 168, 148), outline=(120, 114, 100))
    d.rounded_rectangle((14, 138, 120, 166), 3, fill=(176, 168, 148), outline=(120, 114, 100))
    d.rounded_rectangle((124, 138, 380, 166), 3, fill=(228, 222, 204), outline=(140, 133, 116))
    d.rounded_rectangle((384, 138, 498, 166), 3, fill=(176, 168, 148), outline=(120, 114, 100))
    return img


def game_box(title, sub, colours, seed):
    rng = np.random.default_rng(seed)
    img, d = card(168, 216, colours[0])
    for i in range(14):
        x, y, r = rng.integers(0, 168), rng.integers(60, 216), rng.integers(8, 60)
        d.ellipse((x - r, y - r, x + r, y + r), fill=tuple(int(c * rng.uniform(0.5, 1.2)) % 256 for c in colours[1]))
    d.rectangle((0, 0, 167, 52), fill=(12, 12, 14))
    d.text((84, 26), title, fill=colours[2], anchor="mm", font=font("impact.ttf", 30 if len(title) < 9 else 22))
    d.text((84, 200), sub, fill=(240, 240, 240), anchor="mm", font=font("arialbd.ttf", 11))
    d.rectangle((0, 0, 167, 215), outline=(10, 10, 10), width=2)
    return img


def printed(lines, size, w, h, paper=(212, 202, 172), ink=(60, 58, 54), face="arialbd.ttf", centre=True, frame=None):
    """Lettering as it is printed on a computer's plastic, or on a plate or label of `paper`."""
    img = Image.new("RGB", (w, h), paper)
    d = ImageDraw.Draw(img)
    if frame is not None:
        d.rectangle((1, 1, w - 2, h - 2), outline=frame, width=2)
    f = font(face, size)
    pitch = size * 1.25
    y = (h - pitch * len(lines)) / 2
    for line in lines:
        wide = d.textlength(line, font=f)
        d.text(((w - wide) / 2 if centre else 8, y), line, fill=ink, font=f)
        y += pitch
    return img


def pc_lettering(a):
    """What is printed on the classroom's computers (world/pc.py): drive and key legends in the
    plastic's own colour, badges, and the rating labels behind."""
    a.add("pc_cd", printed(["COMPACT DISC  8X"], 13, 150, 20))
    a.add("pc_badge", printed(["RVC 486", "DX2-66"], 15, 120, 48, paper=(28, 48, 110), ink=(232, 232, 236), frame=(190, 192, 198)))
    a.add("crt_brand", printed(["SHADERVISION  21"], 20, 220, 36, paper=(186, 188, 192), ink=(40, 42, 50), frame=(120, 122, 128)))
    a.add("crt_osd", printed(["RGB2/+   RGB1/-   VIDEO2   VIDEO1    EXIT   PROCEED"], 9, 290, 14))
    a.add("crt_rear", printed(["SHADERVISION 21  COLOUR MONITOR", "MODEL SV-2196   100-240 V~  2.0 A  50/60 Hz", "SERIAL 96-1017-00428", "",
                               "CAUTION: HIGH VOLTAGE INSIDE.", "NO USER SERVICEABLE PARTS.", "MADE IN TAIWAN"], 12, 330, 180,
                              paper=(214, 214, 208), ink=(30, 30, 30), centre=False, frame=(60, 60, 60)))
    a.add("kb_lamps", printed(["Num      Caps     Scroll"], 9, 110, 14))
    # a loudspeaker's grille: holes punched in the moulding over an oval speaker of 9 x 5.5 cm
    img = Image.new("RGB", (216, 132), (212, 202, 172))
    d = ImageDraw.Draw(img)
    for row in range(16):
        for col in range(27):
            x, y = 4 + col * 8 + (4 if row % 2 else 0), 6 + row * 8
            if ((x - 108) / 104.0) ** 2 + ((y - 66) / 62.0) ** 2 < 1:
                d.ellipse((x - 2.4, y - 2.4, x + 2.4, y + 2.4), fill=(34, 32, 30))
    a.add("crt_speaker", img)
    # a legend a control or socket (world/pc_faces.py): pc.py puts each at its own control's place
    import pc_faces
    f = font("arialbd.ttf", pc_faces.LEGEND_PX)
    for name, (text, tone) in pc_faces.LEGENDS.items():
        paper, ink = pc_faces.TONES[tone]
        wide = int(ImageDraw.Draw(Image.new("RGB", (4, 4))).textlength(text, font=f)) + 6
        img = Image.new("RGB", (wide, pc_faces.LEGEND_PX + 6), paper)
        ImageDraw.Draw(img).text((wide / 2, pc_faces.LEGEND_PX / 2 + 3), text, fill=ink, anchor="mm", font=f)
        a.add("lg_" + name, img)


def atlas():
    a = Atlas(2048, 2048)
    a.add("keyboard", keyboard_top())
    a.add("rack_drives", rack_unit("drives", 1))
    a.add("rack_switch", rack_unit("switch", 2))
    a.add("rack_vent", rack_unit("vent", 3))
    a.add("rack_panel", rack_unit("panel", 4))
    for name, kind in (("bay_floppy", "floppy"), ("bay_cd", "cd"), ("bay_blank", "blank")):
        a.add(name, bay(kind))
    a.add("floppy_black", floppy((38, 38, 42), (236, 232, 220), ["LINUX", "boot 1/2"]))
    a.add("floppy_blue", floppy((44, 78, 150), (240, 240, 236), ["RV32", "tools"]))
    a.add("floppy_red", floppy((160, 44, 40), (240, 236, 224), ["SAVES", "do not", "format"]))
    a.add("floppy_grey", floppy((150, 150, 146), (250, 246, 210), ["WADs", "backup"]))
    boxes = [("HELLGATE", "3D ACTION  *  SHAREWARE", ((120, 30, 20), (230, 120, 30), (250, 210, 60))),
             ("RED DAWN", "REAL-TIME STRATEGY", ((40, 44, 40), (170, 40, 36), (240, 240, 240))),
             ("TREMOR", "TRUE 3D  *  NEEDS A MATH CHIP", ((60, 44, 30), (120, 96, 60), (220, 170, 90))),
             ("CUBELAND", "BUILD ANYTHING", ((70, 140, 200), (90, 170, 80), (255, 255, 255))),
             ("GEARS", "OPENGL DEMO DISK", ((30, 30, 60), (200, 60, 60), (120, 220, 120)))]
    for i, (title, sub, colours) in enumerate(boxes):
        a.add("game_%d" % i, game_box(title, sub, colours, 90 + i))
    for i, colour in enumerate(((246, 232, 120), (244, 170, 190), (160, 220, 240))):
        img, d = card(72, 72, colour)
        for j, text in enumerate((["call", "Linus"], ["0x8700", "0000"], ["mount", "/dev/fd0"])[i]):
            d.text((8, 12 + j * 24), text, fill=(40, 40, 90), font=font("segoepr.ttf", 13))
        a.add("note_%d" % i, img)
    img, d = card(160, 52, (196, 198, 202))
    d.rectangle((0, 0, 159, 51), outline=(110, 112, 118), width=2)
    d.text((80, 18), "ShaderEmu", fill=(30, 34, 60), anchor="mm", font=font("arialbi.ttf", 22))
    d.text((80, 38), "RV32IMAF  *  SHADER INSIDE", fill=(150, 40, 40), anchor="mm", font=font("arialbd.ttf", 9))
    a.add("badge", img)
    img, d = card(96, 96, (232, 228, 214))
    d.rounded_rectangle((2, 2, 93, 93), 8, outline=(170, 166, 150), width=2)
    for y in (26, 62):
        d.rounded_rectangle((30, y, 66, y + 22), 6, fill=(214, 210, 196), outline=(120, 116, 104))
        d.rectangle((38, y + 6, 41, y + 15), fill=(40, 40, 40))
        d.rectangle((55, y + 6, 58, y + 15), fill=(40, 40, 40))
    a.add("socket", img)
    img, d = card(72, 108, (232, 228, 214))
    d.rounded_rectangle((2, 2, 69, 105), 8, outline=(170, 166, 150), width=2)
    d.rounded_rectangle((24, 30, 48, 78), 4, fill=(244, 240, 228), outline=(130, 126, 114), width=2)
    d.line((24, 54, 48, 54), fill=(170, 166, 150), width=2)
    a.add("switch", img)
    img, d = card(256, 320, (244, 240, 228))
    d.rectangle((0, 0, 255, 130), fill=(40, 70, 120))
    d.polygon(((0, 130), (70, 50), (120, 100), (170, 30), (256, 130)), fill=(230, 236, 244))
    d.ellipse((196, 14, 226, 44), fill=(250, 230, 150))
    d.text((128, 152), "OCTOBER 1996", fill=(170, 30, 30), anchor="mm", font=font("arialbd.ttf", 20))
    for i in range(31):
        x, y = 22 + ((i + 2) % 7) * 32, 178 + ((i + 2) // 7) * 26
        d.text((x, y), str(i + 1), fill=(40, 40, 40) if (i + 2) % 7 else (170, 30, 30), font=font("arial.ttf", 14))
    d.ellipse((112, 198, 140, 222), outline=(200, 40, 40), width=2)
    a.add("calendar", img)
    for i, (title, colour) in enumerate((("BYTES", (200, 50, 40)), ("PC PLAYER", (30, 60, 140)), ("KERNEL", (20, 20, 20)))):
        img, d = card(160, 210, (236, 232, 222))
        d.rectangle((0, 0, 159, 46), fill=colour)
        d.text((80, 23), title, fill=(255, 255, 255), anchor="mm", font=font("impact.ttf", 30 if len(title) < 7 else 24))
        d.rectangle((14, 60, 146, 150), fill=tuple(int(c * 0.6 + 60) for c in colour))
        d.ellipse((50, 74, 110, 134), fill=(240, 220, 120))
        for j, text in enumerate(("32 BITS FOR ALL", "Is RISC the future?", "50 shareware hits")):
            d.text((14, 158 + j * 16), text, fill=(30, 30, 30), font=font("arialbd.ttf", 11))
        a.add("magazine_%d" % i, img)
    img, d = card(128, 128, (236, 232, 220))
    d.ellipse((4, 4, 123, 123), fill=(250, 248, 240), outline=(40, 40, 40), width=5)
    for i in range(12):
        ang = i * np.pi / 6
        d.line((64 + 48 * np.sin(ang), 64 - 48 * np.cos(ang), 64 + 54 * np.sin(ang), 64 - 54 * np.cos(ang)), fill=(30, 30, 30), width=3)
    d.line((64, 64, 92, 46), fill=(20, 20, 20), width=4)
    d.line((64, 64, 60, 22), fill=(20, 20, 20), width=3)
    a.add("clock", img)
    img, d = card(140, 190, (248, 247, 240))
    rng = np.random.default_rng(95)
    for i in range(20):
        d.line((10 + 8 * rng.integers(0, 3), 12 + i * 8, 40 + rng.integers(20, 90), 12 + i * 8), fill=(70, 70, 80), width=2)
    for y in range(6, 190, 16):
        d.ellipse((2, y, 6, y + 4), fill=(200, 200, 196))
    a.add("listing", img)
    for i, text in enumerate(("FRAGILE", "CABLES", "OLD DRIVES")):
        img, d = card(128, 60, (240, 236, 222))
        d.rectangle((0, 0, 127, 59), outline=(180, 40, 30), width=3)
        d.text((64, 30), text, fill=(180, 40, 30), anchor="mm", font=font("impact.ttf", 24 if len(text) < 8 else 19))
        a.add("label_%d" % i, img)
    img, d = card(64, 16, (196, 160, 104))
    a.add("tape", img)
    img, d = card(128, 128, (212, 204, 182))
    for y in range(12, 120, 10):
        d.rounded_rectangle((14, y, 114, y + 4), 2, fill=(54, 50, 44))
    a.add("vent", img)
    img, d = card(140, 116, (30, 60, 120))
    d.rectangle((4, 4, 135, 111), outline=(240, 240, 240), width=2)
    d.text((70, 58), "RISC-V", fill=(240, 200, 60), anchor="mm", font=font("impact.ttf", 30))
    a.add("mousepad", img)
    pc_lettering(a)
    arr = np.asarray(a.img).astype(float) / 255
    save("Details", arr, 0.25)
    # what is a lamp in those pictures: the LEDs and lit read-outs of the rack and the drives
    lamps = np.array([(60, 220, 90), (240, 170, 40), (80, 230, 110), (230, 60, 50), (60, 200, 80), (230, 160, 40)]) / 255.0
    lit = np.zeros(arr.shape[:2], bool)
    for colour in lamps:
        lit |= np.abs(arr - colour).max(-1) < 0.06
    inside = np.zeros(arr.shape[:2], bool)
    for name, (u0, v0, u1, v1) in a.rects.items():
        if name.startswith(("rack_", "bay_")):
            inside[int((1 - v1) * a.high):int((1 - v0) * a.high) + 1, int(u0 * a.size):int(u1 * a.size) + 1] = True
    save("Details_e", arr * (lit & inside)[..., None])
    print("atlas: %d lit pixels" % (lit & inside).sum())
    json.dump(a.rects, open(os.path.join(HERE, "atlas.json"), "w"), indent=1)


def prop_screen():
    img, d = card(256, 192, (4, 10, 6))
    lines = ["ShaderEmu BIOS v0.9", "RV32IMAF  128 MB", "", "/ # uname -a", "Linux rvc 5.17", "/ # doom &", "[1] 42", "/ # _"]
    for i, text in enumerate(lines):
        d.text((10, 8 + i * 22), text, fill=(70, 240, 110), font=font("consola.ttf", 18))
    save("PropScreen", np.asarray(img).astype(float) / 255)


# ---------------------------------------------------------------- the city

def city_windows(name, seed, kind):
    cells, n = 16, 1024
    c = n // cells
    rng = np.random.default_rng(seed)
    wall = np.array({"office": (0.060, 0.064, 0.080), "flats": (0.085, 0.072, 0.064), "glass": (0.030, 0.042, 0.062)}[kind])
    img = np.zeros((n, n, 3))
    img[:] = wall * (0.9 + 0.2 * rng.random((n, n, 1)))
    yy, xx = np.mgrid[0:c, 0:c]
    floor_on = rng.random(cells)
    for row in range(cells):
        img[row * c:row * c + 3] *= 1.5   # a floor's edge catches some light
        for col in range(cells):
            busy = floor_on[row] > 0.58
            chance = {"office": 0.14 + 0.55 * busy, "flats": 0.28, "glass": 0.20 + 0.55 * busy}[kind]
            lit = rng.random() < chance
            warm = rng.random() < {"office": 0.4, "flats": 0.85, "glass": 0.2}[kind]
            colour = np.array((1.0, 0.72, 0.40)) if warm else np.array((0.70, 0.86, 1.0))
            if kind == "office":
                x0, x1, y0, y1 = 6, c - 6, 12, c - 8
            elif kind == "flats":
                x0, x1, y0, y1 = 16, c - 16, 14, c - 16
            else:
                x0, x1, y0, y1 = 1, c - 1, 10, c - 6
            pane = np.zeros((c, c, 3))
            if lit:
                # brighter towards the lamps on the ceiling, dimmer at the sill
                fall = 0.55 + 0.45 * (1 - (yy - y0) / (y1 - y0))
                pane[:] = colour * rng.uniform(0.45, 1.0) * fall[..., None]
                style = rng.random()
                if style < 0.25:      # a blind part of the way down
                    drop = y0 + int((y1 - y0) * rng.uniform(0.25, 0.7))
                    pane[:drop] *= np.where((yy[:drop] % 3 == 0)[..., None], 0.35, 0.6)
                elif style < 0.45 and kind != "glass":   # curtains at the sides
                    wide = int((x1 - x0) * rng.uniform(0.15, 0.3))
                    pane[:, x0:x0 + wide] *= 0.45
                    pane[:, x1 - wide:x1] *= 0.45
                elif style < 0.55:    # a screen, blue, and little else
                    pane[:] = np.array((0.25, 0.40, 0.75)) * rng.uniform(0.3, 0.6) * fall[..., None]
            else:
                pane[:] = np.array((0.022, 0.030, 0.050)) * rng.uniform(0.6, 1.8)
            cell = img[row * c:(row + 1) * c, col * c:(col + 1) * c]
            cell[y0:y1, x0:x1] = pane[y0:y1, x0:x1]
            if kind != "glass":   # a frame, and a bar across
                cell[y0:y1, (x0 + x1) // 2 - 1:(x0 + x1) // 2 + 1] = wall * 0.8
                cell[y1:y1 + 2, x0 - 2:x1 + 2] = wall * 1.8
            else:
                cell[y0:y1, c // 2 - 1:c // 2 + 1] *= 0.5
    save(name, img)


def city_street():
    n = 1024
    h = n // 2
    img = np.zeros((n, n, 3))
    rng = np.random.default_rng(120)
    y, x = np.mgrid[0:h, 0:h]
    u, v = (x + 0.5) / h, (y + 0.5) / h
    edge = np.maximum(np.abs(u - 0.5), np.abs(v - 0.5))
    for tile in range(4):
        t = np.zeros((h, h, 3))
        road = edge > 0.40
        walk = (edge > 0.36) & ~road
        t[:] = ((0.050, 0.050, 0.058), (0.030, 0.050, 0.036), (0.060, 0.056, 0.052), (0.045, 0.045, 0.060))[tile]
        t[walk] = (0.13, 0.12, 0.11)
        t[road] = (0.085, 0.075, 0.068)
        lane = (edge > 0.448) & (edge < 0.452) & ((np.maximum(u, v) * 24) % 1.0 < 0.5)
        t[lane] = (0.30, 0.27, 0.20)

        def glow(cx, cy, radius, colour, power=2.0):
            d = np.hypot(u - cx, v - cy) / radius
            t[:] += np.clip(1 - d, 0, 1)[..., None] ** power * np.array(colour)
        for i in range(8):   # street lamps on the pavement
            s = (i + 0.5) / 8
            for cx, cy in ((s, 0.115), (s, 0.885), (0.115, s), (0.885, s)):
                glow(cx, cy, 0.075, (1.0, 0.62, 0.28))
        for i in range(rng.integers(8, 16)):   # cars: white ahead, red behind
            s, side = rng.random(), rng.integers(0, 4)
            off = 0.035 if rng.random() < 0.5 else 0.07
            cx, cy = ((s, off), (s, 1 - off), (off, s), (1 - off, s))[side]
            glow(cx, cy, 0.018, (1.0, 0.95, 0.85) if rng.random() < 0.5 else (0.9, 0.08, 0.05), 1.2)
        if tile == 1:   # a park: lamps along its paths
            for i in range(6):
                glow(0.2 + 0.12 * i, 0.5 + 0.2 * np.sin(i), 0.03, (0.8, 0.9, 0.7))
        if tile == 2:   # a car park
            for i in range(5):
                for j in range(5):
                    glow(0.22 + 0.14 * i, 0.22 + 0.14 * j, 0.03, (0.9, 0.85, 0.7))
        ty, tx = divmod(tile, 2)
        img[ty * h:(ty + 1) * h, tx * h:(tx + 1) * h] = t
    save("CityStreet", img)


def night_sky():
    """A panorama for the skybox: u = 0.5 looks along +x (out of the window), v = 1 straight up."""
    w, h = 4096, 2048
    y, x = np.mgrid[0:h, 0:w]
    angle = (0.5 - (x + 0.5) / w) * 2 * np.pi
    up = np.cos((y + 0.5) / h * np.pi)   # row 0 is the top
    flat = np.sqrt(np.clip(1 - up * up, 0, 1))
    d = np.stack([np.cos(angle) * flat, up, np.sin(angle) * flat], -1)
    t = np.clip(up * 1.5 + 0.08, 0, 1)[..., None]
    sky = (1 - t) * np.array((0.085, 0.105, 0.20)) + t * np.array((0.004, 0.006, 0.018))
    glow = np.clip(1 - np.abs(up) * 5.0, 0, 1)[..., None] ** 2.5
    sky += glow * np.array((0.26, 0.15, 0.10)) * (0.7 + 0.3 * np.cos(angle))[..., None]   # the city on the haze
    moon = np.array((0.925, 0.33, 0.19))
    moon /= np.linalg.norm(moon)
    m = d @ moon
    # stars first: clouds and the moon go over them
    rng = np.random.default_rng(5)
    stars = np.zeros((h, w))
    count = 9000
    sy = (np.arccos(rng.random(count)) / np.pi * h).astype(int)
    sx = rng.integers(0, w, count)
    stars[np.clip(sy, 0, h - 1), sx] = rng.random(count) ** 4 * 0.9 + 0.08
    sky += stars[..., None] * np.array((1.0, 1.0, 1.08)) * np.clip(up * 6, 0, 1)[..., None]
    # clouds: broken bands low in the sky, lit from below by the city and at their edges by the moon
    big = np.zeros((h, w))
    for k, (beta, seed, sx_, sy_) in enumerate(((2.2, 200, 1.0, 3.0), (1.6, 201, 1.0, 2.0))):
        tile = noise(1024, beta, seed, stretch=(sx_, sy_))
        big += np.tile(tile, (2, 4))[:h, :w] * (0.7 if k == 0 else 0.3)
    cover = np.clip((big - 0.25) * 0.9, 0, 1) * np.clip(1 - np.abs(up - 0.22) / 0.42, 0, 1) ** 0.8
    lit = (np.array((0.20, 0.13, 0.10)) * np.clip(1 - up * 2.4, 0.15, 1)[..., None]
           + np.array((0.16, 0.18, 0.24)) * np.clip(m, 0, 1)[..., None] ** 24)
    sky = sky * (1 - 0.85 * cover[..., None]) + lit * cover[..., None]
    halo = np.clip(m, 0, 1)
    sky += np.array((0.20, 0.21, 0.26)) * (halo ** 400)[..., None] + np.array((0.05, 0.055, 0.07)) * (halo ** 40)[..., None]
    # the disc itself is geometry, bright enough to bloom (world/city.py)
    deep = np.clip(-up * 3, 0, 1)[..., None]
    ground = (1 - deep) * np.array((0.11, 0.085, 0.10)) + deep * np.array((0.01, 0.01, 0.015))
    sky = np.where((up < 0)[..., None], ground, sky)
    save("NightSky", np.clip(sky, 0, 1) ** (1 / 2.2))


def cloud_noise():
    n = 1024
    billow = 0.6 * noise(n, 2.4, 300) + 0.3 * noise(n, 1.6, 301) + 0.1 * noise(n, 1.0, 302)
    save("CloudNoise", np.repeat(unit(billow)[..., None], 3, -1))


def rain_streaks():
    """Thin streaks, falling straight; the picture joins itself on every side."""
    w, h = 512, 1024
    rng = np.random.default_rng(310)
    img = np.zeros((h, w))
    rows = np.arange(h)
    for _ in range(420):
        x, top, length = rng.integers(0, w), rng.integers(0, h), rng.integers(50, 190)
        level = rng.uniform(0.15, 1.0) ** 2
        along = (rows - top) % h
        streak = np.abs(np.sin(np.pi * np.minimum(along, length) / length)) ** 0.6 * level
        img[:, x] = np.maximum(img[:, x], streak)
        img[:, (x + 1) % w] = np.maximum(img[:, (x + 1) % w], streak * 0.35)
    save("RainStreaks", np.repeat(img[..., None], 3, -1))


def ui():
    """What the panels are drawn with (ShaderEmuRetro.cs): bevels of a desktop of the nineties,
    a keycap, and fanfold paper. The bevels and the cap are stretched by their middles."""
    out = os.path.join(OUT, "..", "UI")
    os.makedirs(out, exist_ok=True)

    def bevel(name, lit, dark, lit2, dark2, face):
        n = 48
        a = np.full((n, n, 3), face, float)
        for inset, (top, low) in enumerate(((lit, dark), (lit2, dark2))):
            a[inset, inset:n - inset] = top
            a[inset:n - inset, inset] = top
            a[n - 1 - inset, inset:n - inset] = low
            a[inset:n - inset, n - 1 - inset] = low
        Image.fromarray((a * 255 + 0.5).astype(np.uint8)).save(os.path.join(out, name + ".png"))
    bevel("Raised", 1.0, 0.0, 0.87, 0.5, 0.753)
    bevel("Sunken", 0.5, 1.0, 0.0, 0.87, 1.0)

    # a keycap seen from above: the skirt falling away darker, the dished top lighter
    n = 64
    y, x = np.mgrid[0:n, 0:n]
    edge = np.minimum(np.minimum(x, n - 1 - x), np.minimum(y, n - 1 - y)).astype(float)
    corner = np.hypot(np.clip(9 - np.minimum(x, n - 1 - x), 0, None), np.clip(9 - np.minimum(y, n - 1 - y), 0, None))
    inside = corner <= 9
    top_face = (np.minimum(np.minimum(x - 9, n - 10 - x), np.minimum(y - 6, n - 14 - y)) >= 0)
    shade = np.where(top_face, 1.0 - 0.05 * (y / n), 0.78 - 0.10 * (y / n) + 0.02 * edge)
    shade = np.where(edge < 1.5, 0.42, shade)
    rgba = np.dstack([np.repeat(np.clip(shade, 0, 1)[..., None], 3, -1), inside.astype(float)])
    Image.fromarray((rgba * 255 + 0.5).astype(np.uint8)).save(os.path.join(out, "Keycap.png"))

    # fanfold listing paper: green bars, tractor holes down both edges
    w, h = 1150, 850
    paper = np.empty((h, w, 3))
    paper[:] = (0.955, 0.945, 0.895)
    rows = np.arange(h)[:, None]
    bars = ((rows // 50) % 2 == 0) * np.ones((1, w), bool)
    paper[bars] = (0.875, 0.925, 0.865)
    paper *= (0.985 + 0.015 * np.random.default_rng(400).random((h, w, 1)))
    yy, xx = np.mgrid[0:h, 0:w]
    for side in (11, w - 12):
        paper[:, side + (11 if side < 100 else -11)] *= 0.86          # the tear line
        holes = np.hypot(xx - side, (yy % 25) - 12) < 4.2
        paper[holes] = (0.20, 0.19, 0.18)
    Image.fromarray((np.clip(paper, 0, 1) * 255 + 0.5).astype(np.uint8)).save(os.path.join(out, "Paper.png"))


def keyboard():
    """world/keyboard.json is the builder's layout (a key: x, y, width, height from the panel's top
    left, in millimetres, and its legend). The caps of world/computer.py take this picture from above.
    The legends themselves are the panel's lettering, not part of it."""
    layout = json.load(open(os.path.join(HERE, "keyboard.json"), encoding="utf-8"))
    wide, high, scale = layout["width"], layout["height"], 3
    img = Image.new("RGB", (int(wide * scale), int(high * scale)), (38, 36, 33))
    draw = ImageDraw.Draw(img)
    for x, y, w, h, legend in layout["keys"]:
        named = (sum(c.isalpha() for c in legend) >= 2 and " " not in legend.strip()) or (len(legend) > 1 and legend[0] == "F" and legend[1].isdigit())
        named = named or legend.startswith("Use my")
        cap = (168, 163, 148) if named else (229, 222, 199)
        draw.rectangle((x * scale, y * scale, (x + w) * scale, (y + h) * scale), fill=cap)
    img = img.resize((4096, 2048), Image.LANCZOS)
    a = np.asarray(img).astype(float) / 255
    save("Keyboard", a * (0.96 + 0.04 * np.random.default_rng(500).random(a.shape[:2]))[..., None], 0.32)


def smudges():
    """The film on a tube's glass, for CRT.shader: dust wiped in streaks over the whole face. It is
    the television's own (Poly Haven's television_02, CC0): its screen's patch of its colour map,
    and in alpha that patch of its roughness (0 its smoothest, 1 its dullest)."""
    folder = os.path.join(HERE, "..", "build", "polyhaven", "television_02", "textures")
    box = (722, 28, 996, 228)   # the screen, in from its bezel
    colour = Image.open(os.path.join(folder, "television_02_diff_1k.jpg")).convert("RGB").crop(box)
    rough = np.asarray(Image.open(os.path.join(folder, "television_02_arm_1k.jpg")).crop(box))[..., 1].astype(float) / 255
    low, high = np.percentile(rough, 1), np.percentile(rough, 99.5)
    print("smudges: the television's screen is rough %.2f to %.2f (mean %.2f)" % (low, high, rough.mean()))
    alpha = Image.fromarray((np.clip((rough - low) / (high - low), 0, 1) * 255 + 0.5).astype(np.uint8))
    film = colour.resize((1024, 768), Image.BICUBIC)
    film.putalpha(alpha.resize((1024, 768), Image.BICUBIC))
    film.save(os.path.join(OUT, "Smudges.png"))


def holo_grid():
    """A metre of the holodeck's wall: black, with a bright line along its edges (half of it on each tile)."""
    n = 256
    y, x = np.mgrid[0:n, 0:n]
    edge = np.minimum(np.minimum(x, n - 1 - x), np.minimum(y, n - 1 - y))
    line = np.clip(1.5 - edge / 2.0, 0, 1)
    base = 0.035 + 0.01 * unit(noise(n, 1.5, 81))
    rgb = base[..., None] * np.array([1.0, 1.0, 1.05]) * (1 - line[..., None]) + line[..., None] * np.array([0.95, 0.72, 0.12])
    save("HoloGrid", rgb, 0.55 - 0.25 * line)
    save("HoloGrid_e", line[..., None] * np.array([1.0, 0.72, 0.10]))


def carpet():
    """A corridor's carpet: short loops in dark red, a small repeat woven into it."""
    n = 1024
    y, x = np.mgrid[0:n, 0:n]
    pile = 0.75 + 0.25 * unit(noise(n, 0.4, 82))
    k = n // 8
    diamond = (np.abs((x % k) - k / 2) + np.abs((y % k) - k / 2)) / k
    motif = (np.abs(diamond - 0.28) < 0.035) | (diamond < 0.07)
    rgb = np.where(motif[..., None], np.array([0.42, 0.32, 0.16]), np.array([0.30, 0.07, 0.08])) * pile[..., None]
    rgb *= (0.92 + 0.08 * unit(noise(n, 2.2, 83)))[..., None]
    save("Carpet", rgb, 0.03)
    save("Carpet_n", normal_map(pile, 0.0005))


def main():
    os.makedirs(OUT, exist_ok=True)
    # walls, floor, wood and cloth are Poly Haven's (world/fetch.py)
    for make in (plastic, metal, cardboard, rug, round_rug, books, atlas, prop_screen, city_street, night_sky, cloud_noise, rain_streaks, ui, keyboard, holo_grid, carpet, smudges):
        make()
    city_windows("CityOffice", 100, "office")
    city_windows("CityFlats", 101, "flats")
    city_windows("CityGlass", 102, "glass")
    save("White", np.ones((8, 8, 3)))


if __name__ == "__main__":
    main()
