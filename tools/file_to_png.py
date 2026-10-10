"""A file as a PNG picture, for a host that can only be handed pictures (docs/fetch.md).

    python tools/file_to_png.py FILE [OUT.png]     the file's bytes as a picture's pixels
    python tools/file_to_png.py --back IN.png [OUT] the file out of such a picture
    python tools/file_to_png.py --check FILE        there and back in memory; says if it is the same

The picture's pixels are bytes, three each (red, green, blue), rows from the top. The first
256 are a header: the mark, the file's length, its CRC-32, its name. The picture must reach
the machine unchanged: a host that scales it or makes a JPEG of it destroys the file.
"""
import io
import math
import os
import struct
import sys
import zlib

from PIL import Image

MARK = b"SHADEREMU\x1aF1"
HEADER = 256
MOST = 2048   # pixels a side that VRChat loads


def pack(data, name):
    name = os.path.basename(name).encode("utf-8")[:235]
    header = MARK + struct.pack("<II", len(data), zlib.crc32(data) & 0xffffffff) + name
    stream = header.ljust(HEADER, b"\0") + data
    pixels = (len(stream) + 2) // 3
    # near square, at least 64 wide, a whole number of rows
    width = min(MOST, max(64, 1 << math.ceil(math.log2(max(1.0, math.sqrt(pixels))))))
    height = (pixels + width - 1) // width
    if height > MOST:
        raise SystemExit("%d bytes do not fit a picture of %d x %d (%d bytes)" % (len(data), MOST, MOST, MOST * MOST * 3 - HEADER))
    return Image.frombytes("RGB", (width, height), stream.ljust(width * height * 3, b"\0"))


def unpack(image):
    stream = image.convert("RGB").tobytes()
    if stream[:len(MARK)] != MARK:
        raise SystemExit("not a file in a picture: the mark is missing")
    length, crc = struct.unpack("<II", stream[12:20])
    name = stream[20:HEADER].split(b"\0")[0].decode("utf-8", "replace")
    data = stream[HEADER:HEADER + length]
    if len(data) != length or zlib.crc32(data) & 0xffffffff != crc:
        raise SystemExit("the picture was changed on its way: the file's sum is wrong")
    return name, data


def main(argv):
    if len(argv) >= 2 and argv[0] == "--back":
        name, data = unpack(Image.open(argv[1]))
        out = argv[2] if len(argv) > 2 else name
        open(out, "wb").write(data)
        print("%s: %d bytes" % (out, len(data)))
    elif len(argv) == 2 and argv[0] == "--check":
        data = open(argv[1], "rb").read()
        kept = io.BytesIO()
        pack(data, argv[1]).save(kept, "PNG")
        name, back = unpack(Image.open(io.BytesIO(kept.getvalue())))
        same = back == data and name == os.path.basename(argv[1])
        print("%s: %d bytes, a PNG of %d, %s" % (argv[1], len(data), len(kept.getvalue()), "the same there and back" if same else "DIFFERENT"))
        return 0 if same else 1
    elif len(argv) >= 1 and not argv[0].startswith("--"):
        data = open(argv[0], "rb").read()
        out = argv[1] if len(argv) > 1 else argv[0] + ".png"
        image = pack(data, argv[0])
        image.save(out, "PNG", optimize=True)
        print("%s: %d bytes as %d x %d pixels, %d bytes of PNG" % (out, len(data), image.width, image.height, os.path.getsize(out)))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
