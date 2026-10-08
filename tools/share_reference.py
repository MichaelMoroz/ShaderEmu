"""The display stream players send each other (docs/share.md), as a model in numpy.

  python tools/share_reference.py PICTURE.png [--out FOLDER]
  python tools/share_reference.py PICTURE.png --atlas ATLAS.bin [--cells MIP3.raw]   # against Unity's encoder
  python tools/share_reference.py PICTURE.png --store STORE.bin --decoded SHOWN.png  # and its decoder

Codes a picture the way ShareEncode.shader does and draws the bytes the way
ShareDecode.shader does. Prints what each step costs and how close the picture is after
it; --out writes those pictures.

The unit is a cell of 8x8 pixels, and no cell needs another: what moves beside a cell
never touches it. A 32x32 quarter (16 cells) is what a record is about, with a bit a cell
saying which of its cells the record holds.
  coarse    of a quarter: two colours of the palette (the nearest to the means of its cells
            darker and lighter than their mean) and a bit a cell saying which. 4 bytes.
            What a picture that keeps moving is seen as
  palette   256 colours (5:6:5) fitted to the cells that are moving, a step of k-means a
            capture; an entry travels when it changes
  sharp     of a cell that has stopped changing: an 8x8 DCT of Y (less grey) and the means
            of Cb and Cr, at most 64 bytes: its length, a bit saying Y has more than a
            mean, the three means, then Y's block
  bitmap    of a quarter of one or two colours, instead of all of that. When it is that
            only with colours told apart at 4 bits a channel, its shape is exact and its
            colours are not, and its cells follow sharp when there is room:
              F   one colour (5:6:5)
              T   two colours, a bit a 4x4 cell saying "mixed", a bit saying which colour
                  an unmixed cell is, then 16 bits a mixed cell
              B   two bits an 8x8 block: one colour (2 bytes), two and 64 bits (12), or four
                  4x4 cells of two colours and 16 bits each (24): only when that is exact
A DCT block: its length, a bit a group of 8 coefficients (zigzag order) that has any, a byte
of bits for each such group, a byte a coefficient that is not 0.
"""
import argparse
import os

import numpy as np
from PIL import Image

QCOLS, QROWS = 40, 24          # quarters in the atlas' numbering
QUARTERS = QCOLS * QROWS
ROW = 1024                     # bytes of a row: a quarter's row, and from row 960 its cells' row
AT_COARSE, AT_EXACT = 8, 16    # in a quarter's row
SLOT = 64                      # bytes of a cell in the cells' row
CODE_F, CODE_T, CODE_B = 3, 4, 5
NAMES = {CODE_F: 'one colour', CODE_T: 'two colours', CODE_B: 'blocks', 0: 'cells'}
ROUGH = 128                    # in the encoder's kind byte: colours told apart at 4 bits a channel
PALETTE = 256
PALETTE_ROW, STORE_PALETTE_ROW = 1927, 1920   # the palette's row in the atlas and in the store
MOVE, SEED_AT = 4, 16          # levels: an entry moves to its cells' mean when that is further
#                                than MOVE; one without cells takes a cell further than SEED_AT from its entry
KEEP_Y, KEEP_C = 50, 1         # coefficients (zigzag order) a cell's blocks may have

Q_LUMA = np.full((8, 8), 24)   # one step for every coefficient: fine detail is what is wanted
Q_CHROMA = np.array([17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99,
                     47, 66, 99, 99, 99, 99, 99, 99] + [99] * 32).reshape(8, 8)


def zigzag():
    order = []
    for s in range(15):
        for v in (range(s + 1) if s % 2 else range(s, -1, -1)):
            if s - v < 8 and v < 8:
                order.append((v, s - v))
    return order


ZIGZAG = zigzag()              # (v, u): row, column
BASIS = np.array([[np.cos((2 * x + 1) * u * np.pi / 16) for x in range(8)] for u in range(8)])
ALPHA = np.array([np.sqrt(0.5)] + [1.0] * 7)
WEIGHT = np.outer([1, 2, 2, 2, 2, 2, 2, 2], [1, 2, 2, 2, 2, 2, 2, 2])


def steps(chroma):
    """Quantiser steps [v, u] in the transform's own units (a mean is 1 a level)."""
    s = (Q_CHROMA if chroma else Q_LUMA) / (16 * np.outer(ALPHA, ALPHA))
    s[0, 0] = 1
    return s


def mips(picture, count=3):
    """The capture and its mipmaps, each rounded to bytes as a render target stores them."""
    out = [picture.astype(np.float64)]
    for _ in range(count):
        p = out[-1]
        p = (p[0::2, 0::2] + p[1::2, 0::2] + p[0::2, 1::2] + p[1::2, 1::2]) / 4
        out.append(np.floor(p + 0.5))
    return out


def ycc(rgb):
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    y = 0.299 * r + 0.587 * g + 0.114 * b
    return np.stack([y, 128 + 0.564 * (b - y), 128 + 0.713 * (r - y)], axis=-1)


def rgb_of(y, cb, cr):
    r = y + 1.402 * (cr - 128)
    b = y + 1.772 * (cb - 128)
    g = (y - 0.299 * r - 0.114 * b) / 0.587
    return np.clip(np.stack([r, g, b], axis=-1), 0, 255)


def stored(plane):
    """As a render target of bytes keeps it."""
    return np.clip(np.floor(plane + 0.5), 0, 255)


def enlarge(plane, clamp):
    """Twice the size with bilinear filtering. clamp: the samples a tile has across: nothing
    is taken from outside a sample's own tile."""
    def along(a, axis):
        a = np.moveaxis(a, axis, 0)
        n = a.shape[0]
        x = np.arange(2 * n)
        u = x / 2 - 0.25
        if clamp:
            lo = (x // 2) // clamp * clamp
            hi = lo + clamp - 1
        else:
            lo, hi = np.zeros(2 * n), np.full(2 * n, n - 1)
        u = np.clip(u, lo, hi)
        i0 = np.floor(u).astype(int)
        f = (u - i0).reshape((-1,) + (1,) * (a.ndim - 1))
        i1 = np.minimum(i0 + 1, hi).astype(int)
        return np.moveaxis(a[i0] * (1 - f) + a[i1] * f, 0, axis)
    return along(along(plane, 0), 1)


def quantise(plane, chroma, keep):
    """[H/8, W/8, 64] integers in zigzag order, -127..127; none beyond the first `keep`."""
    h, w = plane.shape
    b = plane.reshape(h // 8, 8, w // 8, 8).transpose(0, 2, 1, 3)
    c = np.einsum('vy,ijyx,ux->ijvu', BASIS, b, BASIS) / 64
    q = np.clip(np.floor(c / steps(chroma) + 0.5), -127, 127)
    out = np.stack([q[..., v, u] for v, u in ZIGZAG], axis=-1).astype(np.int32)
    out[..., keep:] = 0
    return out


def restore(q, chroma):
    """The plane [H, W] those integers stand for."""
    c = np.zeros(q.shape[:2] + (8, 8))
    s = steps(chroma) * WEIGHT
    for k, (v, u) in enumerate(ZIGZAG):
        c[..., v, u] = q[..., k] * s[v, u]
    by, bx = q.shape[:2]
    return np.einsum('vy,ijvu,ux->ijyx', BASIS, c, BASIS).transpose(0, 2, 1, 3).reshape(by * 8, bx * 8)


def block_bytes(q):
    """A block's bytes from its 64 integers; nothing when they are all 0."""
    groups, maps, values = 0, [], []
    for g in range(8):
        bits = 0
        for j in range(8):
            if q[g * 8 + j]:
                bits |= 1 << j
                values.append(int(q[g * 8 + j]) & 255)
        if bits:
            groups |= 1 << g
            maps.append(bits)
    return bytes([2 + len(maps) + len(values), groups] + maps + values) if values else b''


def scale_bytes(blocks, most=1 << 30, flags=0):
    """A scale's bytes from its blocks' integers: a bit a block that is there, then those.
    flags: more bits for the first byte (scale 3 says there which quarters are transforms)."""
    head = (len(blocks) + 7) // 8
    present, body = 0, b''
    for i, q in enumerate(blocks):
        part = block_bytes(q)
        if part and head + len(body) + len(part) <= most:
            present |= 1 << i
            body += part
    data = (present | flags).to_bytes(head, 'little') + body
    return data + b'\0' * (-len(data) % 4)


def scale_blocks(data, count):
    """The integers [count, 64] a scale's bytes hold."""
    out = np.zeros((count, 64), np.int32)
    at = (count + 7) // 8
    for i in range(count):
        if not data[i // 8] >> i % 8 & 1:
            continue
        groups, maps = data[at + 1], at + 2
        values = maps + bin(groups).count('1')
        for g in range(8):
            if groups >> g & 1:
                for j in range(8):
                    if data[maps] >> j & 1:
                        out[i, g * 8 + j] = data[values] - 256 if data[values] > 127 else data[values]
                        values += 1
                maps += 1
        at += data[at]
    return out


def pack444(rgb):
    """As pack565, of the colour's nearest at 4 bits a channel: colours that close count as one."""
    r, g, b = (np.floor(rgb[..., i] / 255 * 15 + 0.5).astype(np.int32) for i in range(3))
    return (r << 1 | r >> 3) << 11 | (g << 2 | g >> 2) << 5 | (b << 1 | b >> 3)


def pack565(rgb):
    r = np.floor(rgb[..., 0] / 255 * 31 + 0.5).astype(np.int32)
    g = np.floor(rgb[..., 1] / 255 * 63 + 0.5).astype(np.int32)
    b = np.floor(rgb[..., 2] / 255 * 31 + 0.5).astype(np.int32)
    return r << 11 | g << 5 | b


def unpack565(v):
    v = np.asarray(v)
    return np.stack([(v >> 11) / 31 * 255, ((v >> 5) & 63) / 63 * 255, (v & 31) / 31 * 255], axis=-1)


def le16(v):
    return bytes([int(v) & 255, int(v) >> 8 & 255])


def bits(which):
    return sum(1 << i for i, b in enumerate(which) if b)


def two_exact(packed):
    """5:6:5 values of no more than two colours: (lowest, highest, which are the highest)."""
    low, high = int(packed.min()), int(packed.max())
    if ((packed == low) | (packed == high)).all():
        return low, high, packed == high
    return None


def exact_bytes(px, coarsely=False):
    """(code, bytes) of a 32x32 quarter that can be sent as a bitmap, else None. coarsely:
    with its colours told apart at 4 bits a channel only."""
    packed = (pack444 if coarsely else pack565)(px)
    whole = two_exact(packed.reshape(1024))
    if whole and whole[0] == whole[1]:
        return CODE_F, le16(whole[0]) + b'\0\0'
    if whole:
        which = whole[2].reshape(32, 32)
        mixed, value, masks = 0, 0, b''
        for i in range(64):
            cell = which[i // 8 * 4:i // 8 * 4 + 4, i % 8 * 4:i % 8 * 4 + 4].reshape(16)
            if cell.all():
                value |= 1 << i
            elif cell.any():
                mixed |= 1 << i
                masks += le16(bits(cell))
        data = le16(whole[0]) + le16(whole[1]) + mixed.to_bytes(8, 'little') + value.to_bytes(8, 'little') + masks
        return CODE_T, data + b'\0' * (-len(data) % 4)
    kinds, body = 0, b''
    for i in range(16):
        block = packed[i // 4 * 8:i // 4 * 8 + 8, i % 4 * 8:i % 4 * 8 + 8]
        t = two_exact(block.reshape(64))
        if t and t[0] == t[1]:
            body += le16(t[0])
        elif t:
            kinds |= 1 << 2 * i
            body += le16(t[0]) + le16(t[1]) + bits(t[2]).to_bytes(8, 'little')
        else:
            kinds |= 2 << 2 * i
            for c in range(4):
                t = two_exact(block[c // 2 * 4:c // 2 * 4 + 4, c % 2 * 4:c % 2 * 4 + 4].reshape(16))
                if not t:
                    return None
                body += le16(t[0]) + le16(t[1]) + le16(bits(t[2]))
    data = kinds.to_bytes(4, 'little') + body
    return CODE_B, data + b'\0' * (-len(data) % 4)


def colours(data, at):
    return unpack565(data[at] | data[at + 1] << 8), unpack565(data[at + 2] | data[at + 3] << 8)


def mask_picture(data, at, n, c0, c1):
    which = np.array([[data[at + (y * n + x) // 8] >> (y * n + x) % 8 & 1 for x in range(n)] for y in range(n)], bool)
    return np.where(which[..., None], c1, c0)


def exact_picture(code, data):
    """The 32x32 pixels an exact quarter's bytes stand for."""
    if code == CODE_F:
        return np.tile(unpack565(data[0] | data[1] << 8), (32, 32, 1))
    out = np.zeros((32, 32, 3))
    if code == CODE_T:
        c0, c1 = colours(data, 0)
        at = 20
        for i in range(64):
            y, x = i // 8 * 4, i % 8 * 4
            if data[4 + i // 8] >> i % 8 & 1:
                out[y:y + 4, x:x + 4] = mask_picture(data, at, 4, c0, c1)
                at += 2
            else:
                out[y:y + 4, x:x + 4] = c1 if data[12 + i // 8] >> i % 8 & 1 else c0
        return out
    at = 4
    for i in range(16):
        y, x = i // 4 * 8, i % 4 * 8
        kind = data[i // 4] >> 2 * (i % 4) & 3
        if kind == 0:
            out[y:y + 8, x:x + 8] = unpack565(data[at] | data[at + 1] << 8)
            at += 2
        elif kind == 1:
            out[y:y + 8, x:x + 8] = mask_picture(data, at + 4, 8, *colours(data, at))
            at += 12
        else:
            for c in range(4):
                cy, cx = y + c // 2 * 4, x + c % 2 * 4
                out[cy:cy + 4, cx:cx + 4] = mask_picture(data, at + 4, 4, *colours(data, at))
                at += 6
    return out


def nearest(colours, shades):
    """The palette entry nearest each colour (the lowest of equals), and how far it is in the
    channel where it is furthest."""
    d = ((colours[..., None, :] - shades) ** 2).sum(axis=-1)
    which = d.argmin(axis=-1)
    return which, np.abs(colours - shades[which]).max(axis=-1)


def palette_step(palette, cells, counts, seed):
    """One capture's step of the palette (5:6:5 values) towards the cells that count: each
    entry goes to the mean of the cells nearest it, when that is further than MOVE; one
    with no cells takes the colour of a cell picked by a hash, if that cell is further than
    SEED_AT from its entry. Still pictures leave it alone."""
    shades = unpack565(palette)
    which, far = nearest(cells, shades)
    out = palette.copy()
    flat, count = cells.reshape(-1, 3), cells.shape[0] * cells.shape[1]
    for i in range(PALETTE):
        mine = counts & (which == i)
        if mine.any():
            new = pack565(cells[mine].mean(axis=0))
            if np.abs(unpack565(new) - shades[i]).max() > MOVE:
                out[i] = new
            continue
        h = (i * 2654435761 ^ seed * 40503) & 0xffffffff
        h ^= h >> 15
        h = h * 2246822519 & 0xffffffff
        h ^= h >> 13
        at = h % count
        if counts.reshape(-1)[at] and far.reshape(-1)[at] > SEED_AT:
            out[i] = pack565(flat[at])
    return out


def fitted(cells, steps=16):
    palette = np.zeros(PALETTE, np.int64)
    for seed in range(steps):
        palette = palette_step(palette, cells, np.ones(cells.shape[:2], bool), seed)
    return palette


def palette_at(data, row):
    """The palette in a row of Unity's atlas or store: four bytes an entry, two of them used."""
    part = data[row * ROW:row * ROW + 4 * PALETTE].astype(np.int64)
    return part[0::4] | part[1::4] << 8


def coarse_bytes(cells, shades):
    """A quarter's coarse bytes from its 16 cells' colours: the palette's nearest to the
    means of the darker and of the lighter, and a bit a cell."""
    luma = ycc(cells)[..., 0].reshape(16)
    lighter = luma > luma.mean() + 0.0255
    flat = cells.reshape(16, 3)
    low = flat[~lighter].mean(axis=0)
    high = flat[lighter].mean(axis=0) if lighter.any() else low
    which, _ = nearest(np.array([low, high]), shades)
    return bytes([int(which[0]), int(which[1]), bits(lighter) & 255, bits(lighter) >> 8])


def coarse_picture(data, shades):
    """The 32x32 pixels those bytes stand for."""
    high = data[2] | data[3] << 8
    cells = np.array([shades[data[1] if high >> c & 1 else data[0]] for c in range(16)]).reshape(4, 4, 3)
    return np.repeat(np.repeat(cells, 8, axis=0), 8, axis=1)


def cell_bytes(blocks):
    """A cell's sharp bytes from its Y, Cb, Cr integers: its length, a bit a channel that has
    more than a mean, the three means, then a block for each such channel (without its
    mean). A block that would end beyond the cell's 64 bytes is left out."""
    present, body = 0, b''
    for i, q in enumerate(blocks):
        rest = q.copy()
        rest[0] = 0
        part = block_bytes(rest)
        if part and 5 + len(body) + len(part) <= SLOT:
            present |= 1 << i
            body += part
    return bytes([5 + len(body), present] + [int(q[0]) & 255 for q in blocks]) + body


def cell_picture(data):
    """The 8x8 pixels a cell's sharp bytes stand for."""
    q = scale_blocks(data[1:2] + data[5:], 3)
    for i in range(3):
        q[i, 0] = data[2 + i] - 256 if data[2 + i] > 127 else data[2 + i]
    planes = [128 + restore(q[i].reshape(1, 1, 64), i > 0) for i in range(3)]
    return rgb_of(*planes)


class Encoded:
    """Everything the encoder makes of a picture: per quarter its exact (code, bytes) or
    None, its coarse bytes, and its 16 cells' sharp bytes."""

    def __init__(self, picture, palette=None, cells=None):
        self.rows, self.cols = picture.shape[0] // 32, picture.shape[1] // 32
        m = mips(picture, 3)
        if cells is not None:
            m[3] = cells   # the GPU's own mipmap: it rounds a level or two differently
        yc = ycc(m[0])
        # without one given: the palette sixteen captures of this picture would end with
        self.palette = fitted(m[3]) if palette is None else palette
        self.shades = unpack565(self.palette)
        q = [quantise(yc[..., i] - 128, i > 0, KEEP_C if i else KEEP_Y) for i in range(3)]
        self.exact, self.rough, self.coarse, self.cells = {}, {}, {}, {}
        for qy in range(self.rows):
            for qx in range(self.cols):
                px = picture[qy * 32:qy * 32 + 32, qx * 32:qx * 32 + 32]
                self.exact[qy, qx] = exact_bytes(px)
                # the same with colours told apart coarsely, for a quarter that is no bitmap as it is
                self.rough[qy, qx] = None if self.exact[qy, qx] else exact_bytes(px, True)
                self.coarse[qy, qx] = coarse_bytes(m[3][qy * 4:qy * 4 + 4, qx * 4:qx * 4 + 4], self.shades)
                self.cells[qy, qx] = [cell_bytes([q[i][qy * 4 + c // 4, qx * 4 + c % 4] for i in range(3)]) for c in range(16)]


class Received:
    """What a receiver shows: each cell is nothing, coarse or sharp; a quarter may be exact."""

    def __init__(self, rows, cols, palette):
        self.picture = np.zeros((rows * 32, cols * 32, 3))
        self.shades = unpack565(palette)

    def coarse(self, qy, qx, data, mask=0xffff):
        px = coarse_picture(data, self.shades)
        for c in range(16):
            if mask >> c & 1:
                y, x = qy * 32 + c // 4 * 8, qx * 32 + c % 4 * 8
                self.picture[y:y + 8, x:x + 8] = px[c // 4 * 8:c // 4 * 8 + 8, c % 4 * 8:c % 4 * 8 + 8]

    def sharp(self, qy, qx, c, data):
        y, x = qy * 32 + c // 4 * 8, qx * 32 + c % 4 * 8
        self.picture[y:y + 8, x:x + 8] = cell_picture(data)

    def exact(self, qy, qx, code, data):
        self.picture[qy * 32:qy * 32 + 32, qx * 32:qx * 32 + 32] = exact_picture(code, data)


def psnr(a, b):
    mse = ((a - b) ** 2).mean()
    return 99.0 if mse == 0 else 10 * np.log10(255 * 255 / mse)


def load(path):
    rgb = np.asarray(Image.open(path).convert('RGB')).astype(np.float64)
    h, w = rgb.shape[:2]
    padded = np.zeros(((h + 31) // 32 * 32, (w + 31) // 32 * 32, 3))
    padded[:h, :w] = rgb
    return padded, h, w


def save(picture, path):
    Image.fromarray(np.floor(np.clip(picture, 0, 255) + 0.5).astype(np.uint8)).save(path)


def from_store(path, rows, cols):
    """What a receiver's rows show. A quarter's row: bytes 0-1 a bit a cell that is coarse,
    2-3 a bit a cell that is sharp, 4 its exact kind or 0."""
    store = np.fromfile(path, np.uint8)
    out = Received(rows, cols, palette_at(store, STORE_PALETTE_ROW))
    for qy in range(rows):
        for qx in range(cols):
            index = qy * QCOLS + qx
            row, cells = bytes(store[index * ROW:][:ROW]), bytes(store[(QUARTERS + index) * ROW:][:ROW])
            if row[4]:
                out.exact(qy, qx, row[4], row[AT_EXACT:])
            else:
                out.coarse(qy, qx, row[AT_COARSE:], row[0] | row[1] << 8)
            for c in range(16):
                if (row[2] | row[3] << 8) >> c & 1:
                    out.sharp(qy, qx, c, cells[c * SLOT:])
    return out.picture


def against_atlas(path, e):
    """Unity's packed rows beside the model's."""
    atlas = np.fromfile(path, np.uint8)
    kinds = exact = coarse = cells = sizes = count = 0
    for (qy, qx), mine in e.exact.items():
        index = qy * QCOLS + qx
        row, theirs = bytes(atlas[index * ROW:][:ROW]), bytes(atlas[(QUARTERS + index) * ROW:][:ROW])
        count += 1
        rough = e.rough[qy, qx]
        kinds += row[4] == (mine[0] if mine else rough[0] | 128 if rough else 0)
        mine = mine or rough
        exact += bool(mine) and row[4] & 127 == mine[0] and row[AT_EXACT:AT_EXACT + 4 * row[5]] == mine[1]
        coarse += row[AT_COARSE:AT_COARSE + 4] == e.coarse[qy, qx]
        if not e.exact[qy, qx]:
            for c in range(16):
                cells += theirs[c * SLOT:c * SLOT + theirs[c * SLOT]] == e.cells[qy, qx][c]
                sizes += abs(theirs[c * SLOT] - len(e.cells[qy, qx][c])) <= 2
    n = sum(1 for v in e.exact.values() if not v) * 16
    print('  atlas: of %d quarters %d the same kind, %d the same bitmap bytes, %d the same coarse bytes; of %d cells %d the same bytes, '
          '%d within 2 bytes of the same length' % (count, kinds, exact, coarse, n, cells, sizes))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('picture')
    ap.add_argument('--out')
    ap.add_argument('--atlas')
    ap.add_argument('--store')
    ap.add_argument('--decoded')
    ap.add_argument('--cells', help="mipmap 3 of Unity's capture (160 x 96 RGBA), for coarse bytes that match its own")
    a = ap.parse_args()
    picture, h, w = load(a.picture)
    name = os.path.splitext(os.path.basename(a.picture))[0]
    # coarse bytes are indices: against Unity's they are made with Unity's palette
    cells = np.fromfile(a.cells, np.uint8).reshape(96, 160, 4)[:picture.shape[0] // 8, :picture.shape[1] // 8, :3].astype(np.float64) if a.cells else None
    e = Encoded(picture, palette_at(np.fromfile(a.atlas, np.uint8), PALETTE_ROW) if a.atlas else None, cells)
    quarters = sorted(e.exact)
    print('%s: %d x %d, %d quarters, %d cells' % (name, w, h, len(quarters), 16 * len(quarters)))
    for code, text in NAMES.items():
        sizes = [len(e.exact[k][1]) if e.exact[k] else sum(map(len, e.cells[k])) for k in quarters if (e.exact[k][0] if e.exact[k] else 0) == code]
        if sizes:
            print('  quarters as %-11s %4d, %6.1f bytes each' % (text, len(sizes), sum(sizes) / len(sizes)))
    # A receiver sent the coarse picture, then the bitmaps, then the sharp cells. Three bytes
    # a run of quarters, two bytes of bits a quarter of cells, a length a bitmap.
    got = Received(e.rows, e.cols, e.palette)
    total = 3 * len(quarters) // 8 + 2 * PALETTE
    for k in quarters:
        got.coarse(k[0], k[1], e.coarse[k])
        total += 4
    report = [('coarse', total, got.picture.copy())]
    count = 0
    for k in quarters:
        bitmap = e.exact[k] or e.rough[k]
        if bitmap:
            got.exact(k[0], k[1], *bitmap)
            total += len(bitmap[1]) + 1
            count += 1
    total += 3 * count // 8
    report.append(('bitmaps', total, got.picture.copy()))
    for k in quarters:
        if not e.exact[k]:
            for c in range(16):
                got.sharp(k[0], k[1], c, e.cells[k][c])
            total += 2 + sum(map(len, e.cells[k]))
    total += 3 * len(quarters) // 8
    report.append(('sharp', total, got.picture))
    rough = sum(1 for k in quarters if e.rough[k])
    print('  %d more quarters are bitmaps with colours told apart coarsely, %.1f bytes each'
          % (rough, sum(len(e.rough[k][1]) for k in quarters if e.rough[k]) / max(rough, 1)))
    for label, size, drawn in report:
        print('  after %-7s %6.1f KB (%4.1f s at 8.3 KB/s), %5.2f dB' % (label, size / 1000, size / 8300, psnr(drawn[:h, :w], picture[:h, :w])))
        if a.out:
            save(drawn[:h, :w], os.path.join(a.out, '%s_%s.png' % (name, label)))
    if a.atlas:
        against_atlas(a.atlas, e)
    if a.decoded:
        theirs = np.asarray(Image.open(a.decoded).convert('RGB')).astype(np.float64)[:h, :w]
        mine = from_store(a.store, e.rows, e.cols) if a.store else got.picture
        off = np.abs(theirs - mine[:h, :w]).max(axis=2)
        print('  decoder: %.2f%% of pixels within 2 levels of the model, %.2f%% within 6; %.2f dB to it, %.2f dB to the picture'
              % (100 * (off <= 2).mean(), 100 * (off <= 6).mean(), psnr(theirs, mine[:h, :w]), psnr(theirs, picture[:h, :w])))


if __name__ == '__main__':
    main()
