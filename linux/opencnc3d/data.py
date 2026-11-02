#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The 1995 DOS data OpenCNC3D takes its 2D, sound and the brain's palettes from: the
MIX files of EA's 2007 freeware Command & Conquer Gold, GDI disc (archive.org item
gdi-95, GDI95.ISO), fetched into a cache outside the tree and checked. Nothing of it
goes into this repository, linux/prebuilt or the shared image.

    python3 linux/opencnc3d/data.py CACHE            fetch what is missing, check all
    python3 linux/opencnc3d/data.py CACHE --list     the ISO's top folder, sizes

The files are TEMPERAT, DESERT, WINTER, CONQUER, GENERAL, SOUNDS, AUD and INSTALL/CCLOCAL
(9.8 MB in all).

The disc is not downloaded whole: its ISO9660 volume descriptor and root folder say
where each file lies, and each MIX is fetched by an HTTP range request, as OpenCNC3D's
data/dosdata/README.md does (its TEMPERAT.MIX and DESERT.MIX SHA-256 are the ones below).
Each file is checked against its SHA-256 and the disc's own size for it.
"""
import hashlib
import os
import struct
import sys
import time
import urllib.request

ISO_URL = "https://archive.org/download/gdi-95/GDI95.ISO"
ISO_SIZE = 608987136
UA = {"User-Agent": "ShaderEmu image build"}
# name -> SHA-256. TEMPERAT and DESERT are OpenCNC3D's data/dosdata/README.md's; the
# others were taken from the first fetch on 2 November 2026 and are held to it.
FILES = {
    "TEMPERAT.MIX": "25b41e31ea7fddc34fa18d9561ca9807b9d0124ff7340e8f3a107461d041ce81",
    "DESERT.MIX": "0141d371bdb847e085984bca44a0f96cb2f2f410d5eba1410b8e498087954072",
    "WINTER.MIX": "681f5aeb48751eb5e2c817ff9407b3176061d06c1fa3e5aeef71829a1b85a7fd",
    "CONQUER.MIX": "cbc5340f8fde5db9fc290e5fd5a1490494654b7b0f1538cbd3bd11bdb31223ca",
    "GENERAL.MIX": "5680a8f600ce0b20c76f42439597428b101039d2bf2bd7e6b5f6bf081fdd78a8",
    "SOUNDS.MIX": "e86f7cdc5239d5b52a9ec20d6da68051b214141c1848f22ee377c9faa7883b38",
    "AUD.MIX": "497f410042d5f122e60eb00bb079fb5b490f79ded7fd94a1a4eb85f80fcea2c9",
    "INSTALL/CCLOCAL.MIX": "ddce969a9aac815e3fbc0a8e548a507e055f44d2261f6cd9b1d1d08327214932",
}
# Not fetched yet: MOVIES.MIX (449 MB) and SCORES.MIX (39 MB), and LOCAL.MIX, SPEECH.MIX
# and ZOUNDS.MIX, which this disc keeps inside INSTALL/SETUP.Z (step 3 of the plan).


def get(start, n):
    req = urllib.request.Request(ISO_URL, headers=dict(UA, Range="bytes=%d-%d"
                                                       % (start, start + n - 1)))
    for attempt in range(5):
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                data = r.read()
            break
        except OSError as e:          # archive.org answers 500 now and then
            if attempt == 4:
                raise SystemExit("data.py: %s: %s" % (ISO_URL, e))
            time.sleep(2 + 3 * attempt)
    if len(data) != n:
        raise SystemExit("data.py: asked %d bytes at %d, got %d" % (n, start, len(data)))
    return data


def root_dir():
    """{NAME or FOLDER/NAME: (lba, size, is_dir)} of the top folder and the folders
    the files come from."""
    pvd = get(16 * 2048, 2048)
    assert pvd[0:6] == b"\x01CD001", "not an ISO9660 volume"
    rec = pvd[156:156 + 34]
    lba, size = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
    out = read_dir(lba, size)
    for sub in sorted({n.split("/")[0] for n in FILES if "/" in n}):
        slba, ssize, _d = out[sub]
        for k, v in read_dir(slba, ssize).items():
            out[sub + "/" + k] = v
    return out


def read_dir(lba, size):
    d = get(lba * 2048, size)
    out = {}
    o = 0
    while o < len(d):
        n = d[o]
        if n == 0:
            o = (o // 2048 + 1) * 2048
            continue
        flba = struct.unpack_from("<I", d, o + 2)[0]
        fsize = struct.unpack_from("<I", d, o + 10)[0]
        flags = d[o + 25]
        nl = d[o + 32]
        name = d[o + 33:o + 33 + nl].decode("latin-1").split(";")[0]
        if name not in ("\0", "\1"):
            out[name.upper()] = (flba, fsize, bool(flags & 2))
        o += n
    return out


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    cache = sys.argv[1]
    os.makedirs(cache, exist_ok=True)
    if "--list" in sys.argv:
        for k, (lba, size, isdir) in sorted(root_dir().items()):
            print("%-14s %10d%s" % (k, size, "  <dir>" if isdir else ""))
        return
    local = lambda n: os.path.join(cache, n.split("/")[-1])
    todo = [n for n, h in FILES.items()
            if not (os.path.exists(local(n)) and sha256(local(n)) == h)]
    if todo:
        ents = root_dir()
        for n in todo:
            lba, size, _d = ents[n]
            print("data.py: fetching %s (%d bytes) from %s" % (n, size, ISO_URL))
            part = local(n) + ".part"
            with open(part, "wb") as f:
                o = 0
                while o < size:
                    k = min(8 << 20, size - o)
                    f.write(get(lba * 2048 + o, k))
                    o += k
            got = sha256(part)
            if got != FILES[n]:
                os.remove(part)
                raise SystemExit("data.py: %s is not the file expected (SHA-256 %s)" % (n, got))
            os.replace(part, local(n))
    for n in sorted(FILES):
        print("data.py: %s ok (%d bytes, SHA-256 %s)"
              % (n, os.path.getsize(local(n)), sha256(local(n))))


if __name__ == "__main__":
    main()
