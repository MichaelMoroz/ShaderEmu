#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""n64_terrain.py -- a cartridge .MAP into the terrain JSON and atlas bake5.py reads.

OpenCNC3D's own `n64_terrain.py` lives in its asset store and is not in its published
tree; this one is written for ShaderEmu from what that tree says of it:
`tools/gen_terrain_tiles.py` (the atlas packing: 32 columns of 24-texel tiles at a
26-texel pitch, a texel of gutter round each, slots 0..n4-1 the 4bpp bank and then the
8bpp bank, which the .MAP numbers from 1024), `tools/heightmap-viewer/export.py` (a clear
or out-of-bank cell draws CLEAR1's icon (x&3)|((y&3)<<2); hole slots are tiles with an
alpha-0 texel) and the extracted bank files themselves:

    <TH>.DA4  24x24 4bpp tiles, 288 bytes each, high nibble first
    <TH>.ND4  one byte a 4bpp tile: which 16-colour sub-palette of PA4
    <TH>.PA4  RGB triplets, 16 a sub-palette
    <TH>.DA8  24x24 8bpp tiles, 576 bytes each, indices into PA8
    <TH>.PA8  256 RGB triplets
    <TH>.TL4/.TL8  the PC (template, icon) each slot draws

The cartridge's terrain palettes hold no water colour: black is the hole the console
draws its sea under (export.py's and cnc_eyes.cpp's reading), so a texel whose colour is
(0, 0, 0) gets alpha 0, and every alpha-0 texel is black.

    python3 n64_terrain.py ASSETS SCEN      writes ASSETS/terrain/terrain_SCEN.json and
                                            ASSETS/terrain/<TH>_tiles.png
"""
import json
import os
import struct
import sys

import numpy as np

TS, GUTTER, COLS = 24, 1, 32
PITCH = TS + 2 * GUTTER
BANK8_BASE = 1024
_EX = None


def _ex(ext, name):
    return open(os.path.join(_EX, ext, name + "." + ext), "rb").read()


def load_theater(root):
    da4, da8 = _ex("DA4", root), _ex("DA8", root)
    th = dict(root=root, da4=da4, da8=da8, nd4=_ex("ND4", root), nd8=_ex("ND8", root),
              pa4=_ex("PA4", root), pa8=_ex("PA8", root),
              tl4=_ex("TL4", root), tl8=_ex("TL8", root),
              n4=len(da4) // (TS * TS // 2), n8=len(da8) // (TS * TS))
    assert len(th["nd4"]) == th["n4"] and len(th["tl4"]) == 2 * th["n4"], root
    assert len(th["tl8"]) == 2 * th["n8"] and len(th["pa8"]) == 768, root
    assert len(th["pa4"]) % 48 == 0 and max(th["nd4"]) < len(th["pa4"]) // 48, root
    return th


def tl_lookup(th, tid):
    if tid < th["n4"]:
        return th["tl4"][2 * tid], th["tl4"][2 * tid + 1]
    if BANK8_BASE <= tid < BANK8_BASE + th["n8"]:
        k = tid - BANK8_BASE
        return th["tl8"][2 * k], th["tl8"][2 * k + 1]
    return None


def tile_rgba(th, tid):
    """(24, 24, 4) uint8 of one bank tile."""
    if tid < th["n4"]:
        raw = np.frombuffer(th["da4"], np.uint8, TS * TS // 2, tid * TS * TS // 2)
        idx = np.empty(TS * TS, np.uint8)
        idx[0::2] = raw >> 4
        idx[1::2] = raw & 15
        sub = th["nd4"][tid]
        pal = np.frombuffer(th["pa4"], np.uint8, 48, sub * 48).reshape(16, 3)
    else:
        k = tid - BANK8_BASE
        idx = np.frombuffer(th["da8"], np.uint8, TS * TS, k * TS * TS)
        pal = np.frombuffer(th["pa8"], np.uint8).reshape(256, 3)
    rgb = pal[idx].reshape(TS, TS, 3)
    a = np.where(rgb.any(axis=2), 255, 0).astype(np.uint8)
    return np.dstack([rgb, a])


def slot_ids(th):
    return list(range(th["n4"])) + [BANK8_BASE + k for k in range(th["n8"])]


def build_atlas(th):
    """The theater's tiles, 32 a row at a 26-texel pitch; the gutter repeats each
    tile's edge so a filtered sample at a tile's border does not read its neighbour."""
    ids = slot_ids(th)
    rows = (len(ids) + COLS - 1) // COLS
    atlas = np.zeros((rows * PITCH, COLS * PITCH, 4), np.uint8)
    holes = set()
    for i, tid in enumerate(ids):
        t = tile_rgba(th, tid)
        if (t[:, :, 3] == 0).any():
            holes.add(i)
        r, c = divmod(i, COLS)
        y0, x0 = r * PITCH, c * PITCH
        atlas[y0:y0 + PITCH, x0:x0 + PITCH] = np.pad(t, ((1, 1), (1, 1), (0, 0)), "edge")
    return atlas, holes


def ini_theater(path):
    for line in open(path, errors="ignore"):
        t = line.strip().upper()
        if t.startswith("THEATER="):
            v = t.split("=", 1)[1].strip()
            return {"TEMPERATE": "TEMPERAT", "DESERT": "DESERT",
                    "WINTER": "WINTER"}.get(v, v[:8])
    raise SystemExit("%s: no Theater= line" % path)


def main(assets, scen):
    global _EX
    from PIL import Image
    _EX = os.path.join(assets, "extracted")
    root = ini_theater(os.path.join(_EX, "INI", scen + ".INI"))
    th = load_theater(root)
    atlas, holes = build_atlas(th)
    ah, aw = atlas.shape[:2]
    out = os.path.join(assets, "terrain")
    os.makedirs(out, exist_ok=True)
    name = root + "_tiles.png"
    Image.fromarray(atlas, "RGBA").save(os.path.join(out, name))

    raw = open(os.path.join(_EX, "MAP", scen + ".MAP"), "rb").read()
    assert len(raw) == 8192, (scen, len(raw))
    cells, stats = [], dict(clear=0, unmapped=0, bank4=0, bank8=0)
    for i in range(64 * 64):
        x, y = i % 64, i // 64
        tid = struct.unpack_from(">H", raw, i * 2)[0]
        in4 = tid < th["n4"]
        in8 = BANK8_BASE <= tid < BANK8_BASE + th["n8"]
        if tid == 0xFFFF or not (in4 or in8):
            stats["clear" if tid == 0xFFFF else "unmapped"] += 1
            tid = (x & 3) | ((y & 3) << 2)
            in4 = True
        else:
            stats["bank4" if in4 else "bank8"] += 1
        slot = tid if in4 else th["n4"] + (tid - BANK8_BASE)
        r, c = divmod(slot, COLS)
        x0, y0 = c * PITCH + GUTTER, r * PITCH + GUTTER
        cells.append(dict(x=x, y=y, u0=x0 / aw, v0=y0 / ah,
                          u1=(x0 + TS) / aw, v1=(y0 + TS) / ah,
                          hasHoles=slot in holes, slot=slot))
    doc = dict(scenario=scen, theater=root, atlas=name, atlasDos=None,
               width=64, height=64, tileSize=TS, gutter=GUTTER, cols=COLS,
               atlasWidth=aw, atlasHeight=ah, cells=cells, stats=stats)
    with open(os.path.join(out, "terrain_%s.json" % scen), "w") as f:
        json.dump(doc, f)
    print("terrain %s: %s, atlas %dx%d (%d slots, %d with holes), cells %s"
          % (scen, root, aw, ah, len(slot_ids(th)), len(holes), stats))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
