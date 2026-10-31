#!/usr/bin/env python3
"""The trial's files as the port reads them: make_data.py TRIAL_FOLDER OUT.

language.dll's string table becomes language.txt (LoadString), the faces aoe-fonts.bin, Empires.dat is
converted (dat34.py) to the header the code wants. Run when an image is made; nothing of it is kept."""
import os, struct, sys, shutil

def pe_strings(path):
    """The string tables of a DLL: {id: text}."""
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    optsize = struct.unpack_from('<H', d, pe + 20)[0]
    rva_res = struct.unpack_from('<I', d, pe + 24 + 112)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsize + i * 40
        vs, va, rs, rp = struct.unpack_from('<IIII', d, o + 8)
        secs.append((va, max(vs, rs), rp))
    def off(rva):
        for va, sz, rp in secs:
            if va <= rva < va + sz:
                return rva - va + rp
        raise ValueError(rva)
    base = off(rva_res)
    def entries(o):
        _, _, _, _, nn, ni = struct.unpack_from('<IIHHHH', d, o)
        for i in range(nn + ni):
            yield struct.unpack_from('<II', d, o + 16 + i * 8)
    out = {}
    for name, sub in entries(base):
        if name != 6:  # RT_STRING
            continue
        for bid, sub2 in entries(base + (sub & 0x7fffffff)):
            for lang, leaf in entries(base + (sub2 & 0x7fffffff)):
                rva, size, _, _ = struct.unpack_from('<IIII', d, base + leaf)
                o = off(rva)
                for k in range(16):
                    n = struct.unpack_from('<H', d, o)[0]
                    o += 2
                    if n:
                        out[(bid - 1) * 16 + k] = d[o:o + n * 2].decode('utf-16-le').encode('cp1252', 'replace').decode('latin1')
                    o += n * 2
    return out

def write_language(trial, out):
    s = pe_strings(os.path.join(trial, 'language.dll'))
    with open(os.path.join(out, 'language.txt'), 'w', encoding='latin1', newline='\n') as f:
        for k in sorted(s):
            f.write('%d\t%s\n' % (k, s[k].replace('\\', '\\\\').replace('\n', '\\n').replace('\r', '\\r')))
    return len(s)

def write_fonts(trial, out, sizes=range(8, 37)):
    from PIL import Image, ImageDraw, ImageFont
    faces = [('Arial', 'Arial.ttf', 0), ('Arial', 'Arialbd.ttf', 1), ('Comic Sans MS', 'Comic.ttf', 0), ('Comic Sans MS', 'Comicbd.ttf', 1),
             ('Copperplate', 'Coprgtl.ttf', 0), ('Copperplate', 'Coprgtb.ttf', 1)]
    heads, bits = [], bytearray()
    HEAD = 24 + 8 + 256 + 1024
    fonts = [(n, f, b, s) for n, f, b in faces for s in sizes]
    start = 16 + HEAD * len(fonts)
    for name, file, bold, size in fonts:
        font = ImageFont.truetype(os.path.join(trial, file), size)
        asc, desc = font.getmetrics()
        h = asc + desc
        widths, offsets = [], []
        for c in range(256):
            ch = bytes([c]).decode('cp1252', 'replace')
            w = int(round(font.getlength(ch))) if c >= 32 else 0
            w = min(w, 255)
            im = Image.new('L', (max(w, 1), h), 0)
            ImageDraw.Draw(im).text((0, 0), ch, fill=255, font=font)
            offsets.append(start + len(bits))
            widths.append(w)
            stride = (w + 7) // 8
            px = im.load()
            for y in range(h):
                row = bytearray(stride)
                for x in range(w):
                    if px[x, y] >= 110:
                        row[x >> 3] |= 0x80 >> (x & 7)
                bits += row
        heads.append(name.encode()[:23].ljust(24, b'\0') + struct.pack('<HHHH', size, bold, h, asc) + bytes(widths) + struct.pack('<256I', *offsets))
    with open(os.path.join(out, 'aoe-fonts.bin'), 'wb') as f:
        f.write(struct.pack('<IIII', len(fonts), 0, 0, 0))
        f.write(b''.join(heads))
        f.write(bits)
    return len(fonts), len(bits)

KEEP = ('.drs', '.ai', '.per', '.rps', '.cty', '.col', '.scn', '.cpn', '.ply', '.mid', '.wav', '.inf')

def main():
    trial, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    print('strings', write_language(trial, out))
    print('fonts', write_fonts(trial, out))
    for n in os.listdir(trial):
        if n.lower().endswith(KEEP) or n.lower() in ('tileedge.dat',):
            shutil.copyfile(os.path.join(trial, n), os.path.join(out, n))
    # the world's data: see dat34.py
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)
    import dat34
    dat34.convert(os.path.join(trial, 'Empires.dat'), os.path.join(out, 'Empires.dat'))

if __name__ == '__main__':
    main()
