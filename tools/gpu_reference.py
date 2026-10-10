"""Software model of the GPU device (docs/gpu.md), for checking the shader and the harness.

Reads a harness snapshot taken while the guest is paused (so the command list in RAM is the
one last drawn) and the GPU's colour target saved by --gpu-capture, redraws the list in
software and compares.

    python tools/gpu_reference.py SNAPSHOT TARGET.bmp [--png PREFIX] [--size WIDTHxHEIGHT]

The real picture comes from the graphics card's rasteriser, so this is not bit-exact: pixels
on triangle edges can fall either way, and colours can differ by a rounding step. The report
separates interior agreement from edge pixels.
"""
import os, struct, sys, zlib
import numpy as np

f32 = np.float32
TARGET = 2048


class Machine:
    def __init__(self, path):
        tex = np.memmap(path, dtype=np.uint32, mode='r', offset=32).reshape(4096, 2048, 4)
        self.words = tex[64:].reshape(-1)
        self.rom_words = None

    def rom(self):
        """The ROM (addresses from 0x40000000) as words: the Linux image's root file system."""
        if self.rom_words is None:
            path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build', 'images', 'linux', 'rootfs.bin')
            data = open(path, 'rb').read()
            self.rom_words = np.frombuffer(data + bytes(-len(data) % 4 + 4), dtype=np.uint32)
        return self.rom_words

    def w(self, addr, n=1):
        i = (addr & 0x7fffffff) >> 2
        return np.array(self.words[i:i + n])

    def fixed(self, addr, n):
        return (self.w(addr, n).view(np.int32) / f32(65536.0)).astype(f32)

    def numbers(self, addr, n, floats):
        """A draw's numbers: floats as they are (VERTEX_FLOAT), or 16.16 fixed point."""
        return self.w(addr, n).view(f32).copy() if floats else self.fixed(addr, n)


def colour_of(v):
    """0xTTRRGGBB as red, green, blue, alpha: T is transparency, so 0x00RRGGBB is opaque."""
    v = np.asarray(v, np.uint32)
    return np.stack([(v >> 16) & 255, (v >> 8) & 255, v & 255, 255 - (v >> 24)], axis=-1).astype(f32) / f32(255.0)


def place_pixels(p, depth):
    out = np.zeros((len(p), 4), f32)
    out[:, 0] = p[:, 0] * f32(2.0 / TARGET) - 1
    out[:, 1] = 1 - p[:, 1] * f32(2.0 / TARGET)
    out[:, 2] = depth
    out[:, 3] = 1
    return out


def vertices(m):
    """Every command's vertices, in mesh order: clip position, colour, uv, and per-vertex state."""
    mode, width, height = (int(v) for v in m.w(0x87000000, 3))
    if '--size' in sys.argv:   # a list drawn into a window: the picture has the window's size
        width, height = (int(v) for v in sys.argv[sys.argv.index('--size') + 1].split('x'))
    list_addr, count = (int(v) for v in m.w(0x87000014, 2))
    size = np.array([width, height], f32)
    out = []
    for c in range(min(count, 4096)):
        w = m.w(list_addr + 64 * c, 16)
        op = int(w[0])
        if op == 0:
            break
        if op == 1:
            pos = place_pixels(np.array([[0, 0], [2, 0], [0, 2]], f32) * size, 1.0)
            out.append(dict(op=op, pos=pos, colour=np.repeat(colour_of(w[1])[None], 3, 0), uv=np.zeros((3, 2), f32),
                            tex=(0, 0, 0, 0), key=0))
        elif op == 2:
            x0, y0, x1, y1 = (int(np.int32(v)) for v in w[4:8])
            u = w[12:16].view(np.int32) / f32(65536.0)
            corners = [0, 1, 2, 2, 1, 3]
            px = np.array([[x1 if k & 1 else x0, y1 if k & 2 else y0] for k in corners], f32)
            uv = np.array([[u[2] if k & 1 else u[0], u[3] if k & 2 else u[1]] for k in corners], f32)
            out.append(dict(op=op, pos=place_pixels(px, 0.0), colour=np.repeat(colour_of(w[1])[None], 6, 0), uv=uv,
                            tex=tuple(int(v) for v in w[8:12]), key=int(w[2])))
        elif op == 3:
            n = int(w[2])
            vmode, modelview, floats = int(w[4]) & 0xff, int(w[4]) & 0x100, bool(int(w[4]) & 0x800)
            # with the quads flag every six vertices drawn are four stored corners: 0 1 2, 0 2 3
            which = np.arange(n)
            if int(w[4]) & 0x200:
                which = (which // 6) * 4 + np.array([0, 1, 2, 0, 2, 3])[which % 6]
            corner = which % 3
            if int(w[4]) & 0x8000:   # points: a stored vertex a triangle
                which = which // 3
            stored = int(which.max()) + 1 if n else 0
            if int(w[4]) & 0x400:
                # compact: one texel a vertex (x, y, z, packed uv), the colour from the command
                if int(w[4]) & 0x4000:
                    # packed: a word a vertex (x, y, z bytes and a tag), u and v in a buffer of their own
                    p, c = m.w(int(w[1]), stored)[which].astype(np.uint32), m.w(int(w[12]), stored)[which].astype(np.uint32)
                    t = np.stack([p & 0xff, p >> 8 & 0xff, p >> 16 & 0xff, (c & 0xffffff) | (p & 0xff000000)], axis=1)
                    xyz = t[:, :3].astype(f32)
                else:
                    t = m.w(int(w[1]), stored * 4).reshape(stored, 4)[which]
                    xyz = np.ascontiguousarray(t[:, :3])
                    xyz = xyz.view(f32) if floats else (xyz.view(np.int32) / f32(65536.0)).astype(f32)
                pos = np.concatenate([xyz, np.ones((n, 1), f32)], axis=1)
                packed = t[:, 3].astype(np.uint32)
                uv = np.stack([(packed & 0xffff).astype(np.int16), (packed >> 16).astype(np.uint16).astype(np.int16)], axis=1).astype(f32) / f32(1024.0)
                v = np.zeros((n, 4, 4), f32)
                v[:, 0], v[:, 3] = pos, colour_of(w[11])
                if int(w[4]) & 0x1000:
                    # tagged: 12 bits each of u and v, and a tag, which with 0x2000 names a colour in the table at word 11
                    uv = np.stack([packed & 0xfff, packed >> 12 & 0xfff], axis=1).astype(f32) / f32(1024.0)
                    if int(w[4]) & 0x2000 and n:
                        table = m.w(int(w[11]), 256)
                        v[:, 3] = np.stack([colour_of(table[int(tag)]) for tag in packed >> 24])
                v[:, 2, :2] = uv
            else:
                v = m.numbers(int(w[1]), stored * 16, floats).reshape(stored, 4, 4)[which]
            pos, normal = v[:, 0], v[:, 1]
            colour = v[:, 3].copy()
            if vmode == 0:
                clip = place_pixels(pos[:, :2], pos[:, 2])
            else:
                u = m.numbers(int(w[6]), 40, floats).reshape(10, 4)
                if modelview:   # rows 4-6 are the modelview, applied before the projection
                    pos = np.concatenate([(pos @ u[4:7].T).astype(f32), pos[:, 3:4]], axis=1)
                if int(w[4]) & 0x8000:
                    # a point's triangle: the point, then `s` above it and `s` to its right as the eye sees it
                    how = w[12:15].view(f32).copy() if floats else w[12:15].astype(np.int32).astype(f32) / f32(65536.0)
                    depth = -pos[:, 2]
                    s = (how[0] * np.where(depth < how[2], f32(1.0), f32(1.0) + how[1] * depth)).astype(f32)
                    pos[:, 1] += np.where(corner == 1, s, f32(0.0))
                    pos[:, 0] += np.where(corner == 2, s, f32(0.0))
                cl = (pos @ u[0:4].T).astype(f32)
                part = size / f32(TARGET)
                clip = np.stack([(cl[:, 0] + cl[:, 3]) * part[0] - cl[:, 3], cl[:, 3] - (cl[:, 3] - cl[:, 1]) * part[1],
                                 (cl[:, 2] + cl[:, 3]) * f32(0.5), cl[:, 3]], axis=1).astype(f32)
                if vmode == 2:
                    nv = (normal[:, :3] @ u[4:7, :3].T).astype(f32)
                    facing = np.maximum(nv @ u[7, :3], 0).astype(f32)
                    colour = np.concatenate([facing[:, None] * u[8, :3] + u[9, :3], np.ones((n, 1), f32)], axis=1)
            uv = v[:, 2, :2].copy()
            if int(w[5]) & 0x200:
                # laid on the picture: words 12-15 are the corner's coordinates and their change in 1,024 pixels
                lay = w[12:16].view(f32).copy() if floats else w[12:16].astype(np.int32).astype(f32) / f32(65536.0)
                uv = np.repeat(lay[None, :2], n, 0)
                colour = np.repeat(np.array([[lay[2], lay[3], 0, 1]], f32), n, 0)
            out.append(dict(op=op, pos=clip, colour=colour.astype(f32), uv=uv,
                            tex=(int(w[5]), int(w[7]), int(w[8]), int(w[9])), key=int(w[10])))
    return width, height, out


def shade(m, colour, uv, tex, key):
    """Returns (red, green, blue, alpha 0..1, keep mask)."""
    mode, addr, tw, th = tex[0] & 0xff, tex[1], tex[2], tex[3]
    keep = np.ones(len(colour), bool)
    if tex[0] & 0x400 and mode in (1, 2) and tw and th:
        # smooth: the four texels round the point, repeating; a key texel counts for nothing
        base = (addr & 0x7fffffff) >> 2
        ax = (uv[:, 0] - np.floor(uv[:, 0])).astype(f32) * f32(tw) - f32(0.5)
        ay = (uv[:, 1] - np.floor(uv[:, 1])).astype(f32) * f32(th) - f32(0.5)
        lx, ly = np.floor(ax), np.floor(ay)
        px, py = (ax - lx).astype(np.float64), (ay - ly).astype(np.float64)
        x0, y0 = (lx.astype(np.int64) + tw) % tw, (ly.astype(np.int64) + th) % th
        x1, y1 = (x0 + 1) % tw, (y0 + 1) % th
        total = np.zeros((len(colour), 4))
        weight = np.zeros(len(colour))
        for x, y, w in ((x0, y0, (1 - px) * (1 - py)), (x1, y0, px * (1 - py)), (x0, y1, (1 - px) * py), (x1, y1, px * py)):
            n = y * tw + x
            if mode == 2:
                texel = (m.words[base + n // 4] >> (8 * (n & 3)).astype(np.uint32)) & 0xff
                there = texel != key if tex[0] & 0x100 else np.ones(len(colour), bool)
                texel = m.words[(0x07000400 >> 2) + texel]
            else:
                texel = m.words[base + n]
                there = texel != key if tex[0] & 0x100 else np.ones(len(colour), bool)
            w = w * there
            total += colour_of(texel) * w[:, None]
            weight += w
        keep = weight >= 0.5
        return np.clip(colour * total / np.maximum(weight, 1e-9)[:, None], 0, 1), keep
    if mode != 0 and tw and th:
        fx, fy = uv[:, 0] - np.floor(uv[:, 0]), uv[:, 1] - np.floor(uv[:, 1])
        x = np.minimum((fx.astype(f32) * f32(tw)).astype(np.int64), tw - 1)
        y = np.minimum((fy.astype(f32) * f32(th)).astype(np.int64), th - 1)
        n = y * tw + x
        base = (addr & 0x7fffffff) >> 2
        if mode == 3:
            n = y * ((tw + 7) >> 3) + (x >> 3)
            texel = (m.words[base + n // 4] >> (8 * (n & 3)).astype(np.uint32)) & 0xff
            return np.clip(colour, 0, 1), ((texel << (x & 7).astype(np.uint32)) & 0x80) != 0
        if mode == 5:
            # a layer of tiles: the address is four words (cells, tiles, palette, sizes)
            cells, tiles, palette = (int(v) & 0x7fffffff for v in m.words[base:base + 3])
            sizes = int(m.words[base + 3])   # (all 32 bits: the pieces' size is the top four)
            tile_w, tile_h, across, piece = max(sizes & 0xff, 1), max((sizes >> 8) & 0xff, 1), (sizes >> 16) & 0xfff, sizes >> 28
            at = cells + 2 * ((y // tile_h) * across + x // tile_w)
            cell = (m.words[at // 4] >> (8 * (at & 3)).astype(np.uint32)) & 0xffff
            px, py = x % tile_w, y % tile_h
            px = np.where(cell & 0x400, tile_w - 1 - px, px)
            py = np.where(cell & 0x800, tile_h - 1 - py, py)
            tile = (cell & 0x3ff).astype(np.int64)
            if piece:
                # tiles in pieces of 2^piece: the tiles' address is a table of the pieces'
                tiles = (m.words[tiles // 4 + (tile >> piece)] & 0x7fffffff).astype(np.int64)
                tile &= (1 << piece) - 1
            at = tiles + (tile * tile_h + py) * tile_w + px
            texel = (m.words[at // 4] >> (8 * (at & 3)).astype(np.uint32)) & 0xff
            if tex[0] & 0x100:
                keep = texel != key
            texel = m.words[palette // 4 + texel + 16 * (cell >> 12)]
        elif mode == 2:
            texel = (m.words[base + n // 4] >> (8 * (n & 3)).astype(np.uint32)) & 0xff
            if tex[0] & 0x100:
                keep = texel != key
            texel = m.words[(0x07000400 >> 2) + texel]
        elif mode == 4:
            # three bytes a pixel from any byte address, in RAM or the ROM, running on into the next word
            in_rom = (addr >> 30) == 1
            store = m.rom() if in_rom else m.words
            first = (addr - 0x40000000 if in_rom else addr & 0x7fffffff) + 3 * n
            at, shift = first // 4, (8 * (first & 3)).astype(np.uint64)
            texel = ((store[at].astype(np.uint64) | store[at + 1].astype(np.uint64) << np.uint64(32)) >> shift).astype(np.uint32)
            texel = (texel & 0xff) << 16 | (texel & 0xff00) | (texel >> 16) & 0xff
        else:
            texel = m.words[base + n]
            if tex[0] & 0x100:
                keep = texel != key
        colour = colour * colour_of(texel)
    return np.clip(colour, 0, 1), keep


def pass_of(cmd):
    """The pass a command is drawn in: bits 16-18 of its fragment mode word (docs/gpu.md)."""
    return (cmd['tex'][0] >> 16) & 7 if cmd['op'] != 1 else 0


def clip_near(p, colour, uv):
    """A triangle cut at the near plane (clip z = 0) as the card cuts it: none, one or two triangles."""
    inside = p[:, 2] >= 0
    if inside.all():
        return [(p, colour, uv)]
    if not inside.any():
        return []
    poly = []
    for i in range(3):
        j = (i + 1) % 3
        if inside[i]:
            poly.append((p[i], colour[i], uv[i]))
        if inside[i] != inside[j]:
            t = p[i][2] / (p[i][2] - p[j][2])
            poly.append((p[i] + t * (p[j] - p[i]), colour[i] + t * (colour[j] - colour[i]), uv[i] + t * (uv[j] - uv[i])))
    return [tuple(np.array([poly[0][k], poly[i][k], poly[i + 1][k]]) for k in range(3)) for i in range(1, len(poly) - 1)]


def draw(m):
    width, height, commands = vertices(m)
    image = np.zeros((height, width, 3), np.uint8)
    depth = np.full((height, width), np.inf)
    edge = np.zeros((height, width), bool)   # pixels within half a pixel of some triangle edge
    stats = {'commands': len(commands), 'triangles': 0, 'behind_eye': 0, 'passes': sorted({pass_of(c) for c in commands})}
    # passes in order, each with the list's commands in order; 0-3 test depth, only 0 writes it
    for cmd in sorted(commands, key=pass_of):
        which = pass_of(cmd)
        pos = cmd['pos'].astype(np.float64)
        triangles = []
        for t in range(len(pos) // 3):
            s = slice(3 * t, 3 * t + 3)
            if (pos[s, 3] > 1e-6).all():
                triangles.append((pos[s], cmd['colour'][s].astype(np.float64), cmd['uv'][s].astype(np.float64)))
                continue
            cut = [c for c in clip_near(pos[s], cmd['colour'][s].astype(np.float64), cmd['uv'][s].astype(np.float64))
                   if (c[0][:, 3] > 1e-6).all()]
            stats['behind_eye'] += not cut
            triangles += cut
        for p, corner_colour, corner_uv in triangles:
            iw = 1.0 / p[:, 3]
            # the card snaps vertices to 1/256 of a pixel
            sx = np.round((p[:, 0] * iw * 0.5 + 0.5) * TARGET * 256) / 256
            sy = np.round((0.5 - p[:, 1] * iw * 0.5) * TARGET * 256) / 256
            z = p[:, 2] * iw
            area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0])
            if area == 0:
                continue
            x_lo, x_hi = max(int(np.floor(sx.min())), 0), min(int(np.ceil(sx.max())) + 1, width)
            y_lo, y_hi = max(int(np.floor(sy.min())), 0), min(int(np.ceil(sy.max())) + 1, height)
            if x_lo >= x_hi or y_lo >= y_hi:
                continue
            stats['triangles'] += 1
            ys, xs = np.mgrid[y_lo:y_hi, x_lo:x_hi]
            px, py = xs + 0.5, ys + 0.5
            e = [(sx[(i + 2) % 3] - sx[(i + 1) % 3]) * (py - sy[(i + 1) % 3]) - (sy[(i + 2) % 3] - sy[(i + 1) % 3]) * (px - sx[(i + 1) % 3])
                 for i in range(3)]
            sign = 1.0 if area > 0 else -1.0
            # A pixel centre exactly on an edge belongs to the triangle only if that edge is a
            # left or a top one, so that two triangles sharing an edge never both draw it
            # (which blending would show).
            inside = np.ones(e[0].shape, bool)
            for i in range(3):
                a_, b_ = (i + 1) % 3, (i + 2) % 3
                gx, gy = -(sy[b_] - sy[a_]) * sign, (sx[b_] - sx[a_]) * sign   # the edge function's gradient
                owns = gx > 0 or (gx == 0 and gy > 0)
                inside &= (e[i] * sign > 0) | ((e[i] == 0) & owns)
            # distance to each edge line, to mark pixels whose coverage is a close call
            near = np.zeros_like(inside)
            for i in range(3):
                a, b = (i + 1) % 3, (i + 2) % 3
                length = np.hypot(sx[b] - sx[a], sy[b] - sy[a])
                if length > 0:
                    near |= np.abs(e[i]) / length < 0.75
            sl = (slice(y_lo, y_hi), slice(x_lo, x_hi))
            box = (px > sx.min() - 1) & (px < sx.max() + 1) & (py > sy.min() - 1) & (py < sy.max() + 1)
            edge[sl] |= near & box
            if not inside.any():
                continue
            l = [e[i] / area for i in range(3)]
            zf = l[0] * z[0] + l[1] * z[1] + l[2] * z[2]
            ok = inside & (zf >= -1e-9) & (zf <= 1 + 1e-9)   # the far plane itself is inside
            if which < 4:
                ok &= zf <= depth[sl]
            if not ok.any():
                continue
            pw = [l[i][ok] * iw[i] for i in range(3)]
            total = pw[0] + pw[1] + pw[2]
            pw = [v / total for v in pw]
            col = sum(pw[i][:, None] * corner_colour[i] for i in range(3))
            uv = sum(pw[i][:, None] * corner_uv[i] for i in range(3))
            if cmd['op'] == 3 and cmd['tex'][0] & 0x200:
                at = np.argwhere(ok)
                centre = np.stack([at[:, 1] + x_lo + 0.5, at[:, 0] + y_lo + 0.5], axis=1)
                uv = uv[:, :2] + centre * col[:, :2] / 1024.0
                col = np.ones_like(col)
            rgba, keep = shade(m, col.astype(f32), uv.astype(f32), cmd['tex'], cmd['key'])
            idx = np.argwhere(ok)[keep]
            yy, xx = idx[:, 0] + y_lo, idx[:, 1] + x_lo
            src, alpha = rgba[keep][:, :3].astype(np.float64), rgba[keep][:, 3:4].astype(np.float64)
            dst = image[yy, xx] / 255.0
            blend = which & 3   # none, alpha, additive, multiply
            out = src if blend == 0 else src * alpha + dst * (1 - alpha) if blend == 1 else src * alpha + dst if blend == 2 else src * dst
            image[yy, xx] = (np.clip(out, 0, 1) * 255.0 + 0.5).astype(np.uint8)
            if which == 0:
                depth[yy, xx] = zf[ok][keep]
    return image, edge, stats


def read_bmp(path):
    d = open(path, 'rb').read()
    off, = struct.unpack_from('<I', d, 10)
    w, h = struct.unpack_from('<ii', d, 18)
    return np.frombuffer(d, np.uint8, w * -h * 4, off).reshape(-h, w, 4)[:, :, 2::-1]


def write_png(path, image):
    image = np.ascontiguousarray(image)
    h, w, _ = image.shape
    raw = b''.join(b'\x00' + image[y].tobytes() for y in range(h))
    def chunk(t, c):
        return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main():
    m = Machine(sys.argv[1])
    want, edge, stats = draw(m)
    height, width, _ = want.shape
    got = read_bmp(sys.argv[2])[:height, :width]
    d = np.abs(got.astype(int) - want.astype(int)).max(axis=2)
    inner = ~edge
    print('picture %dx%d, %d commands, %d triangles on screen, %d wholly behind the eye'
          % (width, height, stats['commands'], stats['triangles'], stats['behind_eye']))
    print('away from triangle edges: %d pixels, %.3f%% identical, %.3f%% within 2 levels, %d differ by more than 8'
          % (inner.sum(), (d[inner] == 0).mean() * 100, (d[inner] <= 2).mean() * 100, int((d[inner] > 8).sum())))
    print('on triangle edges: %d pixels, %.2f%% within 2 levels' % (edge.sum(), (d[edge] <= 2).mean() * 100 if edge.any() else 100))
    if '--png' in sys.argv:
        prefix = sys.argv[sys.argv.index('--png') + 1]
        write_png(prefix + '_card.png', got)
        write_png(prefix + '_model.png', want)
        write_png(prefix + '_diff.png', np.repeat(np.clip(d * 16, 0, 255)[:, :, None], 3, axis=2).astype(np.uint8))


if __name__ == '__main__':
    main()
