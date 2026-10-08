"""The texture pack with nothing compressed: the zip's entries stored, and each PNG's pixels in
stored blocks with no row filters. The guest then copies where it would inflate.

    python3 store_zip.py IN.zip OUT.zip
"""
import struct, sys, zipfile, zlib

def chunks(data):
    at = 8
    while at < len(data):
        n, kind = struct.unpack('>I4s', data[at:at + 8])
        yield kind, data[at + 8:at + 8 + n]
        at += 12 + n

def chunk(kind, body):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)

def unfiltered(raw, width, height, bpp):
    """The rows with filter 0, from rows with any of PNG's five."""
    row = width * bpp
    out = bytearray()
    prev = bytearray(row)
    for y in range(height):
        kind = raw[y * (row + 1)]
        cur = bytearray(raw[y * (row + 1) + 1:(y + 1) * (row + 1)])
        for i in range(row):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if kind == 1:
                cur[i] = (cur[i] + a) & 255
            elif kind == 2:
                cur[i] = (cur[i] + b) & 255
            elif kind == 3:
                cur[i] = (cur[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                cur[i] = (cur[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += b'\x00' + cur
        prev = cur
    return bytes(out)

def stored_png(data):
    parts = list(chunks(data))
    width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', parts[0][1])
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    if depth != 8 or interlace:
        return data   # (none of the pack's are: left as they are)
    raw = zlib.decompress(b''.join(body for kind, body in parts if kind == b'IDAT'))
    raw = unfiltered(raw, width, height, channels)
    out, done = data[:8], False
    for kind, body in parts:
        if kind != b'IDAT':
            out += chunk(kind, body)
        elif not done:
            out += chunk(b'IDAT', zlib.compress(raw, 0))
            done = True
    return out

source = zipfile.ZipFile(sys.argv[1])
with zipfile.ZipFile(sys.argv[2], 'w', zipfile.ZIP_STORED) as out:
    for info in source.infolist():
        data = source.read(info)
        if info.filename.lower().endswith('.png'):
            data = stored_png(data)
        out.writestr(zipfile.ZipInfo(info.filename, info.date_time), data)
