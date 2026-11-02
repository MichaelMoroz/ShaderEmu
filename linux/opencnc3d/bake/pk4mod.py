#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""pk4mod.py -- the part of OpenCNC3D's pack baker that its tree does not carry.

OpenCNC3D's `bake5.py` begins with `import pk4mod; B = pk4mod.load()`, and its
`pk4mod.py` only loaded `bake_pk4.pyc`, compiled Python whose source was lost and which
is not in the published tree (its `tools/bakery/README.md`). This file is written anew
for ShaderEmu, from what the published tree says of that module: the names `bake5.py`,
`survey_parts.py` and `bake_n64infantry.py` read from `B`, the walker of
`support/vx_rdp.py` (written to agree with it), `tools/romdump/textures.py` and
`soldier.py`, `docs/re-findings.md`, and the renderer's pack loader in
`game/cnc_eyes.cpp`. It is not a copy of anything: no bytecode was read.

It is copied next to `bake5.py` in a fetched OpenCNC3D tree by
`linux/opencnc3d/bake.py` and imported from there; it finds its neighbours
(`support/vx_rdp.py`, `tools/romdump/soldier.py`) from where it lies.

What `B` must give, and what each is here:

    ROM, SCR, ASSETS          the cartridge's bytes; the support folder; the asset tree
    GLOBAL_TLUT               the house TLUT a CI texture with no TLUT of its own is
                              read through (bake5 sets it to the GDI table and back)
    R                         support/vx_rdp.py (make_resolve, walk, decode)
    walk(rom, off, resolve)   (verts, tris): tris are (i0, i1, i2, state); the state
                              carries the texture in force and `prim`
    decode_tex(state, res)    (key, w, h, rgba, rest) of the texture a state binds
    bake_mesh(off, bank)      [(texture index, mode, wrap, (v0, v1, v2))], each vertex
                              (x, y, z, u, v, r, g, b, a), uv over the texture's used size
    TexBank                   textures padded to powers of two, items (w, h, uw, uh, rgba)
    SOL, SPRITE_SETS, SPRITE_ANIMS, INFANTRY_SETS, N_HOUSE, bake_sprite
                              the infantry strips (tools/romdump/soldier.py)
    CONF                      unit_models.json's confidence word -> the pack's byte
    SUBPARTS                  True: models are baked with their scene graph's parts
    w32, wname                little-endian writers
"""
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))     # tools/bakery of the fetched tree
TREE = os.path.normpath(os.path.join(HERE, "..", ".."))
SCR = os.path.join(HERE, "support")
SHARE = os.path.join(SCR, "share", "OpenCNC3D")
ASSETS = os.path.join(SHARE, "assets")
# bake5.py imports `segments` right after loading this module, from a romdump folder its
# own path arithmetic does not reach when it runs as tools/bakery/bake5.py; the lost
# module put the romdump tools on the path, and so does this one (at the end, so that
# bake5's own insertions win).
sys.path.append(os.path.join(TREE, "tools", "romdump"))


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


R = _load("vx_rdp", os.path.join(SCR, "vx_rdp.py"))
# soldier.py finds the ROM and the extracted IMG/ENG files from share/OpenCNC3D, which is
# where it would lie in OpenCNC3D's own layout; it is loaded from the romdump folder and
# its ROOT pointed there.
SOL = _load("soldier", os.path.join(TREE, "tools", "romdump", "soldier.py"))
SOL.ROOT = SHARE
SOL.ROM_P = os.path.join(SHARE, "rom", "cnc_eu.z64")
SOL.IMG_D = os.path.join(ASSETS, "extracted", "IMG")
SOL.SOLF_P = os.path.join(ASSETS, "extracted", "ENG", "SOLFILES.ENG")
_by_name = None


def _sol_by_name():
    global _by_name
    if _by_name is None:
        _by_name = dict(zip(SOL.names(), SOL.records()))
    return _by_name


ROM = open(os.path.join(SHARE, "rom", "cnc_eu.z64"), "rb").read()

# The Nod/neutral house table, ROM 0x99130 (re-findings M5, "CI textures need the global
# palette"); bake5 swaps in the GDI one at 0x98F30 for the PK6 variants.
GLOBAL_TLUT = 0x99130
SUBPARTS = True
N_HOUSE = 2
# cnc_eyes.cpp: `it->second.conf < 1` is "low", drawn as a fallback. unit_models.json
# says high (194), low (1) and none (5, which have no mesh).
CONF = {"none": 0, "low": 0, "medium": 1, "high": 2}

# The renderer's SprAnim enum (cnc_eyes.cpp): STAND, RUN, DETH. A scenario pack carries
# these three strips a set (bake_n64infantry.py's note). bake5 reads only the name of each
# (anim, frames, facings) triple; a strip's counts come from soldier.py's descriptors.
SPRITE_SETS = list(SOL.SET_NAMES)
SPRITE_ANIMS = [("STAND", None, None), ("RUN", None, None), ("DETH1", None, None)]

# INI type code -> (set, (GDI palette, Nod palette)), from soldier.py's TYPE_MAP: combat
# sets take the house (palette 0 GDI, 1 Nod), the doctors and civilians a fixed one.
INFANTRY_SETS = {}
for _code, _tid in SOL.INI_TYPE.items():
    _set, _pal = SOL.TYPE_MAP[_tid]
    INFANTRY_SETS[_code] = (_set, (0, 1) if _pal is None else (_pal, _pal))


def w32(f, v):
    f.write(struct.pack("<I", v & 0xFFFFFFFF))


def wname(f, s, n):
    b = s.encode("latin-1")[:n]
    f.write(b + b"\0" * (n - len(b)))


def _pow2(n):
    p = 1
    while p < n:
        p <<= 1
    return p


class TexBank(object):
    """Textures by key. A texture is stored padded to powers of two with transparent
    texels to the right and below (the renderer scales uv by uw/w and uh/h), so one
    key is one bank slot and the same key twice is the same index."""

    def __init__(self):
        self.items = []
        self.bykey = {}

    def add_rgba(self, key, w, h, rgba):
        if key in self.bykey:
            return self.bykey[key]
        assert len(rgba) == w * h * 4, (key, w, h, len(rgba))
        W, H = _pow2(w), _pow2(h)
        if (W, H) == (w, h):
            data = bytes(rgba)
        else:
            buf = bytearray(W * H * 4)
            for y in range(h):
                buf[y * W * 4:y * W * 4 + w * 4] = rgba[y * w * 4:(y + 1) * w * 4]
            data = bytes(buf)
        self.bykey[key] = len(self.items)
        self.items.append((W, H, w, h, data))
        return self.bykey[key]


# ------------------------------------------------------------------ the walker
class _Tile(object):
    __slots__ = ("fmt", "siz", "line", "tmem", "pal", "cmT", "maskT", "shiftT",
                 "cmS", "maskS", "shiftS", "uls", "ult", "lrs", "lrt", "sized")

    def __init__(self):
        for k in self.__slots__:
            setattr(self, k, 0)
        self.sized = False


def walk(rom, dl_off, resolve, max_cmds=8000):
    """F3DEX2 walk of one display list and everything it calls, with the RDP state.

    A G_DL call shares the caller's state (vertex buffer, tiles, TMEM, PRIM): that is
    what the RSP does, and it is what `mesh_sections`' batch counter in bake5 assumes when
    it counts every G_TRI1/G_TRI2 of the call graph. TMEM is tracked by address, so a
    tile renders what the last LOADBLOCK/LOADTILE put at its TMEM address, not whatever
    G_SETTIMG came last (a TLUT load follows the texture's in most of these lists)."""
    verts, tris = [], []
    S = dict(slots=[None] * 64, tiles=[_Tile() for _ in range(8)],
             timg=None, tmem={}, tlut=None, tlut_n=0, rtile=0, texon=0,
             scale=(0x8000, 0x8000), prim=None, combine=None, rendermode=None,
             geom=0, drawn=set())

    def run(o, depth):
        steps = 0
        while steps < max_cmds and o + 8 <= len(rom):
            steps += 1
            w0, w1 = struct.unpack_from(">II", rom, o)
            o += 8
            op = w0 >> 24
            if op == 0x01:                                    # G_VTX
                numv = (w0 >> 12) & 0xFF
                end = (w0 >> 1) & 0x7F
                start = end - numv
                p = resolve(w1)
                if p is None or p < 0 or p + numv * 16 > len(rom):
                    continue
                for i in range(numv):
                    verts.append(R.read_vtx(rom, p + i * 16))
                    if 0 <= start + i < 64:
                        S["slots"][start + i] = len(verts) - 1
            elif op == 0x02:                                  # G_MODIFYVTX
                where = (w0 >> 16) & 0xFF
                si = (w0 & 0xFFFF) // 2
                if where == 0x14 and 0 <= si < 64 and S["slots"][si] is not None:
                    vi = S["slots"][si]
                    if vi in S["drawn"]:        # never rewrite a vertex already drawn
                        verts.append(verts[vi])
                        vi = len(verts) - 1
                        S["slots"][si] = vi
                    s = (w1 >> 16) & 0xFFFF
                    t = w1 & 0xFFFF
                    s = s - 0x10000 if s & 0x8000 else s
                    t = t - 0x10000 if t & 0x8000 else t
                    v = verts[vi]
                    # the value is after the G_TEXTURE scale: twice it is the buffer's
                    # unit (textures.py, MODIFY_ST_MUL, proven on V02)
                    verts[vi] = (v[0], v[1], v[2], s * 2, t * 2) + tuple(v[5:])
            elif op in (0x05, 0x06):                          # G_TRI1 / G_TRI2
                for w in ((w0,) if op == 0x05 else (w0, w1)):
                    t = [(w >> 17) & 0x7F, (w >> 9) & 0x7F, (w >> 1) & 0x7F]
                    if all(0 <= i < 64 and S["slots"][i] is not None for i in t):
                        f = [S["slots"][i] for i in t]
                        S["drawn"].update(f)
                        tris.append((f[0], f[1], f[2], _state()))
            elif op == 0xD7:                                  # G_TEXTURE
                S["rtile"] = (w0 >> 8) & 7
                S["texon"] = (w0 >> 1) & 0x7F
                S["scale"] = ((w1 >> 16) & 0xFFFF, w1 & 0xFFFF)
            elif op == 0xFA:                                  # G_SETPRIMCOLOR
                S["prim"] = ((w1 >> 24) & 0xFF, (w1 >> 16) & 0xFF,
                             (w1 >> 8) & 0xFF, w1 & 0xFF)
            elif op == 0xFC:                                  # G_SETCOMBINE
                S["combine"] = (w0, w1)
            elif op == 0xE2:                                  # G_SETOTHERMODE_L
                if w0 == 0xE200001C:
                    S["rendermode"] = w1
            elif op == 0xD9:                                  # G_GEOMETRYMODE
                S["geom"] = (S["geom"] & (w0 & 0xFFFFFF)) | w1
            elif op == 0xFD:                                  # G_SETTIMG
                S["timg"] = dict(addr=w1, fmt=(w0 >> 21) & 7, siz=(w0 >> 19) & 3,
                                 width=(w0 & 0xFFF) + 1)
            elif op == 0xF5:                                  # G_SETTILE
                tl = S["tiles"][(w1 >> 24) & 7]
                tl.fmt = (w0 >> 21) & 7
                tl.siz = (w0 >> 19) & 3
                tl.line = (w0 >> 9) & 0x1FF
                tl.tmem = w0 & 0x1FF
                tl.pal = (w1 >> 20) & 0xF
                tl.cmT = (w1 >> 18) & 3
                tl.maskT = (w1 >> 14) & 0xF
                tl.shiftT = (w1 >> 10) & 0xF
                tl.cmS = (w1 >> 8) & 3
                tl.maskS = (w1 >> 4) & 0xF
                tl.shiftS = w1 & 0xF
            elif op == 0xF2:                                  # G_SETTILESIZE
                tl = S["tiles"][(w1 >> 24) & 7]
                tl.uls = (w0 >> 12) & 0xFFF
                tl.ult = w0 & 0xFFF
                tl.lrs = (w1 >> 12) & 0xFFF
                tl.lrt = w1 & 0xFFF
                tl.sized = True
            elif op in (0xF3, 0xF4):                          # G_LOADBLOCK / LOADTILE
                tl = S["tiles"][(w1 >> 24) & 7]
                if S["timg"] is not None:
                    ent = dict(S["timg"])
                    ent["how"] = "BLOCK" if op == 0xF3 else "TILE"
                    ent["uls"] = (w0 >> 12) & 0xFFF
                    ent["ult"] = w0 & 0xFFF
                    S["tmem"][tl.tmem] = ent
            elif op == 0xF0:                                  # G_LOADTLUT
                if S["timg"] is not None:
                    S["tlut"] = S["timg"]["addr"]
                    S["tlut_n"] = ((w1 >> 14) & 0x3FF) + 1
            elif op == 0xDE:                                  # G_DL
                p = resolve(w1)
                if p is not None and depth < 16:
                    run(p, depth + 1)
                if (w0 >> 16) & 0xFF == 1:                    # a branch: no return
                    return
            elif op == 0xDF:                                  # G_ENDDL
                return

    def _state():
        tl = S["tiles"][S["rtile"]]
        img = S["tmem"].get(tl.tmem)
        return dict(fmt=tl.fmt, siz=tl.siz, line=tl.line, tmem=tl.tmem, pal=tl.pal,
                    cmS=tl.cmS, cmT=tl.cmT, maskS=tl.maskS, maskT=tl.maskT,
                    shiftS=tl.shiftS, shiftT=tl.shiftT,
                    uls=tl.uls, ult=tl.ult, lrs=tl.lrs, lrt=tl.lrt, sized=tl.sized,
                    timg=img["addr"] if img else None,
                    timg_fmt=img["fmt"] if img else None,
                    timg_siz=img["siz"] if img else None,
                    timg_w=img["width"] if img else None,
                    load=img["how"] if img else None,
                    load_uls=img["uls"] if img else 0,
                    load_ult=img["ult"] if img else 0,
                    tlut=S["tlut"], tlut_n=S["tlut_n"],
                    scaleS=S["scale"][0], scaleT=S["scale"][1],
                    texon=S["texon"] != 0, prim=S["prim"], combine=S["combine"],
                    rendermode=S["rendermode"], geom=S["geom"])

    run(dl_off, 0)
    return verts, tris


# ------------------------------------------------------------------ textures
_BPP = {0: 4, 1: 8, 2: 16, 3: 32}


def _tex_size(st):
    """The tile's used size, in texels: G_SETTILESIZE, or its wrap mask when unsized."""
    if st["sized"]:
        w = ((st["lrs"] - st["uls"]) >> 2) + 1
        h = ((st["lrt"] - st["ult"]) >> 2) + 1
    else:
        w = (1 << st["maskS"]) if st["maskS"] else 0
        h = (1 << st["maskT"]) if st["maskT"] else 0
    return w, h


def _rgba5551(v):
    return R.rgba5551(v)


def _tlut_colours(tlut_rom):
    return [_rgba5551(struct.unpack_from(">H", ROM, tlut_rom + i * 2)[0])
            for i in range(256)]


def _decode(base, w, h, fmt, siz, stride, tlut, pal):
    """Plain linear texels from ROM (re-findings M5: no odd-row interleave to undo).
    Returns RGBA bytes or None for a format this cartridge's models do not use."""
    rom = ROM
    out = bytearray()
    bpp = _BPP[siz]
    rowbytes = stride * bpp // 8
    for y in range(h):
        p = base + y * rowbytes
        for x in range(w):
            if bpp == 4:
                q = p + x // 2
                b = rom[q] if q < len(rom) else 0
                n = (b >> 4) if x % 2 == 0 else (b & 15)
                if fmt == 2:                           # CI4
                    c = tlut[(pal << 4) | n] if tlut else (n * 17, n * 17, n * 17, 255)
                elif fmt == 3:                         # IA4 (3/1)
                    i = ((n >> 1) * 255) // 7
                    c = (i, i, i, 255 if n & 1 else 0)
                elif fmt == 4:                         # I4
                    i = n * 17
                    c = (i, i, i, i)
                else:
                    return None
            elif bpp == 8:
                q = p + x
                b = rom[q] if q < len(rom) else 0
                if fmt == 2:                           # CI8
                    c = tlut[b] if tlut else (b, b, b, 255)
                elif fmt == 3:                         # IA8 (4/4)
                    c = ((b >> 4) * 17,) * 3 + ((b & 15) * 17,)
                elif fmt == 4:                         # I8: the RDP puts I in alpha too
                    c = (b, b, b, b)
                else:
                    return None
            elif bpp == 16:
                q = p + x * 2
                v = struct.unpack_from(">H", rom, q)[0] if q + 2 <= len(rom) else 0
                if fmt == 0:                           # RGBA5551
                    c = _rgba5551(v)
                elif fmt == 3:                         # IA16
                    c = (v >> 8,) * 3 + (v & 0xFF,)
                else:
                    return None
            else:
                q = p + x * 4
                if fmt != 0:
                    return None
                c = tuple(rom[q:q + 4]) if q + 4 <= len(rom) else (0, 0, 0, 0)
            out += bytes(c)
    return bytes(out)


def decode_tex(st, resolve):
    """(key, w, h, rgba, rest) for the texture a walk state binds, or None.

    The data address is the image loaded at the render tile's TMEM address. A LOADBLOCK
    image is read in rows of the tile's own width (support/vx_rdp.py's note on the TMEM
    line stride: the RDP reads DRAM rows at the image width, which for a block load is
    the tile's); a LOADTILE one at the G_SETTIMG width, from its (uls, ult) corner. A CI
    texture with no G_LOADTLUT of its own reads GLOBAL_TLUT, which is the house table and
    is NOT in the key: the GDI walk must give the same keys as the Nod one."""
    if st.get("timg") is None:
        return None
    base = resolve(st["timg"])
    if base is None:
        return None
    w, h = _tex_size(st)
    if w <= 0 or h <= 0 or w > 1024 or h > 1024:
        return None
    fmt, siz = st["fmt"], st["siz"]
    if st.get("load") == "TILE" and st.get("timg_w"):
        stride = st["timg_w"]
        bpp = _BPP[siz]
        base += ((st["load_ult"] >> 2) * stride + (st["load_uls"] >> 2)) * bpp // 8
    else:
        stride = w
    tlut_rom = None
    tlut = None
    if fmt == 2:
        if st.get("tlut") is not None:
            tlut_rom = resolve(st["tlut"])
        tlut = _tlut_colours(tlut_rom if tlut_rom is not None else GLOBAL_TLUT)
    px = _decode(base, w, h, fmt, siz, stride, tlut,
                 st["pal"] if siz == 0 else 0)
    if px is None:
        return None
    key = ("tex", base, fmt, siz, w, h, stride, tlut_rom,
           st["pal"] if (fmt == 2 and siz == 0) else 0)
    return key, w, h, px, dict(cmS=st["cmS"], cmT=st["cmT"], maskS=st["maskS"],
                               maskT=st["maskT"])


MODE_OPAQUE, MODE_CUTOUT, MODE_SHADOW = 0, 1, 2

# Combiners that read no TEXEL0 in cycle 1 colour or alpha: a triangle drawn under one
# has no texture, whatever tile is set up (re-findings M5, textures.py's six combiners).
_NO_TEXEL = {(0xFC327E64, 0xFFFFF7FB),        # PRIM * SHADE, PRIM_A
             (0xFCFFFFFF, 0xFFFDF6FB)}        # PRIM, PRIM_A
_PRIM_TEXA = (0xFCFFFFFF, 0xFFFDF2F9)         # PRIM, TEXEL0_A
_TEX_ONLY = (0xFCFFFFFF, 0xFFFCF279)          # TEXEL0, TEXEL0_A


def _axis(cm, mask):
    """The pack's two wrap bits for one axis (cnc_eyes.cpp wrap_mode): bit 1 clamp,
    bit 0 mirror. The RDP clamps an axis whose mask is 0."""
    return (cm & 3) | (0 if mask else 2)


def bake_mesh(off, bank):
    """The triangle soup of one display list's call graph, in walk order."""
    resolve = R.make_resolve(off)
    verts, wt = walk(ROM, off, resolve)
    texcache = {}
    out = []
    for (a, b, c, st) in wt:
        ti = -1
        mode = MODE_OPAQUE
        wrap = 0
        tw = th = 1
        uls = ult = 0.0
        textured = (st["texon"] and st.get("timg") is not None
                    and st.get("combine") not in _NO_TEXEL)
        if textured:
            ck = (st["timg"], st["fmt"], st["siz"], st["pal"], st["tlut"], st["load"],
                  st["uls"], st["ult"], st["lrs"], st["lrt"], st["sized"],
                  st["maskS"], st["maskT"], st["timg_w"], st["load_uls"], st["load_ult"])
            if ck not in texcache:
                d = decode_tex(st, resolve)
                if d is None:
                    texcache[ck] = None
                else:
                    key, w, h, px, _rest = d
                    if st["fmt"] == 4:
                        md = MODE_SHADOW
                    elif any(px[i] == 0 for i in range(3, len(px), 4)):
                        md = MODE_CUTOUT
                    else:
                        md = MODE_OPAQUE
                    texcache[ck] = (bank.add_rgba(key, w, h, px), w, h, md)
            got = texcache[ck]
            if got is not None:
                ti, tw, th, mode = got
                wrap = _axis(st["cmS"], st["maskS"]) | (_axis(st["cmT"], st["maskT"]) << 2)
                uls = st["uls"] / 4.0 if st["sized"] else 0.0
                ult = st["ult"] / 4.0 if st["sized"] else 0.0
        # S10.5 texel coordinates. Every tile in the cartridge has shift 15 (x2) and every
        # G_TEXTURE scale 0x8000 (x0.5), which cancel (re-findings M5, "The UV rule"); the
        # product is kept general all the same.
        ks = (st["scaleS"] / 65536.0) * _shift_mul(st["shiftS"])
        kt = (st["scaleT"] / 65536.0) * _shift_mul(st["shiftT"])
        tri = []
        prim = st.get("prim")
        cc = st.get("combine")
        for vi in (a, b, c):
            x, y, z, u, v, r, g, bb, _al = verts[vi]
            # The vertex alpha slot is 0 in most of these lists and no combiner of theirs
            # reads SHADE_A (textures.py's six): the face's alpha is the texture's or
            # PRIM's, so the vertex carries 255 (re-findings' wall notes measured the
            # BRIK shadow plates at (211,211,211,255) from a vertex alpha of 0).
            al = 255
            if ti >= 0:
                uu = ((u / 32.0) * ks - uls) / tw
                vv = ((v / 32.0) * kt - ult) / th
                if cc == _PRIM_TEXA and prim is not None:
                    # rgb = PRIM, a = TEXEL0_A: the shadow decals. The vertex slot is not
                    # a colour there (a packed normal on the WOOD shadows).
                    r, g, bb = prim[:3]
            else:
                uu = vv = 0.0
                if prim is not None and (r, g, bb) == (0, 0, 0):
                    # the one place the PRIM colour is taken without a combiner: an
                    # untextured face whose shade is black (bake5's note on the PK4 baker)
                    r, g, bb, al = prim
            tri.append((float(x), float(y), float(z), uu, vv, r, g, bb, al))
        out.append((ti, mode, wrap, tuple(tri)))
    return out


def _shift_mul(s):
    """The RDP's tile shift: 1..10 divide by 2^s, 11..15 multiply by 2^(16-s)."""
    if s == 0:
        return 1.0
    if s <= 10:
        return 1.0 / (1 << s)
    return float(1 << (16 - s))


# ------------------------------------------------------------------ sprites
def bake_sprite(setname, anim, pal):
    """(w, h, rgba, frames, frame_h) of one infantry strip through one palette, or None
    when the cartridge has no such strip or the set has no such palette."""
    name = "%s_%s" % (setname, anim)
    rec = _sol_by_name().get(name)
    if rec is None or pal >= rec["npal"]:
        return None
    if not os.path.exists(os.path.join(SOL.IMG_D, name + ".IMG")):
        return None
    return SOL.decode(name, pal)


def load():
    return sys.modules[__name__]
