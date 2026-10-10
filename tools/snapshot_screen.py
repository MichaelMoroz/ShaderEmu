"""The guest's screen from a machine snapshot (rvc_harness --save-state), as a PNG.

    python tools/snapshot_screen.py SNAPSHOT OUT.png

Composes the display's layers (docs/display.md, mode 4: the table at the address in
0x8700000c is a count, three unused words, then eight words a layer: x, y, width, height,
address) or shows the RAM framebuffer of the other modes that have one.
"""
import sys
import numpy as np
from PIL import Image


def main():
    tex = np.memmap(sys.argv[1], dtype=np.uint32, mode='r', offset=32).reshape(4096, 2048, 4)
    words = tex[64:].reshape(-1)

    def w(address, n=1):
        i = (address & 0x7fffffff) >> 2
        return np.array(words[i:i + n])

    mode, width, height, table = (int(v) for v in w(0x87000000, 4))
    print('display mode %d, %d x %d' % (mode, width, height))
    if not (0 < width <= 2048 and 0 < height <= 2048):
        raise SystemExit('no display')
    screen = np.zeros((height, width), dtype=np.uint32)
    if mode == 4:
        count = int(w(table)[0])
        for k in range(count):
            x, y, lw, lh, address = (int(v) for v in w(table + 16 + 32 * k, 5))
            x, y = np.int32(x), np.int32(y)
            print('  layer %d: %d x %d at %d, %d from %08x' % (k, lw, lh, x, y, address))
            pixels = w(address, lw * lh).reshape(lh, lw)
            x0, y0, x1, y1 = max(0, x), max(0, y), min(width, x + lw), min(height, y + lh)
            if x1 > x0 and y1 > y0:
                screen[y0:y1, x0:x1] = pixels[y0 - y:y1 - y, x0 - x:x1 - x]
    else:
        screen = w(0x87001000, width * height).reshape(height, width)
    rgb = np.stack([(screen >> 16) & 255, (screen >> 8) & 255, screen & 255], axis=-1).astype(np.uint8)
    Image.fromarray(rgb).save(sys.argv[2])
    print('saved', sys.argv[2])


if __name__ == '__main__':
    main()
