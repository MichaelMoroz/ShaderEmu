"""The holodeck's HUD layer without Unity (docs/world/holodeck.md, "The HUD layer"): rvc_harness --hud-capture's BMP of the
last seated frame's flat draws, cropped to the frame's picture by the snapshot's volume words, as a PNG. Beside it, the
display's own picture of the same moment for comparison (tools/snapshot_screen.py).

    rvc_harness ... --cpu --save-state F.snap --hud-capture F.bmp
    python tools/hud_shot.py F.snap F.bmp OUT.png

The volume frame's words are at 0x81000300: its list's address, command count, frames so far, and the picture's size
(width in bits 0-15, height in bits 16-27; bit 31 a flat frame, the large screen's, bits 28-30 the up axis).
"""
import sys
import numpy as np
from PIL import Image


def main():
    snap, bmp, out = sys.argv[1:4]
    rows = int(np.fromfile(snap, dtype=np.uint32, count=4)[3])
    words = np.memmap(snap, dtype=np.uint32, mode='r', offset=32).reshape(rows, 2048, 4)[64:].reshape(-1)
    lst, count, frames, size = (int(v) for v in words[(0x81000300 & 0x7fffffff) >> 2:][:4])
    w, h, flat, up = size & 0xffff, (size >> 16) & 0xfff, size >> 31, (size >> 28) & 7
    print('volume frame %d: list %08x, %d commands, picture %d x %d, %s, up %d' % (frames, lst, count, w, h, 'flat' if flat else 'seated', up))
    if not (0 < w <= 2048 and 0 < h <= 2048):
        raise SystemExit('no volume frame')
    Image.open(bmp).convert('RGB').crop((0, 0, w, h)).save(out)
    print('saved', out)


if __name__ == '__main__':
    main()
