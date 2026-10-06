"""Software model of the GPU device (docs/gpu.md), for checking the shader and the harness.

Reads a harness snapshot taken while the guest is paused (so the command list in RAM is the
one last drawn) and the GPU's colour target saved by --gpu-capture, redraws the list in
software and compares.

    python tools/gpu_reference.py SNAPSHOT TARGET.bmp [--png PREFIX]

The real picture comes from the graphics card's rasteriser, so this is not bit-exact: pixels
on triangle edges can fall either way, and colours can differ by a rounding step. The report
separates interior agreement from edge pixels.
"""
import struct, sys, zlib
import numpy as np

f32 = np.float32
TARGET = 2048


class Machine:
    def __init__(self, path):
        tex = np.memmap(path, dtype=np.uint32, mode='r', offset=32).reshape(4096, 2048, 4)
        self.words = tex[64:].reshape(-1)

    def w(self, addr, n=1):
        i = (addr & 0x7fffffff) >> 2
        return np.array(self.words[i:i + n])

    def fixed(self, addr, n):
        return (self.w(addr, n).view(np.int32) / f32(65536.0)).astype(f32)


def colour_of(v):
    v = np.asarray(v, np.uint32)
    return np.stack([(v >> 16) & 255, (v >> 8) & 255, v & 255], axis=-1).astype(f32) / f32(255.0)


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
            v = m.fixed(int(w[1]), n * 16).reshape(n, 4, 4)
            pos, normal = v[:, 0], v[:, 1]
            colour = v[:, 3, :3].copy()
            vmode = int(w[4])
            if vmode == 0:
                clip = place_pixels(pos[:, :2], pos[:, 2])
            else:
                u = m.fixed(int(w[6]), 40).reshape(10, 4)
                cl = (pos @ u[0:4].T).astype(f32)
                part = size / f32(TARGET)
                clip = np.stack([(cl[:, 0] + cl[:, 3]) * part[0] - cl[:, 3], cl[:, 3] - (cl[:, 3] - cl[:, 1]) * part[1],
                                 (cl[:, 2] + cl[:, 3]) * f32(0.5), cl[:, 3]], axis=1).astype(f32)
                if vmode == 2:
                    nv = (normal[:, :3] @ u[4:7, :3].T).astype(f32)
                    facing = np.maximum(nv @ u[7, :3], 0).astype(f32)
                    colour = facing[:, None] * u[8, :3] + u[9, :3]
            out.append(dict(op=op, pos=clip, colour=colour.astype(f32), uv=v[:, 2, :2].copy(),
                            tex=(int(w[5]), int(w[7]), int(w[8]), int(w[9])), key=int(w[10])))
    return width, height, out


def shade(m, colour, uv, tex, key):
    """Returns (rgb 0..1, keep mask)."""
    mode, addr, tw, th = tex[0] & 0xff, tex[1], tex[2], tex[3]
    keep = np.ones(len(colour), bool)
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
        if mode == 2:
            texel = (m.words[base + n // 4] >> (8 * (n & 3)).astype(np.uint32)) & 0xff
            if tex[0] & 0x100:
                keep = texel != key
            texel = m.words[(0x07000400 >> 2) + texel]
        else:
            texel = m.words[base + n]
            if tex[0] & 0x100:
                keep = texel != key
        colour = colour * colour_of(texel)
    return np.clip(colour, 0, 1), keep


def draw(m):
    width, height, commands = vertices(m)
    image = np.zeros((height, width, 3), np.uint8)
    depth = np.full((height, width), np.inf)
    edge = np.zeros((height, width), bool)   # pixels within half a pixel of some triangle edge
    stats = {'commands': len(commands), 'triangles': 0, 'behind_eye': 0}
    for cmd in commands:
        pos = cmd['pos'].astype(np.float64)
        for t in range(len(pos) // 3):
            p = pos[3 * t:3 * t + 3]
            if (p[:, 3] <= 1e-6).any():
                stats['behind_eye'] += 1   # the card clips these; the model leaves them out
                continue
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
            inside = (e[0] * sign >= 0) & (e[1] * sign >= 0) & (e[2] * sign >= 0)
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
            ok = inside & (zf <= depth[sl]) & (zf >= 0) & (zf <= 1)
            if not ok.any():
                continue
            pw = [l[i][ok] * iw[i] for i in range(3)]
            total = pw[0] + pw[1] + pw[2]
            pw = [v / total for v in pw]
            col = sum(pw[i][:, None] * cmd['colour'][3 * t + i].astype(np.float64) for i in range(3))
            uv = sum(pw[i][:, None] * cmd['uv'][3 * t + i].astype(np.float64) for i in range(3))
            rgb, keep = shade(m, col.astype(f32), uv.astype(f32), cmd['tex'], cmd['key'])
            idx = np.argwhere(ok)[keep]
            yy, xx = idx[:, 0] + y_lo, idx[:, 1] + x_lo
            image[yy, xx] = (rgb[keep] * 255.0 + 0.5).astype(np.uint8)
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
    print('picture %dx%d, %d commands, %d triangles on screen, %d dropped behind the eye'
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
