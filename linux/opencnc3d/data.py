#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The 1995 MS-DOS data OpenCNC3D takes its 2D, its sound and the brain's palettes from: MIX
files fetched into a cache outside the tree and checked. Nothing of it goes into this
repository, linux/prebuilt or the shared image.

    python3 linux/opencnc3d/data.py CACHE            fetch what is missing, check all
    python3 linux/opencnc3d/data.py CACHE --list     each disc's files and sizes

Two disc images on archive.org, the two OpenCNC3D's own launcher reads by byte range
(its tools/media/media-sources.txt), neither downloaded whole: each file's place is read from
the ISO9660 folders and the file fetched by HTTP range requests.
  - gdi-95/GDI95.ISO: the GDI disc of C&C Gold, the freeware release of 2007. Its TEMPERAT.MIX
    and DESERT.MIX are the SHA-256 OpenCNC3D's data/dosdata/README.md gives; from it come the
    three theaters and AUD.MIX. Gold is the Windows 95 game at 640 x 400: its fonts and its
    sidebar are drawn for that, and OpenCNC3D's menu and sidebar draw the DOS game's 320 x 200
    ones, so nothing with fonts or interface art is taken from it.
  - cc_worldwide_warfare/COVERT.iso: The Covert Operations, the MS-DOS disc, in English, whose
    score and TRANSIT.MIX OpenCNC3D's launcher fetches. From it: LOCAL.MIX (the fonts, the
    title and its palette), CONQUER.MIX (sidebar, cursors, infantry, cameos), GENERAL.MIX,
    SOUNDS.MIX, SPEECH.MIX and TRANSIT.MIX.
Not fetched: the films (MOVIES.MIX) and the score (SCORES.MIX), the plan's step 3, and
ZOUNDS.MIX, which neither disc has.
"""
import hashlib
import os
import struct
import sys
import time
import urllib.request

UA = {"User-Agent": "ShaderEmu image build"}
DISCS = {
    "gdi": "https://archive.org/download/gdi-95/GDI95.ISO",
    "covert": "https://archive.org/download/cc_worldwide_warfare/COVERT.iso",
}
# name -> (disc, its path there, SHA-256). The GDI disc's TEMPERAT and DESERT are OpenCNC3D's
# data/dosdata/README.md's; the others were taken from the first fetch on 2 November 2026.
FILES = {
    "TEMPERAT.MIX": ("gdi", "TEMPERAT.MIX", "25b41e31ea7fddc34fa18d9561ca9807b9d0124ff7340e8f3a107461d041ce81"),
    "DESERT.MIX": ("gdi", "DESERT.MIX", "0141d371bdb847e085984bca44a0f96cb2f2f410d5eba1410b8e498087954072"),
    "WINTER.MIX": ("gdi", "WINTER.MIX", "681f5aeb48751eb5e2c817ff9407b3176061d06c1fa3e5aeef71829a1b85a7fd"),
    "AUD.MIX": ("gdi", "AUD.MIX", "497f410042d5f122e60eb00bb079fb5b490f79ded7fd94a1a4eb85f80fcea2c9"),
    "LOCAL.MIX": ("covert", "INSTALL/LOCAL.MIX", "bfb82755e5010cc752d43f47cb109154030e1f50f50a63156b45af9e80a2ec76"),
    "CONQUER.MIX": ("covert", "CONQUER.MIX", "9b2f655ac1144c4b9271a7f7246884321cb9337537fe53ebc8956f1e4ca30921"),
    "GENERAL.MIX": ("covert", "GENERAL.MIX", "5c098c77a2ab20c1c0dab6997b76f4dfbcfeb183714fd1a45e701fcfc8c19d26"),
    "SOUNDS.MIX": ("covert", "SOUNDS.MIX", "37dc88345e68a5514722210bef0a87d6eaf9434be8ec64bb970d03ac521df5a8"),
    "SPEECH.MIX": ("covert", "INSTALL/SPEECH.MIX", "4d5e730df355fed1420e85355abf18d1ef04f4015a45d9773c44e2b11e63e71d"),
    "TRANSIT.MIX": ("covert", "INSTALL/TRANSIT.MIX", "9ec1f850bff16430b8d7634f3de81862954fcd9b6468684397643fd1b3d85c78"),
}


def get(url, start, n):
    req = urllib.request.Request(url, headers=dict(UA, Range="bytes=%d-%d" % (start, start + n - 1)))
    for attempt in range(5):
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                data = r.read()
            break
        except OSError as e:          # archive.org answers 500 now and then
            if attempt == 4:
                raise SystemExit("data.py: %s: %s" % (url, e))
            time.sleep(2 + 3 * attempt)
    if len(data) != n:
        raise SystemExit("data.py: asked %d bytes at %d of %s, got %d" % (n, start, url, len(data)))
    return data


def get_big(url, start, n):
    out = bytearray()
    while len(out) < n:
        k = min(8 << 20, n - len(out))
        out += get(url, start + len(out), k)
    return bytes(out)


def iso_dir(url):
    """{PATH: (lba, size)} of the ISO's top folder and the folders in it."""
    pvd = get(url, 16 * 2048, 2048)
    assert pvd[0:6] == b"\x01CD001", "not an ISO9660 volume"
    rec = pvd[156:156 + 34]
    out = {}

    def walk(lba, size, prefix):
        d = get(url, lba * 2048, size)
        o = 0
        while o < len(d):
            n = d[o]
            if n == 0:
                o = (o // 2048 + 1) * 2048
                continue
            name = d[o + 33:o + 33 + d[o + 32]].decode("latin-1").split(";")[0].upper()
            at, sz = struct.unpack_from("<I", d, o + 2)[0], struct.unpack_from("<I", d, o + 10)[0]
            if name not in ("\0", "\1"):
                if d[o + 25] & 2:
                    if not prefix:
                        walk(at, sz, name + "/")
                else:
                    out[prefix + name] = (at, sz)
            o += n

    walk(struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0], "")
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
        for disc, url in DISCS.items():
            for k, v in sorted(iso_dir(url).items()):
                print("%-6s %-24s %10d" % (disc, k, v[1]))
        return
    path = lambda n: os.path.join(cache, n)
    dirs = {}
    for n, (disc, where, want) in FILES.items():
        if os.path.exists(path(n)) and sha256(path(n)) == want:
            continue
        url = DISCS[disc]
        print("data.py: fetching %s (%s of %s)" % (n, where, url))
        if disc not in dirs:
            dirs[disc] = iso_dir(url)
        lba, size = dirs[disc][where]
        data = get_big(url, lba * 2048, size)
        got = hashlib.sha256(data).hexdigest()
        if got != want:
            raise SystemExit("data.py: %s is not the file expected (SHA-256 %s)" % (n, got))
        with open(path(n) + ".part", "wb") as f:
            f.write(data)
        os.replace(path(n) + ".part", path(n))
    for n in sorted(FILES):
        print("data.py: %s ok (%d bytes)" % (n, os.path.getsize(path(n))))


if __name__ == "__main__":
    main()
