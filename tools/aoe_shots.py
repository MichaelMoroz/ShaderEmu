"""Age of Empires' pictures out of the machine (docs/games/aoe.md, "The world in slices"): the script's "shot" step keeps the
page and its palette in memory under a mark, and rvc_cpu --save-ram RAM at the run's end has them; this writes each as a PNG.
   python tools/aoe_shots.py RAM OUTPREFIX       (OUTPREFIX00.png, OUTPREFIX01.png, ... by the shot's number)"""
import struct, sys
from PIL import Image
ram = open(sys.argv[1], 'rb').read()
at = 0
while True:
    at = ram.find(b'AOESHOT!', at)
    if at < 0:
        break
    n, w, h, _ = struct.unpack_from('<4i', ram, at + 8)
    if 0 < w <= 1024 and 0 < h <= 768 and at + 1056 + w * h <= len(ram):
        pal = ram[at + 32:at + 1056]
        im = Image.frombytes('P', (w, h), ram[at + 1056:at + 1056 + w * h])
        im.putpalette(b''.join(pal[i * 4:i * 4 + 3] for i in range(256)))
        out = '%s%02d.png' % (sys.argv[2], n)
        im.convert('RGB').save(out)
        print(out, hex(0x80000000 + at))
    at += 8
