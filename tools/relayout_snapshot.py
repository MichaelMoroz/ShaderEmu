"""Converts a row-major snapshot to a tiled RAM layout: relayout_snap.py in.snap out.snap BITS [z]"""
import sys
import numpy as np

src, dst, bits = sys.argv[1], sys.argv[2], int(sys.argv[3])
zorder = len(sys.argv) > 4 and sys.argv[4] == 'z'
HDR, W, H, ROW0 = 32, 2048, 4096, 64
raw = np.fromfile(src, dtype=np.uint8)
assert raw.size == HDR + W * H * 16, raw.size
tex = raw[HDR:].reshape(H, W, 16)
ram = tex[ROW0:].reshape(-1, 16)            # row-major: index = texel number
T, TPR = 1 << bits, W >> bits
ys, xs = np.meshgrid(np.arange(H - ROW0, dtype=np.uint32), np.arange(W, dtype=np.uint32), indexing='ij')
tile = (ys >> bits) * TPR + (xs >> bits)
ix, iy = xs & (T - 1), ys & (T - 1)
if zorder:
    def spread(v):
        v = v & 0xff; v = (v | (v << 4)) & 0x0f0f; v = (v | (v << 2)) & 0x3333; v = (v | (v << 1)) & 0x5555
        return v
    within = spread(ix) | (spread(iy) << 1)
else:
    within = ix | (iy << bits)
lin = (tile << (2 * bits)) | within           # texel number that lives at (x, y) in the new layout
assert np.array_equal(np.sort(lin.ravel()), np.arange(lin.size, dtype=np.uint32)), 'layout is not a bijection'
out = raw.copy()
out[HDR:].reshape(H, W, 16)[ROW0:] = ram[lin]
out.tofile(dst)
print('wrote', dst)
