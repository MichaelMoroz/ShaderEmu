"""What the Jedi Outcast demo's pak holds, read on the build computer (docs/games/jk2.md, "The demo's data").
   python linux/jk2/survey.py PAK [--files] [--entities]
PAK is the demo's demo/assets0.pk3 (linux/jk2/data.py puts it in its cache). It prints the pak's checksum as the
engine computes it (the 1.03 and 1.04 engines refuse a demo pak that is not 1431467275), the level's lumps and
entities, every Ghoul2 model and skeleton, the sounds, music and films, and the pictures by size. Nothing is
written. The formats are OpenJK's (code/qcommon/qfiles.h, code/rd-common/mdx_format.h)."""
import collections, io, os, struct, sys, zipfile

DEMO_PAK_CHECKSUM = 1431467275      # jk2-1.03 and 1.04's code/qcommon/files.cpp (1.02 had 3766578759)


# MD4 (RFC 1320), for the engine's Com_BlockChecksum: hashlib has it only where OpenSSL's legacy provider is on
def md4(data):
    def f(x, y, z): return (x & y) | (~x & z)
    def g(x, y, z): return (x & y) | (x & z) | (y & z)
    def h(x, y, z): return x ^ y ^ z
    def rol(v, s): v &= 0xffffffff; return ((v << s) | (v >> (32 - s))) & 0xffffffff
    msg = bytearray(data)
    n = len(msg) * 8
    msg.append(0x80)
    while len(msg) % 64 != 56:
        msg.append(0)
    msg += struct.pack('<Q', n)
    a, b, c, d = 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476
    for off in range(0, len(msg), 64):
        x = struct.unpack('<16I', msg[off:off + 64])
        aa, bb, cc, dd = a, b, c, d
        for i in range(4):
            k = 4 * i
            a = rol(a + f(b, c, d) + x[k], 3); d = rol(d + f(a, b, c) + x[k + 1], 7)
            c = rol(c + f(d, a, b) + x[k + 2], 11); b = rol(b + f(c, d, a) + x[k + 3], 19)
        for i in range(4):
            a = rol(a + g(b, c, d) + x[i] + 0x5a827999, 3); d = rol(d + g(a, b, c) + x[i + 4] + 0x5a827999, 5)
            c = rol(c + g(d, a, b) + x[i + 8] + 0x5a827999, 9); b = rol(b + g(c, d, a) + x[i + 12] + 0x5a827999, 13)
        for i in (0, 2, 1, 3):
            a = rol(a + h(b, c, d) + x[i] + 0x6ed9eba1, 3); d = rol(d + h(a, b, c) + x[i + 8] + 0x6ed9eba1, 9)
            c = rol(c + h(d, a, b) + x[i + 4] + 0x6ed9eba1, 11); b = rol(b + h(c, d, a) + x[i + 12] + 0x6ed9eba1, 15)
        a = (a + aa) & 0xffffffff; b = (b + bb) & 0xffffffff; c = (c + cc) & 0xffffffff; d = (d + dd) & 0xffffffff
    return struct.pack('<4I', a, b, c, d)


def pak_checksum(z):
    """FS_LoadZipFile's pack->checksum: Com_BlockChecksum over the CRCs of the files that are not empty, in the
    central directory's order."""
    crcs = [i.CRC for i in z.infolist() if i.file_size > 0]
    a, b, c, d = struct.unpack('<4I', md4(struct.pack('<%dI' % len(crcs), *crcs)))
    return a ^ b ^ c ^ d


def mb(n):
    return '%.2f MB' % (n / 1e6)


# ---- the level (RBSP 1) ----
LUMPS = ('entities shaders planes nodes leafs leafsurfaces leafbrushes models brushes brushsides drawverts '
         'drawindexes fogs surfaces lightmaps lightgrid visibility lightarray').split()
DSURFACE = struct.Struct('<7i4B4B4i4i4i2i3f9f2i')   # dsurface_t: 148 bytes
assert DSURFACE.size == 148


def entities(text):
    out, cur, key = [], None, None
    for tok in tokens(text):
        if tok == '{':
            cur = {}
        elif tok == '}':
            out.append(cur); cur = None
        elif cur is not None:
            if key is None:
                key = tok
            else:
                cur[key] = tok; key = None
    return out


def tokens(text):
    i = 0
    while i < len(text):
        ch = text[i]
        if ch.isspace():
            i += 1
        elif ch == '"':
            j = text.index('"', i + 1); yield text[i + 1:j]; i = j + 1
        else:
            j = i
            while j < len(text) and not text[j].isspace():
                j += 1
            yield text[i:j]; i = j


def bsp(name, data, show_entities):
    ident, version = struct.unpack_from('<4si', data)
    print('%s: %s version %d, %s' % (name, ident.decode('latin-1'), version, mb(len(data))))
    lumps = [struct.unpack_from('<2i', data, 8 + 8 * i) for i in range(len(LUMPS))]
    for (ofs, ln), lname in zip(lumps, LUMPS):
        print('    %-13s %10d bytes' % (lname, ln))
    ofs, ln = lumps[13]
    kinds = collections.Counter(); tris = collections.Counter(); verts = 0; lm = set(); styled = 0
    for k in range(ln // DSURFACE.size):
        s = DSURFACE.unpack_from(data, ofs + k * DSURFACE.size)
        kind, nverts, nidx = s[2], s[4], s[6]
        kinds[kind] += 1; tris[kind] += nidx // 3; verts += nverts
        lm.update(x for x in s[15:19] if x >= 0)
        styled += any(x not in (0, 255) for x in s[7:11])
    names = {1: 'planar', 2: 'patch', 3: 'triangle soup', 4: 'flare'}
    print('    surfaces: ' + ', '.join('%s %d (%d triangles)' % (names.get(k, k), kinds[k], tris[k]) for k in sorted(kinds)))
    print('    draw vertices %d (80 bytes each), indexes %d, shaders %d, light maps %d of 128 x 128 (%d used), '
          'surfaces with a light style other than 0: %d' % (lumps[10][1] // 80, lumps[11][1] // 4, lumps[1][1] // 72,
                                                           lumps[14][1] // (128 * 128 * 3), len(lm), styled))
    eo, el = lumps[0]
    ents = entities(data[eo:eo + el].decode('latin-1'))
    classes = collections.Counter(e.get('classname', '?') for e in ents)
    print('    entities %d; NPCs %d: %s' % (len(ents), sum(v for k, v in classes.items() if k.startswith('NPC_')),
          ', '.join('%s %d' % kv for kv in sorted(classes.items()) if kv[0].startswith('NPC_'))))
    scripts = collections.Counter(v for e in ents for k, v in e.items() if k.endswith('script') or k == 'spawnscript')
    print('    scripts named by entities: %d different' % len(scripts))
    w = ents[0] if ents else {}
    print('    worldspawn: ' + ', '.join('%s=%s' % kv for kv in sorted(w.items()) if kv[0] != 'classname'))
    if show_entities:
        for k, v in sorted(classes.items(), key=lambda kv: -kv[1]):
            print('        %5d %s' % (v, k))


# ---- Ghoul2 ----
def glm(name, data):
    ident, version, mname, aname, _, nbones, nlods, ofs_lods, nsurf, _, end = struct.unpack_from('<4si64s64s7i', data)
    out = []
    ofs = ofs_lods
    for lod in range(nlods):
        lod_end, = struct.unpack_from('<i', data, ofs)
        # after ofsEnd: a table of numSurfaces offsets, relative to the table, then the surfaces
        table = ofs + 4
        nv = nt = nrefs = 0
        for s in range(nsurf):
            so = table + struct.unpack_from('<i', data, table + 4 * s)[0]
            _, idx, _, numv, _, numt, _, numrefs, _, _ = struct.unpack_from('<10i', data, so)
            nv += numv; nt += numt; nrefs = max(nrefs, numrefs)
        out.append((nv, nt, nrefs))
        ofs += lod_end
    return (ident.decode('latin-1'), version, mname.split(b'\0')[0].decode('latin-1'),
            aname.split(b'\0')[0].decode('latin-1'), nbones, nsurf, out)


def gla(data):
    ident, version, gname, scale, nframes, ofs_frames, nbones, ofs_pool, ofs_skel, end = \
        struct.unpack_from('<4si64sf6i', data)
    return ident.decode('latin-1'), version, gname.split(b'\0')[0].decode('latin-1'), nframes, nbones, (end - ofs_pool) // 14


# ---- sounds, music, films ----
MP3_RATES = {3: (44100, 48000, 32000), 2: (22050, 24000, 16000), 0: (11025, 12000, 8000)}
MP3_KBPS = {1: (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320),     # MPEG-1 layer III
            0: (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160)}       # MPEG-2 and 2.5 layer III


def mp3(data):
    i = 0
    if data[:3] == b'ID3':
        i = 10 + ((data[6] & 127) << 21 | (data[7] & 127) << 14 | (data[8] & 127) << 7 | (data[9] & 127))
    while i + 4 <= len(data):
        if data[i] == 0xff and data[i + 1] & 0xe0 == 0xe0:
            ver = (data[i + 1] >> 3) & 3; rate_i = (data[i + 2] >> 2) & 3; br_i = data[i + 2] >> 4
            if ver != 1 and rate_i != 3 and 0 < br_i < 15:
                kbps = MP3_KBPS[1 if ver == 3 else 0][br_i]
                return MP3_RATES[ver][rate_i], kbps, 1 if data[i + 3] >> 6 == 3 else 2, len(data) * 8 / (kbps * 1000)
        i += 1
    return None


def wav(data):
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        return None
    i = 12; fmt = None
    while i + 8 <= len(data):
        cid, ln = struct.unpack_from('<4sI', data, i)
        if cid == b'fmt ':
            tag, ch, rate, _, _, bits = struct.unpack_from('<HHIIHH', data, i + 8); fmt = (tag, ch, rate, bits)
        elif cid == b'data' and fmt:
            return fmt + (ln / (fmt[2] * fmt[1] * fmt[3] / 8),)
        i += 8 + ln + (ln & 1)
    return None


def roq(data):
    i = 8
    while i + 8 <= len(data):
        cid, size, arg = struct.unpack_from('<HIH', data, i)
        if cid == 0x1001:
            w, h = struct.unpack_from('<HH', data, i + 8)
            return w, h
        i += 8 + size
    return None


# ---- pictures ----
def picture_size(name, data):
    if name.endswith('.tga'):
        kind = data[2]; w, h, bpp = struct.unpack_from('<HHB', data, 12)
        return w, h, bpp, {2: 'raw', 10: 'rle', 3: 'grey', 11: 'grey rle'}.get(kind, str(kind))
    if name.endswith('.jpg'):
        i = 2
        while i + 9 < len(data):
            if data[i] != 0xff:
                i += 1; continue
            m = data[i + 1]
            if m in (0xc0, 0xc1, 0xc2):
                h, w = struct.unpack_from('>HH', data, i + 5)
                return w, h, data[i + 9] * 8, 'progressive' if m == 0xc2 else 'baseline'
            i += 2 + struct.unpack_from('>H', data, i + 2)[0]
    return None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        print(__doc__); return 2
    z = zipfile.ZipFile(args[0])
    infos = [i for i in z.infolist() if not i.is_dir()]
    ck = pak_checksum(z)
    print('%s: %d files, %s packed, %s unpacked; checksum %d (%s the demo engine\'s)' % (
        os.path.basename(args[0]), len(infos), mb(os.path.getsize(args[0])), mb(sum(i.file_size for i in infos)), ck,
        'is' if ck == DEMO_PAK_CHECKSUM else 'NOT'))
    by = collections.defaultdict(lambda: [0, 0])
    for i in infos:
        k = (i.filename.split('/')[0].lower() if '/' in i.filename else '.', os.path.splitext(i.filename)[1].lower())
        by[k][0] += 1; by[k][1] += i.file_size
    print('\nBy folder and kind (files, bytes unpacked):')
    for k, (n, s) in sorted(by.items(), key=lambda kv: -kv[1][1]):
        print('    %-12s %-9s %5d %12d' % (k[0], k[1], n, s))
    if '--files' in sys.argv:
        for i in infos:
            print('    %10d %s' % (i.file_size, i.filename))
    print()
    for i in infos:
        if i.filename.lower().endswith('.bsp'):
            bsp(i.filename, z.read(i), '--entities' in sys.argv)
    print('\nGhoul2 meshes (vertices / triangles a LOD, the most bones one surface refers to):')
    totals = [0, 0]
    for i in infos:
        if i.filename.lower().endswith('.glm'):
            ident, v, n, a, nb, ns, lods = glm(i.filename, z.read(i))
            totals[0] += lods[0][0]; totals[1] += lods[0][1]
            print('    %-44s %s %d, %3d bones, %2d surfaces, anim %-28s %s' % (
                i.filename, ident, v, nb, ns, a, ' | '.join('%d/%d/%d' % x for x in lods)))
    print('    LOD 0 of all of them: %d vertices, %d triangles' % tuple(totals))
    print('\nGhoul2 skeletons and animations:')
    for i in infos:
        if i.filename.lower().endswith('.gla'):
            ident, v, n, nf, nb, pool = gla(z.read(i))
            print('    %-44s %s %d, %5d frames, %3d bones, %6d distinct compressed bones (14 bytes), %s' % (
                i.filename, ident, v, nf, nb, pool, mb(i.file_size)))
    md3 = [i for i in infos if i.filename.lower().endswith('.md3')]
    print('\nMD3 models: %d, %s' % (len(md3), mb(sum(i.file_size for i in md3))))
    for kind, ext in (('Sounds', 'sound/'), ('Music', 'music/')):
        rates = collections.Counter(); secs = 0.0; n = 0; size = 0
        for i in infos:
            name = i.filename.lower()
            if not name.startswith(ext) and not (kind == 'Sounds' and name.startswith('sound') and not name.startswith('music')):
                continue
            if not (name.endswith('.wav') or name.endswith('.mp3')):
                continue
            d = z.read(i); n += 1; size += i.file_size
            if name.endswith('.mp3'):
                r = mp3(d)
                if r:
                    rates['mp3 %d Hz %d kbit/s %s' % (r[0], r[1], 'mono' if r[2] == 1 else 'stereo')] += 1; secs += r[3]
            else:
                r = wav(d)
                if r:
                    rates['wav %d Hz %d bits %s%s' % (r[2], r[3], 'mono' if r[1] == 1 else 'stereo',
                                                     '' if r[0] == 1 else ' (format %d)' % r[0])] += 1; secs += r[4]
        print('\n%s: %d files, %s, %.0f s' % (kind, n, mb(size), secs))
        for k, v in rates.most_common():
            print('    %5d %s' % (v, k))
    print('\nFilms:')
    for i in infos:
        if i.filename.lower().endswith('.roq'):
            print('    %-30s %s, %s' % (i.filename, '%d x %d' % (roq(z.read(i)) or (0, 0)), mb(i.file_size)))
    sizes = collections.Counter(); texels = 0; kinds = collections.Counter()
    for i in infos:
        name = i.filename.lower()
        if name.endswith('.tga') or name.endswith('.jpg'):
            p = picture_size(name, z.read(i))
            if p:
                sizes['%d x %d' % p[:2]] += 1; texels += p[0] * p[1]; kinds['%s %s %d bits' % (name[-3:], p[3], p[2])] += 1
    print('\nPictures: %d, %.1f million texels; by kind: %s' % (sum(sizes.values()), texels / 1e6,
          ', '.join('%s %d' % kv for kv in kinds.most_common())))
    print('    by size: ' + ', '.join('%s %d' % kv for kv in sizes.most_common(14)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
