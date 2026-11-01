#!/usr/bin/env python3
"""The host's media_test against zlib and Pillow on files made to try the one-stream inflate: gz of several kinds and sizes, PNGs of one stream at
several levels, block sizes, filters and kinds. Run in WSL with the host's test program: inflate_check.py MEDIA_TEST_HOST [DIR]"""
import gzip, io, os, random, re, struct, subprocess, sys, tempfile, zlib
exe = sys.argv[1]
d = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp()
os.makedirs(d, exist_ok=True)
bad = done = 0
rng = random.Random(7)
def fnv(b):
    s = 2166136261
    for x in b:
        s = ((s ^ x) * 16777619) & 0xffffffff
    return s
def texts(n):
    words = [bytes(rng.choice(b'abcdefghij klmnop\n') for _ in range(rng.randint(2, 9))) for _ in range(2000)]
    out = []
    while sum(map(len, out)) < n:
        out.append(rng.choice(words))
    return b''.join(out)[:n]
kinds = {'text': texts, 'noise': lambda n: bytes(rng.getrandbits(8) for _ in range(n)), 'zeros': lambda n: bytes(n),
         'mixed': lambda n: texts(n // 3) + bytes(rng.getrandbits(8) for _ in range(n // 3)) + bytes(n // 3)}
print('gz')
for name, make in kinds.items():
    for n in (1000, 70000, 400000, 2000000):
        data = make(n)
        for level in (1, 6, 9):
            for mem in (8, 9, 1):
                c = zlib.compressobj(level, zlib.DEFLATED, 31, mem)
                z = c.compress(data) + c.flush()
                f = os.path.join(d, 't.gz')
                open(f, 'wb').write(z)
                for w in (0, 3, 15):
                    p = subprocess.run([exe, 'gunzip', f, str(w)], capture_output=True, text=True)
                    t = p.stdout + p.stderr
                    done += 1
                    m = re.search(r'fnv ([0-9a-f]{8})', t)
                    if not m or int(m.group(1), 16) != fnv(data):
                        bad += 1
                        print('BAD gz', name, n, level, mem, w, t[:120], hex(fnv(data)))
print('gz done', done, 'bad', bad)
def chunk(t, b):
    return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
def png(w, h, typ, rows, level, mem, idat, filt):
    ch = {0: 1, 2: 3, 4: 2, 6: 4}[typ]
    raw = bytearray()
    prev = bytes(w * ch)
    for y in range(h):
        row = rows[y]
        f = filt if filt != 5 else y % 5
        out = bytearray(w * ch)
        for x in range(w * ch):
            a = row[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 0: v = row[x]
            elif f == 1: v = row[x] - a
            elif f == 2: v = row[x] - b
            elif f == 3: v = row[x] - (a + b) // 2
            else:
                pp = a + b - c; pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                v = row[x] - (a if pa <= pb and pa <= pc else b if pb <= pc else c)
            out[x] = v & 255
        raw.append(f); raw += out
        prev = row
    c = zlib.compressobj(level, zlib.DEFLATED, 15, mem)
    z = c.compress(bytes(raw)) + c.flush()
    o = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, typ, 0, 0, 0))
    for i in range(0, len(z), idat):
        o += chunk(b'IDAT', z[i:i + idat])
    return o + chunk(b'IEND', b''), len(z)
print('png')
pd = pb = 0
for w, h in ((640, 480), (333, 500), (1024, 300), (97, 900)):
    for typ in (2, 6, 0):
        ch = {0: 1, 2: 3, 6: 4}[typ]
        for kind in ('picture', 'noisy'):
            rows = []
            base = [rng.randrange(256) for _ in range(ch)]
            for y in range(h):
                row = bytearray(w * ch)
                for x in range(w * ch):
                    if kind == 'noisy':
                        row[x] = rng.getrandbits(8) if (x // 37 + y // 23) % 3 else (x * y) & 255
                    else:
                        row[x] = (x // ch * 3 + y * 5 + rng.randrange(12) + base[x % ch]) & 255
                rows.append(bytes(row))
            for level, mem, idat, filt in ((6, 8, 8192, 5), (9, 9, 65536, 4), (1, 8, 1 << 30, 0), (6, 1, 3000, 1)):
                f = os.path.join(d, 't.png')
                o, zl = png(w, h, typ, rows, level, mem, idat, filt)
                open(f, 'wb').write(o)
                for chs in (3, 4):
                    for wk in (0, 15):
                        out = os.path.join(d, 'o.raw')
                        p = subprocess.run([exe, 'decode', f, out, str(wk), str(chs)], capture_output=True, text=True)
                        pd += 1
                        if p.returncode or not os.path.exists(out):
                            pb += 1; print('BAD run', w, h, typ, kind, level, mem, chs, wk, p.stdout[:100], p.stderr[:100]); continue
                        got = open(out, 'rb').read()
                        want = bytearray()
                        for r in rows:
                            for x in range(w):
                                px = r[x * ch:x * ch + ch]
                                if ch == 1: rgb = px * 3; al = 255
                                elif ch == 3: rgb = px; al = 255
                                else: rgb = px[:3]; al = px[3]
                                if chs == 4: want += rgb + bytes([al])
                                else:
                                    # as the library lays it over white
                                    want += bytes((v * al + 255 * (255 - al) + 127) // 255 for v in rgb)
                        # (the output may have a header and rows on 16 bytes: its pixels' tail compared by rows)
                        stride = (len(got) - (len(got) % h)) // h if len(got) % h == 0 else 0
                        ok = False
                        if stride:
                            rb = w * chs
                            ok = all(got[y * stride:y * stride + rb] == bytes(want[y * rb:(y + 1) * rb]) for y in range(h))
                        else:
                            ok = got[-len(want):] == bytes(want)
                        if not ok:
                            pb += 1; print('BAD pixels', w, h, typ, kind, level, mem, idat, filt, chs, wk, len(got), len(want))
print('png done', pd, 'bad', pb)
print('bad', bad + pb)
