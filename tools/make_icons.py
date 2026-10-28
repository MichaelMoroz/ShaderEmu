"""Makes the desktop's icons and the pictures of its two looks for the Linux image:

  icons16.ppm, icons32.ppm   the classic look's sheets of icons (Chicago95's), 16 cells a row; a
                             pixel of the key colour (magenta) is not drawn (GR_IMAGE_CELL, docs/system/nanox.md)
  icons.idx                  "name cell" lines: which cell is whose (linux/apps/icons.txt)
  icons7-16.ppm, icons7-32.ppm, icons7-48.ppm
                             the Windows 7 look's sheets (Oxygen's, linux/apps/icons7.txt, icons7.idx)
  icons7-16.rgba, ...        the same with their alpha (below): the screen driver draws a cell of
                             a sheet that has such a twin blended, edges and shadows
  caption-on.ppm, caption-off.ppm
                             256 x 1: the classic caption's colours from its left end to its right
  look7.ppm, look7.rgba      the Windows 7 look's pictures, a sheet of 24-pixel cells: the
                             caption's buttons, the Start button (LOOK7 below)
  caption7-on.ppm, caption7-off.ppm, bar7.ppm
                             1 x N strips stretched over a caption and over the bar

all in build/images/linux/root/usr/share. The icons are Chicago95's (GPL-3.0+/MIT,
https://github.com/grassmunk/Chicago95) and Oxygen's (KDE, LGPL-3.0, https://github.com/KDE/oxygen-icons),
fetched once into build/fetch at the commits below, and the pictures in linux/apps/icons (its
README says whose) for what a set lacks.

An .rgba file: "SEI1", then width, height and the cell's size as 32-bit words, then a word a
pixel, rows top to bottom: (255 - alpha) << 24 | red << 16 | green << 8 | blue, the GPU's word
texture (docs/machine/gpu.md, fragment mode 1); a clear pixel is 0xff000000, the key it is drawn with.

    python tools/make_icons.py
"""
import os, struct, subprocess, sys
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# (ICONS_OUT: another folder, for an image of one's own: tools/private_image.py)
OUT = os.environ.get('ICONS_OUT') or os.path.join(ROOT, 'build', 'images', 'linux', 'root', 'usr', 'share')
LIST = os.path.join(ROOT, 'linux', 'apps', 'icons.txt')
LIST7 = os.path.join(ROOT, 'linux', 'apps', 'icons7.txt')
OURS = os.path.join(ROOT, 'linux', 'apps', 'icons')
FETCH = os.path.join(ROOT, 'build', 'fetch', 'Oxygen')
REPO, COMMIT = 'https://github.com/KDE/oxygen-icons', '2a6066659ab6621bc325a4f7735cb78b57dbfbcb'   # v6.30.0
SIZES = (16, 32, 48)
FETCH95 = os.path.join(ROOT, 'build', 'fetch', 'Chicago95')
REPO95, COMMIT95 = 'https://github.com/grassmunk/Chicago95', '5da19b8b1e2a886ebf6628403023d5fac3acc3ee'
FOLDERS95 = ['apps', 'places', 'mimes', 'devices', 'categories', 'status']
SIZES95 = (16, 32)
KEY = (255, 0, 255)
ACROSS = 16
CLEAR = 0xff000000


def fetch():
    """The theme's 16, 32 and 48 pixel icons only: a sparse checkout of one commit."""
    if os.path.isdir(os.path.join(FETCH, '48x48', 'apps')):
        return FETCH
    os.makedirs(os.path.dirname(FETCH), exist_ok=True)
    git = lambda *a, **k: subprocess.run(['git'] + list(a), check=True, **k)
    git('clone', '-q', '--filter=blob:none', '--sparse', '--no-checkout', REPO, FETCH)
    git('sparse-checkout', 'set', *['%dx%d' % (s, s) for s in SIZES], cwd=FETCH)
    git('checkout', '-q', COMMIT, cwd=FETCH)
    return FETCH


def fetch95():
    """Chicago95's 16 and 32 pixel icons only: a sparse checkout of one commit."""
    theme = os.path.join(FETCH95, 'Icons', 'Chicago95')
    git = lambda *a, **k: subprocess.run(['git'] + list(a), check=True, **k)
    if not os.path.isdir(os.path.join(theme, 'apps', '32')):
        os.makedirs(os.path.dirname(FETCH95), exist_ok=True)
        git('clone', '-q', '--filter=blob:none', '--sparse', '--no-checkout', REPO95, FETCH95)
        git('checkout', '-q', COMMIT95, cwd=FETCH95)
    if not os.path.isdir(os.path.join(theme, 'status', '32')):
        git('sparse-checkout', 'set', *['Icons/Chicago95/%s/%d' % (f, s) for f in FOLDERS95 for s in SIZES95], cwd=FETCH95)
    return theme


def find95(theme, source, size):
    if '/' in source:
        folder, name = source.split('/')
        path = os.path.join(theme, folder, str(size), name + '.png')
    else:
        path = os.path.join(OURS, source + '.png')
    return path if os.path.exists(path) else None


def find(theme, source, size):
    if '/' in source:
        path = os.path.join(theme, '%dx%d' % (size, size), *source.split('/')) + '.png'
    else:
        path = os.path.join(OURS, source + '.png')   # one picture, brought down to the size
    return path if os.path.exists(path) else None


def picture(path, size):
    image = Image.open(path).convert('RGBA')
    return image if image.size == (size, size) else image.resize((size, size), Image.LANCZOS)


def keyed(image):
    """RGB with the key colour where the picture is more clear than not."""
    out = Image.new('RGB', image.size, KEY)
    pixels, to = image.load(), out.load()
    for y in range(image.size[1]):
        for x in range(image.size[0]):
            r, g, b, a = pixels[x, y]
            if a >= 128:
                to[x, y] = (254, 0, 254) if (r, g, b) == KEY else (r, g, b)
    return out


def words(image):
    """The GPU's words of an RGBA picture (see the top)."""
    out = bytearray()
    for r, g, b, a in (image.get_flattened_data() if hasattr(image, "get_flattened_data") else image.getdata()):
        out += struct.pack('<I', CLEAR if a == 0 else (255 - a) << 24 | r << 16 | g << 8 | b)
    return bytes(out)


def save_sheet(name, cells, size):
    """A sheet of RGBA cells as NAME.ppm (keyed) and NAME.rgba (with its alpha)."""
    sheet = Image.new('RGBA', (ACROSS * size, ((len(cells) + ACROSS - 1) // ACROSS) * size), (0, 0, 0, 0))
    for n, cell in enumerate(cells):
        sheet.paste(cell, (n % ACROSS * size, n // ACROSS * size))
    keyed(sheet).save(os.path.join(OUT, name + '.ppm'))
    with open(os.path.join(OUT, name + '.rgba'), 'wb') as out:
        out.write(b'SEI1' + struct.pack('<III', sheet.size[0], sheet.size[1], size) + words(sheet))


def gradient(left, right):
    strip = Image.new('RGB', (256, 1))
    strip.putdata([tuple((left[k] * (255 - x) + right[k] * x + 127) // 255 for k in range(3)) for x in range(256)])
    return strip


def vertical(stops, height):
    """A 1 x height strip through colours at fractions of it: [(0.0, rgb), ..., (1.0, rgb)]."""
    strip = Image.new('RGB', (1, height))
    for y in range(height):
        t = (y + 0.5) / height
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            if t0 <= t <= t1:
                f = (t - t0) / (t1 - t0) if t1 > t0 else 0
                strip.putpixel((0, y), tuple(int(c0[k] + (c1[k] - c0[k]) * f + 0.5) for k in range(3)))
                break
    return strip


# The Windows 7 look's cells (24 x 24 each, in this order): the caption's buttons are 18 x 18 at
# a cell's top left, the Start button fills its cell. The window manager and the bar name them.
LOOK7 = ['min', 'max', 'restore', 'close', 'min-off', 'max-off', 'restore-off', 'close-off', 'start', 'start-down']
LOOK7_CELL = 24
BUTTON = 18


def rounded(size, radius, top, bottom, edge, glyph=None, glyph_colour=(30, 40, 60)):
    """A button face drawn four times as large and brought down: a vertical gradient in a
    rounded rectangle with an edge, a lighter top half (the glass), and a glyph."""
    k = 4
    big = Image.new('RGBA', (size * k, size * k), (0, 0, 0, 0))
    face = Image.new('RGBA', big.size)
    face.paste(vertical([(0, top), (0.5, tuple((a + b) // 2 for a, b in zip(top, bottom))), (0.5, bottom), (1, top)], size * k)
               .resize(big.size).convert('RGBA'))
    mask = Image.new('L', big.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size * k - 1, size * k - 1), radius * k, fill=255)
    big.paste(face, (0, 0), mask)
    draw = ImageDraw.Draw(big)
    draw.rounded_rectangle((0, 0, size * k - 1, size * k - 1), radius * k, outline=edge + (255,), width=k)
    draw.rounded_rectangle((k, k, size * k - 1 - k, size * k - 1 - k), max(1, radius - 1) * k, outline=(255, 255, 255, 110), width=k)
    s = size * k
    if glyph == 'min':
        draw.rectangle((s * 0.30, s * 0.62, s * 0.70, s * 0.74), fill=glyph_colour + (255,))
    elif glyph == 'max':
        draw.rectangle((s * 0.28, s * 0.28, s * 0.72, s * 0.72), outline=glyph_colour + (255,), width=int(k * 1.5))
        draw.rectangle((s * 0.28, s * 0.28, s * 0.72, s * 0.38), fill=glyph_colour + (255,))
    elif glyph == 'restore':
        draw.rectangle((s * 0.40, s * 0.22, s * 0.78, s * 0.58), outline=glyph_colour + (255,), width=int(k * 1.5))
        draw.rectangle((s * 0.22, s * 0.42, s * 0.60, s * 0.78), fill=top + (255,), outline=glyph_colour + (255,), width=int(k * 1.5))
    elif glyph == 'close':
        w = int(k * 2.6)
        draw.line((s * 0.30, s * 0.30, s * 0.70, s * 0.70), fill=glyph_colour + (255,), width=w)
        draw.line((s * 0.30, s * 0.70, s * 0.70, s * 0.30), fill=glyph_colour + (255,), width=w)
    return big.resize((size, size), Image.LANCZOS)


def look7_cells(theme):
    cells = []
    blue = dict(top=(236, 244, 252), bottom=(196, 216, 238), edge=(92, 116, 150))
    grey = dict(top=(245, 247, 250), bottom=(222, 228, 236), edge=(150, 160, 176))
    red = dict(top=(236, 158, 140), bottom=(199, 69, 50), edge=(120, 40, 30))
    red_off = dict(top=(240, 200, 192), bottom=(214, 150, 140), edge=(160, 110, 100))
    for faces, dark in ((blue, (30, 40, 60)), (grey, (120, 128, 140))):
        for glyph in ('min', 'max', 'restore'):
            cells.append(rounded(BUTTON, 3, glyph=glyph, glyph_colour=dark, **faces))
        cells.append(rounded(BUTTON, 3, glyph='close', glyph_colour=(255, 255, 255), **(red if faces is blue else red_off)))
    # the Start button: the theme's own round picture, and brighter while its menu is open
    orb = picture(find(theme, 'places/start-here', 48), 48).resize((LOOK7_CELL, LOOK7_CELL), Image.LANCZOS)
    cells.append(orb)
    lit = Image.eval(orb.convert('RGB'), lambda v: min(255, v + 40)).convert('RGBA')
    lit.putalpha(orb.getchannel('A'))
    cells.append(lit)
    out = []
    for cell in cells:
        full = Image.new('RGBA', (LOOK7_CELL, LOOK7_CELL), (0, 0, 0, 0))
        full.paste(cell, (0, 0))
        out.append(full)
    return out


def listed(path, theme, finder, sizes):
    """A list's rows: (names, the first source every size has)."""
    rows = []
    for line in open(path):
        words_ = line.split('#')[0].split()
        if not words_:
            continue
        for source in words_[1:]:
            if all(finder(theme, source, s) for s in sizes):
                rows.append((words_[0].split(','), source))
                break
        else:
            raise SystemExit('%s: none of %s exists for %s' % (os.path.basename(path), ' '.join(words_[1:]), words_[0]))
    return rows


def save_index(name, rows):
    with open(os.path.join(OUT, name), 'w', newline='\n') as index:
        for n, (names, _) in enumerate(rows):
            for one in names:
                index.write('%s %d\n' % (one, n))


def main():
    os.makedirs(OUT, exist_ok=True)
    # the classic look: Chicago95's, keyed, as they always were
    theme95 = fetch95()
    rows = listed(LIST, theme95, find95, SIZES95)
    for size in SIZES95:
        sheet = Image.new('RGB', (ACROSS * size, ((len(rows) + ACROSS - 1) // ACROSS) * size), KEY)
        for n, (_, source) in enumerate(rows):
            sheet.paste(keyed(picture(find95(theme95, source, size), size)), (n % ACROSS * size, n // ACROSS * size))
        sheet.save(os.path.join(OUT, 'icons%d.ppm' % size))
    save_index('icons.idx', rows)
    # the Windows 7 look: Oxygen's, blended
    theme = fetch()
    rows7 = listed(LIST7, theme, find, SIZES)
    for size in SIZES:
        save_sheet('icons7-%d' % size, [picture(find(theme, source, size), size) for _, source in rows7], size)
    save_index('icons7.idx', rows7)
    # the classic caption: the navy of old to a light blue, and greys for a window without the keyboard
    gradient((10, 36, 106), (166, 202, 240)).save(os.path.join(OUT, 'caption-on.ppm'))
    gradient((128, 128, 128), (192, 192, 192)).save(os.path.join(OUT, 'caption-off.ppm'))
    # the Windows 7 look: a pale blue glass caption, a dark one for the bar (CYCAPTION 22, the bar 26)
    vertical([(0, (227, 238, 250)), (0.45, (206, 222, 242)), (0.5, (192, 211, 236)), (1, (199, 218, 241))], 22).save(
        os.path.join(OUT, 'caption7-on.ppm'))
    vertical([(0, (240, 243, 247)), (0.5, (226, 232, 240)), (1, (232, 237, 244))], 22).save(os.path.join(OUT, 'caption7-off.ppm'))
    vertical([(0, (96, 116, 146)), (0.08, (62, 82, 112)), (0.5, (40, 56, 80)), (0.52, (26, 38, 56)), (1, (32, 46, 66))], 26).save(
        os.path.join(OUT, 'bar7.ppm'))
    save_sheet('look7', look7_cells(theme), LOOK7_CELL)
    print('%d classic icons, %d of the Windows 7 look' % (len(rows), len(rows7)))


if __name__ == '__main__':
    main()
