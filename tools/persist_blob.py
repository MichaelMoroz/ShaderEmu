#!/usr/bin/env python3
"""The bytes a host keeps of /persistence (docs/system/persist.md, "The bytes"), on the build computer.

    python tools/persist_blob.py make FOLDER BLOB      a blob of version 1 whose archive holds FOLDER as /persistence
    python tools/persist_blob.py read BLOB [DIR]       its header, and its archive's entries (into DIR); versions 1 and 2
    python tools/persist_blob.py info BLOB             the header, and for version 3 its blocks

`rvc_cpu --persist BLOB` then gives a guest's `nxpersist load!` that folder: a way to bring files into a run
without making an image (at most the fetch's 256 KB)."""
import gzip, io, lzma, os, struct, sys, tarfile, zlib


def header(blob):
    if blob[:4] != b'SEPD':
        raise SystemExit('not a blob: no SEPD mark')
    version, length, crc = struct.unpack_from('<III', blob, 4)
    body = blob[16:]
    ok = length == len(body) and zlib.crc32(body) == crc
    return version, length, ok, body


def make(folder, out):
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.USTAR_FORMAT) as t:
        t.add(folder, arcname='persistence', filter=lambda i: (setattr(i, 'uid', 0), setattr(i, 'gid', 0), setattr(i, 'uname', ''), setattr(i, 'gname', ''), i)[-1])
    packed = gzip.compress(raw.getvalue(), 9, mtime=0)
    with open(out, 'wb') as f:
        f.write(b'SEPD' + struct.pack('<III', 1, len(packed), zlib.crc32(packed)) + packed)
    print(f'{out}: {len(raw.getvalue())} bytes of archive, {16 + len(packed)} kept')


def read(path, to=None):
    blob = open(path, 'rb').read()
    version, length, ok, body = header(blob)
    print(f'version {version}, {length} bytes, sum {"right" if ok else "WRONG"}')
    if version == 1:
        raw = gzip.decompress(body)
    elif version == 2:
        raw = lzma.decompress(body[:-4], format=lzma.FORMAT_XZ)
    else:
        raise SystemExit('version 3 is unpacked by the guest only (raw LZMA2 with preset dictionaries, which Python cannot give)')
    with tarfile.open(fileobj=io.BytesIO(raw)) as t:
        for m in t.getmembers():
            print(f'{m.size:9d} {m.name}')
        if to:
            t.extractall(to, filter='data')


def info(path):
    blob = open(path, 'rb').read()
    version, length, ok, body = header(blob)
    print(f'version {version}, {length} bytes, sum {"right" if ok else "WRONG"}')
    if version == 3:
        size, block, prime, count = struct.unpack_from('<IIII', body, 0)
        words = struct.unpack_from(f'<{count}I', body, 16)
        texts = sum(1 for w in words if w & 0x80000000)
        print(f'archive {size} bytes, {count} blocks of {block}, primed with {prime}; {texts} packed as text')


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == 'make':
        make(a[1], a[2])
    elif len(a) >= 2 and a[0] == 'read':
        read(a[1], a[2] if len(a) > 2 else None)
    elif len(a) == 2 and a[0] == 'info':
        info(a[1])
    else:
        print(__doc__)
