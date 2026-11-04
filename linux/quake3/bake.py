#!/usr/bin/env python3
"""Bakes one level of Quake III Arena's demo into the two files the game runs from here
(docs/games/quake3.md): a pak of the files the game itself reads, stored and not compressed, and a
file of what the renderer and the sound draw and play, as the machine's GPU and sound card
take it: textures as words or bytes they read where the file lies in the ROM, the level's
triangles sorted by what they are drawn with, the models' poses as packed vertices.

    python3 bake.py PAK0.PK3 WANTED.TXT OUT.PK3 OUT.DAT [--map q3dm17] [--texture 128] [--jpgraw PROGRAM]
        [--q3pack PROGRAM] [--quality 95] [--pictures packed|png|raw]

WANTED.TXT is what a run with QUAKE3_WANTED=1 printed: every file, model, picture and sound
the game asked for. Nothing else of the demo's 47 MB goes in.

A picture is rows of a sheet the game decodes into GPU memory when a level or the menu needs
it: a JPEG, or a PNG where its alpha is used (`png`: every one a PNG, the same texels as `raw`,
which writes them as the GPU reads them). Light maps stay raw.
"""
import io, math, os, re, struct, subprocess, sys, tempfile, zipfile, zlib
import numpy as np

# ---- arguments ----

args = sys.argv[1:]
def option(name, default):
    if name in args:
        i = args.index(name)
        value = args[i + 1]
        del args[i:i + 2]
        return value
    return default
MAP = option('--map', 'q3dm17')
TEXTURE_MAX = int(option('--texture', '128'))
PICTURE_MAX = int(option('--picture', '256'))
JPGRAW = option('--jpgraw', 'jpgraw')
BOTS = option('--bots', 'sarge').split(',')
SOUND_RATE = int(option('--sound-rate', '11025'))   # a sound of twice this or more is halved to it
Q3PACK = option('--q3pack', 'q3pack')
QUALITY = int(option('--quality', '95'))
PICTURES = option('--pictures', 'packed')
LIST = '--list' in args      # (also: the largest pictures and sounds, by name)
if LIST: args.remove('--list')
PAK, WANTED, OUT_PAK, OUT_DAT = args

pak = zipfile.ZipFile(PAK)
names = {n.lower(): n for n in pak.namelist()}
def has(name): return name.lower() in names
def read(name): return pak.read(names[name.lower()])

def clean(name):
    """A game path as the tables have it: lower case, forward slashes, no extension."""
    name = name.lower().replace('\\', '/')
    dot = name.rfind('.')
    return name[:dot] if dot > name.rfind('/') else name

# ---- pictures ----

def read_tga(data):
    idlen, cmtype, kind, cmfirst, cmlen, cmbits, x0, y0, w, h, bpp, desc = struct.unpack_from('<BBBHHBHHHHBB', data, 0)
    at = 18 + idlen + cmlen * ((cmbits + 7) // 8)
    n = bpp // 8
    if kind in (2, 3):
        px = np.frombuffer(data, np.uint8, w * h * n, at).reshape(h, w, n)
    elif kind in (10, 11):
        out = np.zeros((w * h, n), np.uint8)
        i = 0
        while i < w * h:
            head = data[at]; at += 1
            count = (head & 127) + 1
            if head & 128:
                out[i:i + count] = np.frombuffer(data, np.uint8, n, at); at += n
            else:
                out[i:i + count] = np.frombuffer(data, np.uint8, count * n, at).reshape(count, n); at += count * n
            i += count
        px = out.reshape(h, w, n)
    else:
        raise ValueError('a TGA of kind %d' % kind)
    if not desc & 0x20: px = px[::-1]
    rgba = np.full((h, w, 4), 255, np.uint8)
    if n == 1: rgba[..., :3] = px
    else:
        rgba[..., 0], rgba[..., 1], rgba[..., 2] = px[..., 2], px[..., 1], px[..., 0]
        if n == 4: rgba[..., 3] = px[..., 3]
    return rgba

def read_jpg(data):
    with tempfile.TemporaryDirectory() as d:
        open(d + '/in.jpg', 'wb').write(data)
        subprocess.run([JPGRAW, d + '/in.jpg', d + '/out.raw'], check=True)
        raw = open(d + '/out.raw', 'rb').read()
    line = raw.index(b'\n')
    w, h = map(int, raw[:line].split())
    rgba = np.full((h, w, 4), 255, np.uint8)
    rgba[..., :3] = np.frombuffer(raw, np.uint8, w * h * 3, line + 1).reshape(h, w, 3)
    return rgba

pictures = {}
def picture(name):
    """The picture of that game name (with or without an extension), RGBA; None if the demo has none."""
    key = clean(name)
    if key not in pictures:
        px = None
        if key in ('$whiteimage', '*white', 'white'):
            px = np.full((8, 8, 4), 255, np.uint8)
        elif has(key + '.tga'): px = read_tga(read(key + '.tga'))
        elif has(key + '.jpg'): px = read_jpg(read(key + '.jpg'))
        pictures[key] = px
    return pictures[key]

def smaller(px, most):
    """Halved (each pixel the mean of four) until neither side is over `most`."""
    while max(px.shape[0], px.shape[1]) > most and px.shape[0] % 2 == 0 and px.shape[1] % 2 == 0:
        px = px.astype(np.uint16).reshape(px.shape[0] // 2, 2, px.shape[1] // 2, 2, 4).sum(axis=(1, 3))
        px = ((px + 2) // 4).astype(np.uint8)
    return px

# ---- the file ----

blob = bytearray()       # everything the tables point into, placed after them
sizes_by_kind, kind_now = {}, ['pictures']
def place(data, align=16):
    while len(blob) % align: blob.append(0)
    at = len(blob)
    blob.extend(data)
    sizes_by_kind[kind_now[0]] = sizes_by_kind.get(kind_now[0], 0) + len(data)
    return at            # (from the blob's start: made a file offset when the file is written)

RGB24, WORDS = 0, 1
RAW, JPEG, PNG = 0, 1, 2      # how a texture or a sheet lies in the file
KEY = 0xff000000         # a texel that is a hole, in a texture of words
textures, texture_of = [], {}
texture_copies = []      # (row, the texture it is a copy of): a stage's frames are rows one after another (stage_textures)
packed_report = {'jpeg': [0, 0, 0, 0.0, 0], 'png': [0, 0, 0]}   # sheets, texels' bytes, files' bytes (and JPEG's error, worst)

def q3pack(*a):
    subprocess.run([Q3PACK] + [str(x) for x in a], check=True)

def packed_file(raw, w, h, channels, as_jpeg):
    """Texels (rows of `channels` bytes, as the GPU reads them) as a file of q3pack's; None when
    libmedia does not read it back (a PNG: not the same texels)."""
    with tempfile.TemporaryDirectory() as d:
        open(d + '/in.raw', 'wb').write(raw)
        if as_jpeg: q3pack('jpeg', QUALITY, w, h, d + '/in.raw', d + '/out')
        else: q3pack('png', w, h, channels, d + '/in.raw', d + '/out')
        if subprocess.run([Q3PACK, 'decode', str(channels), d + '/out', d + '/back.raw'], stderr=subprocess.DEVNULL).returncode:
            return None
        error = np.abs(np.frombuffer(open(d + '/back.raw', 'rb').read(), np.uint8).astype(np.int16) - np.frombuffer(raw, np.uint8))
        if not as_jpeg and error.any(): return None
        data = open(d + '/out', 'rb').read()
    r = packed_report['jpeg' if as_jpeg else 'png']
    r[0] += 1; r[1] += len(raw); r[2] += len(data)
    if as_jpeg: r[3] += float(error.sum()); r[4] = max(r[4], int(error.max()))
    return data

# Packed textures of one width and kind are laid one under another in sheets: a sheet is one
# file the game decodes on every core at once, and each texture is rows of it (sheets()).
SHEET_MOST = int(option('--sheet', str(512 << 10)))   # a sheet's texels' bytes at most
sheet_wanted = []         # (texture, texel rows, width, height, channels, as JPEG)

def texture(px, kind, key=None, packed=True):
    """A picture as a texture of the file: 'rgb' three bytes a texel; 'words' with its alpha as
    transparency; 'keyed' words where alpha under a half is a hole. Returns its number.
    Packed, it is rows of a JPEG (rgb) or a PNG (words) that decodes to those texels (words:
    blue, green, red, transparency, a PNG's RGBA with T for alpha)."""
    if key and key in texture_of: return texture_of[key]
    h, w = px.shape[:2]
    if kind == 'rgb':
        raw, fmt, flags, channels = px[..., :3].tobytes(), RGB24, 0, 3
    else:
        p = px.astype(np.uint32)
        words = (255 - p[..., 3]) << 24 | p[..., 0] << 16 | p[..., 1] << 8 | p[..., 2]
        flags = 1
        if kind == 'keyed':
            words = np.where(p[..., 3] < 128, KEY, words & 0xffffff)
            flags = 2
        raw, fmt, channels = words.astype('<u4').view(np.uint8).reshape(h, w, 4)[..., [2, 1, 0, 3]].tobytes(), WORDS, 4
        if not packed or PICTURES == 'raw':
            raw = words.astype('<u4').tobytes()
    if not packed or PICTURES == 'raw':
        textures.append([place(raw, 4 if fmt == RGB24 else 16), w, h, fmt, flags, 0xffff, KEY if kind == 'keyed' else 0])
    else:
        textures.append([0, w, h, fmt, flags, 0xffff, KEY if kind == 'keyed' else 0])
        sheet_wanted.append((len(textures) - 1, raw, w, h, channels, kind == 'rgb' and PICTURES == 'packed'))
    if key: texture_of[key] = len(textures) - 1
    return len(textures) - 1

sheets = []               # (at, bytes, width, height, channels, packing, 0)

def make_sheets():
    """The packed textures' sheets: by kind and width, at most SHEET_MOST texels' bytes each. A
    JPEG's textures begin on its blocks (a texture's last row is repeated to the next)."""
    block = 8 if QUALITY >= 90 else 16     # (q3pack's: colours at half size under 90)
    was, kind_now[0] = kind_now[0], 'pictures'
    groups = {}
    for t in sheet_wanted: groups.setdefault((t[5], t[4], t[2]), []).append(t)
    for (as_jpeg, channels, w), items in sorted(groups.items()):
        while items:
            rows, take = 0, []
            for t in items:
                tall = -(-t[3] // block) * block if as_jpeg else t[3]
                if take and (rows + tall) * w * channels > SHEET_MOST: break
                take.append((t, rows))
                rows += tall
            items = items[len(take):]
            px = np.zeros((rows, w * channels), np.uint8)
            for t, y in take:
                px[y:y + t[3]] = np.frombuffer(t[1], np.uint8).reshape(t[3], w * channels)
                px[y + t[3]:y + -(-t[3] // block) * block if as_jpeg else y + t[3]] = px[y + t[3] - 1]
                textures[t[0]][0], textures[t[0]][5] = y * w * channels, len(sheets)
            data = packed_file(px.tobytes(), w, rows, channels, as_jpeg)
            if data is None:
                # (as libmedia wrote it, it does not read it back: its textures stay as the GPU reads them)
                print('%s: a sheet of %d x %d is not read back by libmedia: %d textures left as texels' % (os.path.basename(OUT_DAT), w, rows, len(take)))
                for t, y in take:
                    raw = np.frombuffer(t[1], np.uint8).reshape(-1, channels)
                    textures[t[0]][0] = place((raw[:, [2, 1, 0, 3]] if channels == 4 else raw).tobytes(), 16)
                    textures[t[0]][5] = 0xffff
                continue
            sheets.append((place(data, 16), len(data), w, rows, channels, JPEG if as_jpeg else PNG, 0))
    kind_now[0] = was

# ---- shader scripts ----

def tokens(text):
    text = re.sub(r'//[^\n]*', '', text)
    return re.findall(r'[{}]|[^\s{}]+', text)

scripts = {}
for n in sorted(names):
    if n.startswith('scripts/') and n.endswith('.shader'):
        t = tokens(read(n).decode('latin1'))
        i = 0
        while i < len(t):
            name = clean(t[i]); i += 1
            if i >= len(t) or t[i] != '{': continue
            i += 1
            general, stages, line = [], [], []
            # (a directive ends where the next one's line began: the lines are found again from the text)
            depth = 1
            body = []
            while i < len(t) and depth:
                if t[i] == '{': depth += 1
                elif t[i] == '}': depth -= 1
                body.append(t[i]); i += 1
            scripts.setdefault(name, (n, body[:-1]))

def script_lines(name):
    """A shader's script as its own directives and its stages', each a list of lines of words."""
    source, _ = scripts[name]
    text = re.sub(r'//[^\n]*', '', read(source).decode('latin1'))
    m = re.search(r'(?im)^[ \t]*' + re.escape(name) + r'[ \t\r]*$', text) or re.search(r'(?i)(?<![\w/])' + re.escape(name) + r'\s*\{', text)
    i = text.index('{', m.start())
    depth, general, stages, current, start = 0, [], [], None, i + 1
    for j in range(i, len(text)):
        c = text[j]
        if c in '{}\n':
            words = text[start:j].split()
            if words: (current if current is not None else general).append([words[0].lower()] + words[1:])
            start = j + 1
            if c == '{':
                depth += 1
                if depth == 2: current = []
            elif c == '}':
                depth -= 1
                if depth == 1: stages.append(current); current = None
                if depth == 0: break
    return general, stages

WAVES = {'sin': 1, 'triangle': 2, 'square': 3, 'sawtooth': 4, 'inversesawtooth': 5, 'noise': 6}
OPAQUE, BLEND, ADD, FILTER = 0, 1, 2, 3
MASKED = 9   # the GPU's pass 9 (shader_vm/src/gpu.h): GL_DST_COLOR GL_ONE_MINUS_DST_ALPHA, by what is there's alpha
S_KEYED, S_LIGHTMAP, S_ENV, S_CLAMP, S_IMPLICIT, S_INVERT = 1, 2, 4, 8, 16, 32
RGB_IDENTITY, RGB_VERTEX, RGB_ENTITY, RGB_ONE_MINUS_ENTITY, RGB_DIFFUSE, RGB_WAVE, RGB_CONST = range(7)
A_IDENTITY, A_VERTEX, A_ENTITY, A_ONE_MINUS_ENTITY, A_WAVE, A_CONST = range(6)
TC_SCROLL, TC_SCALE, TC_ROTATE, TC_STRETCH, TC_TURB = 1, 2, 3, 4, 5
F_SKY, F_OFFSET, F_AUTOSPRITE, F_NODRAW, F_2D = 1, 2, 4, 8, 16

def number(s):
    try: return float(s)
    except ValueError: return 0.0

def parse_stage(lines):
    s = dict(map=None, frames=[], fps=0.0, blend=None, flags=0, rgb=None, alpha=A_IDENTITY, rgbwave=(0, 0, 0, 0, 0),
             alphawave=(0, 0, 0, 0, 0), const=[255, 255, 255, 255], tcmods=[], video=False)
    for l in lines:
        k, a = l[0], [x.lower() for x in l[1:]]
        if k in ('map', 'clampmap') and a:
            s['map'] = a[0]; s['frames'] = [a[0]]
            if k == 'clampmap': s['flags'] |= S_CLAMP
            if a[0] == '$lightmap': s['flags'] |= S_LIGHTMAP
        elif k == 'animmap' and len(a) > 1:
            s['fps'] = number(a[0]); s['frames'] = a[1:9]; s['map'] = a[1]
        elif k == 'videomap': s['video'] = True
        elif k == 'blendfunc' and a:
            if a[0] in ('add', 'blend', 'filter'): s['blend'] = {'add': ('gl_one', 'gl_one'), 'blend': ('gl_src_alpha', 'gl_one_minus_src_alpha'), 'filter': ('gl_dst_color', 'gl_zero')}[a[0]]
            elif len(a) > 1: s['blend'] = (a[0], a[1])
        elif k == 'alphafunc': s['flags'] |= S_KEYED
        elif k == 'rgbgen' and a:
            if a[0] == 'wave' and len(a) >= 6: s['rgb'] = RGB_WAVE; s['rgbwave'] = (WAVES.get(a[1], 1),) + tuple(number(x) for x in a[2:6])
            elif a[0] in ('vertex', 'exactvertex'): s['rgb'] = RGB_VERTEX
            elif a[0] == 'entity': s['rgb'] = RGB_ENTITY
            elif a[0] == 'oneminusentity': s['rgb'] = RGB_ONE_MINUS_ENTITY
            elif a[0] == 'lightingdiffuse': s['rgb'] = RGB_DIFFUSE
            elif a[0] == 'const':
                v = [number(x) for x in a if x not in ('const', '(', ')')][:3]
                s['rgb'] = RGB_CONST; s['const'][:len(v)] = [max(0, min(255, int(x * 255))) for x in v]
            else: s['rgb'] = RGB_IDENTITY
        elif k == 'alphagen' and a:
            if a[0] == 'wave' and len(a) >= 6: s['alpha'] = A_WAVE; s['alphawave'] = (WAVES.get(a[1], 1),) + tuple(number(x) for x in a[2:6])
            elif a[0] == 'vertex': s['alpha'] = A_VERTEX
            elif a[0] == 'entity': s['alpha'] = A_ENTITY
            elif a[0] == 'oneminusentity': s['alpha'] = A_ONE_MINUS_ENTITY
            elif a[0] == 'const' and len(a) > 1: s['alpha'] = A_CONST; s['const'][3] = max(0, min(255, int(number(a[1]) * 255)))
        elif k in ('tcgen', 'texgen') and a and a[0] == 'environment': s['flags'] |= S_ENV
        elif k == 'tcmod' and a:
            p = [number(x) for x in a[1:]] + [0, 0, 0, 0, 0]
            if a[0] == 'scroll': s['tcmods'].append((TC_SCROLL, 0, p[0], p[1], 0, 0))
            elif a[0] == 'scale': s['tcmods'].append((TC_SCALE, 0, p[0], p[1], 0, 0))
            elif a[0] == 'rotate': s['tcmods'].append((TC_ROTATE, 0, p[0], 0, 0, 0))
            elif a[0] == 'stretch': s['tcmods'].append((TC_STRETCH, WAVES.get(a[1] if len(a) > 1 else '', 1), p[1], p[2], p[3], p[4]))
            elif a[0] == 'turb':
                q = p[1:] if len(a) > 1 and a[1] in WAVES else p   # (an old form names a wave first)
                s['tcmods'].append((TC_TURB, 0, q[0], q[1], q[2], q[3]))
    return s

def blend_pass(blend):
    """Which of the GPU's four ways of drawing over what is there a blendFunc comes nearest to,
    and whether the texture is to be turned inside out for it."""
    if not blend: return OPAQUE, False
    src, dst = blend
    if dst == 'gl_zero' and src == 'gl_one': return OPAQUE, False
    if src == 'gl_dst_color' and dst == 'gl_one_minus_dst_alpha': return MASKED, False
    if src == 'gl_dst_color' or dst == 'gl_src_color': return FILTER, False
    if src == 'gl_zero' and dst == 'gl_one_minus_src_color': return FILTER, True
    if dst == 'gl_one_minus_src_alpha': return BLEND, False
    if src == 'gl_one_minus_src_alpha' and dst == 'gl_src_alpha': return BLEND, True
    if dst == 'gl_one': return ADD, False
    if dst == 'gl_one_minus_src_color': return ADD, False
    return BLEND, False

shaders, shader_of, stages_out = [], {}, []

def has_alpha(px): return px is not None and int(px[..., 3].min()) < 250

def stage_textures(s, most, pass_, invert, over=None, alpha_kept=False):
    """The textures of a stage's pictures, in the kind its way of drawing needs. None: no picture.
    alpha_kept: a later stage goes by the picture's alpha (MASKED), so an opaque one keeps its alpha too."""
    out = []
    for f in s['frames']:
        if f == '$lightmap': return []
        px = picture(f)
        if px is None: return None
        kind = 'keyed' if s['flags'] & S_KEYED else 'words' if pass_ in (BLEND, ADD) and has_alpha(px) and s['blend'] and 'alpha' in ''.join(s['blend']) else 'rgb'
        if kind == 'rgb' and alpha_kept and pass_ == OPAQUE and has_alpha(px): kind = 'words'
        key = (clean(f), kind, most, invert, over)
        if key not in texture_of:
            px = smaller(px, most)
            if over is not None:    # drawn over a stage that cannot be drawn (a reflection): over its colour, once and for all
                a = px[..., 3:4].astype(np.float32) / 255
                px = px.copy()
                px[..., :3] = (px[..., :3] * a + np.array(over, np.float32) * (1 - a)).astype(np.uint8)
                px[..., 3] = 255
                kind = 'rgb'
            if invert:
                px = px.copy()
                if pass_ == FILTER: px[..., :3] = 255 - px[..., :3]
                else: px[..., 3] = 255 - px[..., 3]
            texture(px, kind, key)
        out.append(texture_of[key])
    if len(out) > 1 and out != list(range(out[0], out[0] + len(out))):
        # The renderer takes a stage's pictures as textures one after another (st->texture + frame): a frame made
        # before (by another stage, or the same picture twice in an animMap) is a row of its own here, a copy of that
        # texture's, made once the sheets have placed it. Taken as they were, the frames after it were whatever
        # textures came next: railexplosion's last frames were rocketexplosion's (opaque, by alpha), launchpad_diamond's
        # arrows three pictures of other shaders, bulletexplosion's and rocketexplosion's last frames whole squares.
        first = len(textures)
        for t in out:
            textures.append(None)
            texture_copies.append((len(textures) - 1, t))
        out = list(range(first, first + len(out)))
    return out

def shader(name, context='3d'):
    was, kind_now[0] = kind_now[0], 'pictures'
    try:
        return shader_made(name, context)
    finally:
        kind_now[0] = was

def shader_made(name, context='3d'):
    """The shader of that name as the file has it: from its script, or for a picture's name
    the shader the game would make of it. Returns its number, or -1 when there is nothing to draw with."""
    name = clean(name)
    key = name
    if key in shader_of: return shader_of[key]
    most = PICTURE_MAX if context == '2d' else TEXTURE_MAX
    flags, made = 0, []
    if name in scripts:
        general, stage_lines = script_lines(name)
        words = [' '.join(l).lower() for l in general]
        if any(w.startswith('surfaceparm nodraw') for w in words): flags |= F_NODRAW
        if any(w.startswith('surfaceparm sky') or w.startswith('skyparms') for w in words): flags |= F_SKY
        if any(w.startswith('polygonoffset') for w in words): flags |= F_OFFSET
        if any(w.startswith('deformvertexes autosprite') for w in words): flags |= F_AUTOSPRITE
        if any(w.startswith('nomipmaps') or w.startswith('nopicmip') for w in words): most = max(most, PICTURE_MAX)
        parsed = [parse_stage(l) for l in stage_lines]
        parsed = [s for s in parsed if s['map'] and not s['video']]
        # a reflection cannot be drawn: left out, and what is blended over it is laid on its colour
        over = None
        if len(parsed) > 1:
            kept = []
            for i, s in enumerate(parsed):
                if s['flags'] & S_ENV:
                    px = picture(s['map'])
                    if i == 0 and px is not None and blend_pass(s['blend'])[0] == OPAQUE:
                        over = tuple(int(v) for v in px[..., :3].reshape(-1, 3).mean(axis=0))
                    continue
                kept.append(s)
            if kept: parsed = kept
        masked = [i for i, s in enumerate(parsed) if blend_pass(s['blend'])[0] == MASKED]
        for i, s in enumerate(parsed):
            pass_, invert = blend_pass(s['blend'])
            lay = over if (over is not None and i == 0 and pass_ == BLEND) else None
            if lay is not None: pass_ = OPAQUE
            tex = stage_textures(s, most, pass_, invert, lay, bool(masked) and i < masked[0])
            if tex is None: continue
            rgb = s['rgb']
            if rgb is None: rgb = RGB_IDENTITY
            made.append(dict(s, textures=tex, pass_=pass_, rgb=rgb))
    else:
        px = picture(name)
        if px is None:
            shader_of[key] = -1
            return -1
        px = smaller(px, most)
        blended = has_alpha(px)
        tkey = (name, 'words' if blended else 'rgb', most, False, None)
        tex = texture(px, 'words' if blended else 'rgb', tkey)
        s = parse_stage([])
        s['flags'] |= S_IMPLICIT
        made.append(dict(s, textures=[tex], pass_=BLEND if blended and context == '2d' else OPAQUE, rgb=RGB_DIFFUSE))
        if context == '2d': flags |= F_2D
    first = len(stages_out)
    stages_out.extend(made)
    shaders.append((name, flags, first, len(made)))
    shader_of[key] = len(shaders) - 1
    return shader_of[key]

# ---- what the game asked for ----

wanted = {}
for line in open(WANTED, encoding='latin1'):
    m = re.match(r'\s*(?:\d+\s+)?(?:wanted:\s+)?(file|model|shader|shader2d|skin|sound)\s+(\S+)', line)
    if m: wanted.setdefault(m.group(1), []).append(m.group(2))
for k in wanted: wanted[k] = sorted(set(wanted[k]))

TWO_D = ('gfx/2d/', 'menu/', 'icons/', 'ui/', 'levelshots/', 'gfx/colors/')
for n in wanted.get('shader2d', []): shader(n, '2d')
for n in wanted.get('shader', []): shader(n, '2d' if clean(n).startswith(TWO_D) or clean(n) in ('white', 'console') else '3d')
shader('white', '2d')

# ---- models ----

# The directions a vertex's normal is one of: a vertex is tagged with the nearest, and a model
# is lit by a table of the light at each (docs/machine/gpu.md, tagged vertices).
NORMALS = 256
k = np.arange(NORMALS) + 0.5
phi, z = k * math.pi * (3 - math.sqrt(5)), 1 - 2 * k / NORMALS
normal_table = np.stack([np.cos(phi) * np.sqrt(1 - z * z), np.sin(phi) * np.sqrt(1 - z * z), z], axis=1).astype(np.float32)

models, surfaces_out, skins, skin_entries = [], [], [], []

def packed_coords(st, whole=None):
    """Texture coordinates of triangles (n, 3, 2) as the words packed and tagged vertices have:
    12 bits each of 1,024ths, whole repeats taken off a triangle's three (or `whole` off all)."""
    st = st - (np.floor(st.min(axis=1, keepdims=True)) if whole is None else whole)
    q = np.clip(np.round(st * 1024), 0, 4095).astype(np.uint32)
    return (q[..., 0] | q[..., 1] << 12).reshape(-1)

def model(name):
    kind_now[0] = 'models'
    data = read(name)
    ident, version, _, mflags, nframes, ntags, nsurfs, nskins, oframes, otags, osurfs, oend = struct.unpack_from('<4si64siiiiiiiii', data, 0)
    if ident != b'IDP3': return
    frames = np.frombuffer(data, '<f4', nframes * 14, oframes).reshape(nframes, 14)   # bounds, origin, radius, (name)
    frame_info = np.concatenate([frames[:, 6:9], frames[:, 9:10]], axis=1).astype('<f4')
    tag_names = [data[otags + 112 * i:otags + 112 * i + 64].split(b'\0')[0] for i in range(ntags)]
    tags = np.zeros((nframes, ntags, 12), '<f4')
    for f in range(nframes):
        for t in range(ntags):
            tags[f, t] = np.frombuffer(data, '<f4', 12, otags + 112 * (f * ntags + t) + 64)
    surfs, at = [], osurfs
    for i in range(nsurfs):
        sid, sname, sflags, sframes, sshaders, sverts, stris, otris, oshaders, ost, oxyz, send = struct.unpack_from('<4s64siiiiiiiiii', data, at)
        tris = np.frombuffer(data, '<i4', stris * 3, at + otris).reshape(stris, 3)
        st = np.frombuffer(data, '<f4', sverts * 2, at + ost).reshape(sverts, 2)
        xyzn = np.frombuffer(data, '<i2', sframes * sverts * 4, at + oxyz).reshape(sframes, sverts, 4)
        sshader = data[at + oshaders:at + oshaders + 64].split(b'\0')[0].decode('latin1') if sshaders else ''
        surfs.append((sname.split(b'\0')[0].decode('latin1').lower(), sshader, tris, st, xyzn))
        at += send
    if surfs:
        every = np.concatenate([s[4][..., :3].reshape(-1, 3) for s in surfs]).astype(np.float32) / 64
        low, high = every.min(axis=0), every.max(axis=0)
    else:
        low, high = np.zeros(3, np.float32), np.ones(3, np.float32)
    scale = np.maximum(high - low, 1e-3) / 255
    first = len(surfaces_out)
    for sname, sshader, tris, st, xyzn in surfs:
        flat = tris.reshape(-1)
        xyz = xyzn[:, flat, :3].astype(np.float32) / 64
        q = np.clip(np.round((xyz - low) / scale), 0, 255).astype(np.uint32)
        lat = (xyzn[:, flat, 3].astype(np.int32) >> 8 & 255) * (2 * math.pi / 255)
        lng = (xyzn[:, flat, 3].astype(np.int32) & 255) * (2 * math.pi / 255)
        normal = np.stack([np.cos(lat) * np.sin(lng), np.sin(lat) * np.sin(lng), np.cos(lng)], axis=2).astype(np.float32)
        tag = np.argmax(normal @ normal_table.T, axis=2).astype(np.uint32)
        words = (q[..., 0] | q[..., 1] << 8 | q[..., 2] << 16 | tag << 24).astype('<u4')
        coords = packed_coords(st[tris]).astype('<u4')
        surfaces_out.append((sname, shader(sshader) if sshader else -1, len(flat), place(coords.tobytes()), place(words.tobytes())))
    tag_at = place(b''.join(n.ljust(64, b'\0') for n in tag_names) + tags.tobytes(), 4) if ntags else 0
    models.append((clean(name) + '.md3', nframes, nsurfs, ntags, first, tag_at, place(frame_info.tobytes(), 4), scale, low,
                   frames[0, 0:3], frames[0, 3:6]))

for n in wanted.get('model', []):
    if has(n) and n.lower().endswith('.md3'): model(n)

def skin(name):
    first = len(skin_entries)
    for line in read(name).decode('latin1').splitlines():
        part = [p.strip() for p in line.split(',')]
        if len(part) == 2 and part[0] and not part[0].lower().startswith('tag_'):
            skin_entries.append((part[0].lower(), shader(part[1]) if part[1] else -1))
    skins.append((clean(name) + '.skin', first, len(skin_entries) - first))

for n in wanted.get('skin', []):
    if has(n): skin(n)

# ---- the level ----

bsp = read('maps/%s.bsp' % MAP)
lumps = [struct.unpack_from('<ii', bsp, 8 + 8 * i) for i in range(17)]
def lump(i): return bsp[lumps[i][0]:lumps[i][0] + lumps[i][1]]
L_ENTITIES, L_SHADERS, L_PLANES, L_NODES, L_LEAFS, L_LEAFSURFACES, L_LEAFBRUSHES, L_MODELS, L_BRUSHES, L_BRUSHSIDES, L_VERTS, L_INDEXES, L_FOGS, L_SURFACES, L_LIGHTMAPS, L_LIGHTGRID, L_VIS = range(17)

def brighter(rgb):
    """The game's lighting bytes as it shows them in a window: four times as bright, and a
    colour that would pass white kept its colour."""
    c = rgb.astype(np.float32) * 4
    most = np.maximum(c.max(axis=-1, keepdims=True), 255)
    return (c * 255 / most).astype(np.uint8)

kind_now[0] = 'light maps'
light_pages = []
pages = np.frombuffer(lump(L_LIGHTMAPS), np.uint8).reshape(-1, 128, 128, 3)
for p in pages:
    px = np.full((128, 128, 4), 255, np.uint8)
    px[..., :3] = brighter(p)
    light_pages.append(texture(px, 'rgb', packed=False))    # (read where the file lies: AGENTS.md)

bsp_shaders = [lump(L_SHADERS)[i:i + 64].split(b'\0')[0].decode('latin1') for i in range(0, lumps[L_SHADERS][1], 72)]
verts = np.frombuffer(lump(L_VERTS), np.dtype([('xyz', '<f4', 3), ('st', '<f4', 2), ('lm', '<f4', 2), ('normal', '<f4', 3), ('colour', 'u1', 4)]))
indexes = np.frombuffer(lump(L_INDEXES), '<i4')
bsp_surfaces = [struct.unpack_from('<12i3f9f2i', lump(L_SURFACES), i) for i in range(0, lumps[L_SURFACES][1], 104)]
bsp_models = [struct.unpack_from('<6f4i', lump(L_MODELS), i) for i in range(0, lumps[L_MODELS][1], 40)]

PATCH_STEPS = 4     # a patch's curve between three control points, as so many straight pieces

def patch_triangles(first, width, height):
    """A patch's control points made a grid of vertices (xyz, st, lm, colour as rows of 11
    numbers) and its triangles' corners as rows of that grid."""
    c = np.concatenate([verts['xyz'][first:first + width * height], verts['st'][first:first + width * height],
                        verts['lm'][first:first + width * height], verts['colour'][first:first + width * height].astype(np.float32)], axis=1)
    c = c.reshape(height, width, 11)
    t = np.linspace(0, 1, PATCH_STEPS + 1)
    basis = np.stack([(1 - t) ** 2, 2 * t * (1 - t), t * t], axis=1)      # (steps + 1, 3)
    out = []
    for by in range(0, height - 1, 2):
        for bx in range(0, width - 1, 2):
            block = c[by:by + 3, bx:bx + 3]
            grid = np.einsum('ia,jb,abk->ijk', basis, basis, block)
            n = PATCH_STEPS + 1
            for i in range(PATCH_STEPS):
                for j in range(PATCH_STEPS):
                    a, b, d, e = grid[i, j], grid[i, j + 1], grid[i + 1, j], grid[i + 1, j + 1]
                    out += [a, d, b, b, d, e]
    return np.array(out, np.float32).reshape(-1, 11)

R_LIGHT, R_TAGGED, R_MOVING, R_SKY, R_SPRITES = 1, 2, 4, 8, 16
runs = {}       # (model, shader, stage in the shader, light page) -> rows of 11 numbers, three a triangle

def surface_triangles(s):
    kind, first, count, first_index, index_count = s[2], s[3], s[4], s[5], s[6]
    if kind in (1, 3):
        at = first + indexes[first_index:first_index + index_count]
        return np.concatenate([verts['xyz'][at], verts['st'][at], verts['lm'][at], verts['colour'][at].astype(np.float32)], axis=1)
    if kind == 2:
        return patch_triangles(first, s[24], s[25])
    return None

def stage_runs(sh):
    """How a shader's stages are drawn on the level: (stage, with the light map, pass) for each
    draw. A light map and the texture it lights are one draw (docs/machine/gpu.md, a lit draw)."""
    name, flags, first, count = shaders[sh]
    st = stages_out[first:first + count]
    light = [i for i, s in enumerate(st) if s['flags'] & S_LIGHTMAP]
    base = None
    if light and st[light[0]]['pass_'] == MASKED:
        # the light map over what the stages before it made, by its alpha (Quake III's glows under a grate): a draw of
        # its own, the light map times white, after them
        return [(i, bool(s['flags'] & S_LIGHTMAP), s['pass_']) for i, s in enumerate(st) if i == light[0] or not s['flags'] & S_LIGHTMAP]
    if light:
        l = light[0]
        if l + 1 < count and st[l + 1]['pass_'] == FILTER and not st[l + 1]['flags'] & S_LIGHTMAP: base = l + 1
        else:
            before = [i for i in range(l) if st[i]['pass_'] == OPAQUE]
            if before: base = before[-1]
    elif count == 1 and st[0]['flags'] & S_IMPLICIT:
        base = 0      # a texture with no script: the game lights it by the surface's light map
    out = []
    for i, s in enumerate(st):
        if s['flags'] & S_LIGHTMAP: continue      # (alone it is not drawn: only with the texture it lights)
        if i == base: out.append((i, True, OPAQUE))
        else: out.append((i, False, s['pass_']))
    return out

kind_now[0] = 'pictures'
world_models = []
for m, bm in enumerate(bsp_models):
    for s in bsp_surfaces[bm[6]:bm[6] + bm[7]]:
        sh = shader(bsp_shaders[s[0]])
        if sh < 0 or shaders[sh][1] & F_NODRAW: continue
        tri = surface_triangles(s)
        if tri is None or not len(tri): continue
        if shaders[sh][1] & F_OFFSET:
            # a mark on a wall: a little off it, the way it faces (its own triangles say which way)
            p = tri[:, :3].reshape(-1, 3, 3)
            n = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
            n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-6)
            tri = tri.copy()
            tri[:, :3] -= np.repeat(n, 3, axis=0) * 0.25
        if shaders[sh][1] & F_SKY:
            runs.setdefault((m, sh, 0, -2), []).append(tri)
            continue
        for stage, lit, pass_ in stage_runs(sh):
            runs.setdefault((m, sh, stage, s[7] if lit and 0 <= s[7] < len(light_pages) else -1), []).append(tri)

def compact_coords(st, whole=None):
    """Texture coordinates of triangles (n, 3, 2) as compact vertices have them: 16 bits each
    of 1,024ths, whole repeats taken off a triangle's three so that they are within 32 (or
    `whole` off all of them)."""
    st = st - (np.floor(st.min(axis=1, keepdims=True)) if whole is None else whole)
    q = np.clip(np.round(st * 1024), -32768, 32767).astype(np.int32)
    return ((q[..., 0] & 0xffff) | (q[..., 1] & 0xffff) << 16).astype(np.uint32).reshape(-1)

kind_now[0] = 'the level'
world_vertices = bytearray()
runs_out = []
for (m, sh, stage, page) in sorted(runs, key=lambda k: (k[0], stages_out[shaders[k[1]][2] + k[2]]['pass_'] if k[3] != -2 and not (k[3] >= 0) else 0, k[1], k[2], k[3])):
    tri = np.concatenate(runs[(m, sh, stage, page)])
    s = stages_out[shaders[sh][2] + stage]
    n = len(tri)
    flags, pass_, uv2_at, table_at, light = 0, s['pass_'], 0, 0, -1
    st = tri[:, 3:5].reshape(-1, 3, 2)
    # A texture that moves (tcMod) is moved by the GPU, a matrix a draw: every triangle then
    # has the same whole repeats taken off, which the matrix is told. (A draw whose coordinates
    # span too many repeats for that stands still.)
    # (a texture with no script on a surface with no light map has the surface's own colours)
    by_vertex = s['rgb'] == RGB_VERTEX or bool(s['flags'] & S_IMPLICIT)
    whole, tagged = None, page < 0 and page != -2 and by_vertex
    if s['tcmods'] and page != -2:
        low = np.floor(st.reshape(-1, 2).min(axis=0))
        if (st.reshape(-1, 2).max(axis=0) - low).max() < (3.9 if tagged else 31):
            whole = low.astype(np.float32)
            flags |= R_MOVING
    fourth = compact_coords(st, whole)
    if page == -2:
        flags, pass_ = R_SKY, OPAQUE
    elif page >= 0:
        flags, pass_, light = flags | R_LIGHT, MASKED if pass_ == MASKED else OPAQUE, light_pages[page]
        lm = np.clip(np.round(tri[:, 5:7] * 65535), 0, 65535).astype(np.uint32)
        uv2_at = place((lm[:, 0] | lm[:, 1] << 16).astype('<u4').tobytes())
    elif by_vertex:
        # the level's own colour at each vertex: one of 256 of a table, which is as fine as it need be made
        colour = brighter(tri[:, 7:10].astype(np.uint8)).astype(np.uint32)
        alpha = tri[:, 10].astype(np.uint32) if s['alpha'] == A_VERTEX else np.full(n, 255, np.uint32)
        for drop in range(0, 8):
            c = colour >> drop << drop
            a = alpha >> drop << drop
            word = (255 - a) << 24 | c[:, 0] << 16 | c[:, 1] << 8 | c[:, 2]
            table, tag = np.unique(word, return_inverse=True)
            if len(table) <= 256: break
        flags |= R_TAGGED
        fourth = packed_coords(st, whole) | tag.astype(np.uint32) << 24
        table_at = place(np.pad(table, (0, 256 - len(table))).astype('<u4').tobytes())
    if shaders[sh][1] & F_AUTOSPRITE: flags |= R_SPRITES
    v = np.zeros(n, np.dtype([('xyz', '<f4', 3), ('uv', '<u4')]))
    v['xyz'], v['uv'] = tri[:, :3], fourth
    runs_out.append((sh, shaders[sh][2] + stage, pass_, flags, len(world_vertices) // 16, n, s['textures'][0] if s['textures'] else -1, light,
                     uv2_at, table_at, whole if whole is not None else np.zeros(2, np.float32), m))
    world_vertices.extend(v.tobytes())
    world_models.append(m)
world_at = place(bytes(world_vertices))

# the light at points of a grid through the level, for what moves in it: ambient, directed, from where
grid_size = np.array([64, 64, 128], np.float32)
entities = lump(L_ENTITIES).decode('latin1')
m = re.search(r'"gridsize"\s+"([^"]+)"', entities)
if m: grid_size = np.array([float(x) for x in m.group(1).split()], np.float32)
world_low, world_high = np.array(bsp_models[0][0:3], np.float32), np.array(bsp_models[0][3:6], np.float32)
grid_origin = grid_size * np.ceil(world_low / grid_size)
grid_bounds = ((grid_size * np.floor(world_high / grid_size) - grid_origin) / grid_size + 1).astype(np.int32)
grid = np.frombuffer(lump(L_LIGHTGRID), np.uint8).reshape(-1, 8).copy()
grid[:, 0:3], grid[:, 3:6] = brighter(grid[:, 0:3]), brighter(grid[:, 3:6])
grid_at = place(grid.tobytes())

# ---- sounds ----

kind_now[0] = 'sounds'
sounds = []
for n in wanted.get('sound', []):
    if not has(n): continue
    data = read(n)
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE': continue
    at, fmt, pcm = 12, None, None
    while at + 8 <= len(data):
        tag, size = data[at:at + 4], struct.unpack_from('<I', data, at + 4)[0]
        if tag == b'fmt ': fmt = struct.unpack_from('<HHIIHH', data, at + 8)
        elif tag == b'data': pcm = data[at + 8:at + 8 + size]
        at += 8 + (size + 1 & ~1)
    if not fmt or pcm is None or fmt[0] != 1 or fmt[1] != 1: continue
    width, rate = fmt[5] // 8, fmt[2]
    while rate >= 2 * SOUND_RATE:
        # every two samples their mean: half the bytes, and nothing above the new rate's reach left to fold back
        a = np.frombuffer(pcm[:len(pcm) // (2 * width) * 2 * width], '<i2' if width == 2 else np.uint8).astype(np.int32).reshape(-1, 2)
        pcm, rate = ((a[:, 0] + a[:, 1]) >> 1).astype('<i2' if width == 2 else np.uint8).tobytes(), rate // 2
    sounds.append((clean(n) + '.wav', place(pcm, 4), len(pcm) // width, rate, width))

# ---- the file of what is drawn and played ----

def name64(s): return s.encode('latin1')[:63].ljust(64, b'\0')

def tables(base):
    """The sections, with what they point at `base` bytes into the file."""
    out = {}
    out['TEXS'] = b''.join(struct.pack('<IHHBBHI', at + (base if sheet == 0xffff else 0), w, h, fmt, flags, sheet, key)
                           for at, w, h, fmt, flags, sheet, key in textures)
    out['SHET'] = b''.join(struct.pack('<IIHHBBHI', base + at, n, w, h, channels, packing, 0, unused)
                           for at, n, w, h, channels, packing, unused in sheets)
    order = sorted(range(len(shaders)), key=lambda i: shaders[i][0])
    out['SHAD'] = b''.join(struct.pack('<64sIHBB', name64(shaders[i][0]), shaders[i][1], shaders[i][2], shaders[i][3], 0) + struct.pack('<i', i) for i in order)
    stage_bytes = []
    for s in stages_out:
        b = struct.pack('<HBBfBBBB', s['textures'][0] if s['textures'] else 0xffff, max(len(s['textures']), 1), s['pass_'], s['fps'],
                        s['flags'], s['rgb'], s['alpha'], min(len(s['tcmods']), 3))
        b += struct.pack('<Iffff', *s['rgbwave']) + struct.pack('<Iffff', *s['alphawave']) + bytes(s['const'])
        for t in (s['tcmods'] + [(0, 0, 0, 0, 0, 0)] * 3)[:3]: b += struct.pack('<BBHffff', t[0], t[1], 0, *t[2:])
        stage_bytes.append(b)
    out['STAG'] = b''.join(stage_bytes)
    out['SHIX'] = b''   # (shaders are found by name in SHAD, whose last word is their number: the tables use numbers)
    morder = sorted(range(len(models)), key=lambda i: models[i][0])
    out['MODL'] = b''.join(struct.pack('<64sHHHHII', name64(m[0]), m[1], m[2], m[3], m[4], (base + m[5]) if m[3] else 0, base + m[6]) +
                           np.concatenate([m[7], m[8], m[9], m[10]]).astype('<f4').tobytes() for m in (models[i] for i in morder))
    out['SURF'] = b''.join(struct.pack('<64siIII', name64(s[0]), s[1], s[2], base + s[3], base + s[4]) for s in surfaces_out)
    sorder = sorted(range(len(skins)), key=lambda i: skins[i][0])
    out['SKIN'] = b''.join(struct.pack('<64sHHI', name64(skins[i][0]), skins[i][1], skins[i][2], 0) for i in sorder)
    out['SKNE'] = b''.join(struct.pack('<64si', name64(n), sh) for n, sh in skin_entries)
    out['WRUN'] = b''.join(struct.pack('<iHBBIIiiII2fHH', sh, stage, pass_, flags, first, n, tex, light, base + uv2 if flags & R_LIGHT else 0,
                                       base + table if flags & R_TAGGED else 0, float(whole[0]), float(whole[1]), m, 0)
                           for sh, stage, pass_, flags, first, n, tex, light, uv2, table, whole, m in runs_out)
    out['WHDR'] = struct.pack('<IIII', len(runs_out), len(bsp_models), base + world_at, len(world_vertices) // 16) + grid_origin.astype('<f4').tobytes() + \
        grid_size.astype('<f4').tobytes() + grid_bounds.astype('<i4').tobytes() + struct.pack('<II', base + grid_at, len(grid))
    bm = []
    for i, m in enumerate(bsp_models):
        mine = [k for k, r in enumerate(runs_out) if r[11] == i]
        bm.append(struct.pack('<II6f', mine[0] if mine else 0, len(mine), *m[0:6]))
    out['WBMD'] = b''.join(bm)
    sndorder = sorted(range(len(sounds)), key=lambda i: sounds[i][0])
    out['SNDS'] = b''.join(struct.pack('<64sIIIHH', name64(s[0]), base + s[1], s[2], s[3], s[4], 1) for s in (sounds[i] for i in sndorder))
    out['NRML'] = normal_table.astype('<f4').tobytes()
    return out

make_sheets()
for row, of in texture_copies:
    textures[row] = list(textures[of])
HEADER = 16 + 32 * 12
sizes = tables(0)
base = HEADER + sum(len(v) + (-len(v) % 16) for v in sizes.values())
base += -base % 4096       # the blob begins on a page: what the GPU reads never shares one with a table
out = tables(base)
head = struct.pack('<4sIII', b'SEQ3', 2, len(out), base)
body, at = b'', HEADER
for tag, data in out.items():
    head += struct.pack('<4sII', tag.encode(), at, len(data))
    data += b'\0' * (-len(data) % 16)
    body += data
    at += len(data)
head = head.ljust(HEADER, b'\0')
with open(OUT_DAT, 'wb') as f:
    f.write(head + body)
    f.write(b'\0' * (base - HEADER - len(body)))
    f.write(blob)

# ---- the pak of what the game reads itself ----

def trimmed_bsp():
    """The level without what only a renderer reads (its light maps, its light grid, its
    indexes): the game's collision reads the rest."""
    drop = (L_LIGHTMAPS, L_LIGHTGRID, L_INDEXES)
    out, at, table = bytearray(bsp[:8 + 8 * 17]), 8 + 8 * 17, []
    for i in range(17):
        data = b'' if i in drop else lump(i)
        table.append((at, len(data)))
        out += data + b'\0' * (-len(data) % 4)
        at = len(out)
    for i, (o, n) in enumerate(table): struct.pack_into('<ii', out, 8 + 8 * i, o, n)
    return bytes(out)

BOT_NAMES = {'sarge': 'Sarge', 'visor': 'Visor', 'major': 'Major', 'grunt': 'Grunt', 'daemia': 'Daemia', 'stripe': 'Stripe'}
def bots_text():
    text = read('scripts/bots.txt').decode('latin1')
    keep = [b for b in re.findall(r'\{[^}]*\}', text) if any(re.search(r'(?i)name\s+%s\b' % BOT_NAMES.get(x, x), b) for x in BOTS)]
    return '\n\n'.join(keep) + '\n'

def arenas_text():
    """The one level there is, of the demo's four."""
    text = read('scripts/arenas.txt').decode('latin1')
    keep = [b for b in re.findall(r'\{[^}]*\}', text) if re.search(r'(?i)map\s+"?%s\b' % MAP, b)]
    return '\n\n'.join(keep) + '\n'

stored = {}
for n in wanted.get('file', []):
    key = n.lower().replace('\\', '/')
    if not has(key) or key.endswith(('.qvm', '.md3', '.roq')): continue
    if key.endswith('.bsp'): stored[key] = trimmed_bsp()
    elif key.endswith(('.tga', '.jpg', '.wav')): stored[key] = b'\0'      # asked for only to learn whether it is there
    elif key == 'scripts/bots.txt': stored[key] = bots_text().encode('latin1')
    elif key == 'scripts/arenas.txt': stored[key] = arenas_text().encode('latin1')
    else: stored[key] = read(key)
with zipfile.ZipFile(OUT_PAK, 'w', zipfile.ZIP_STORED) as z:
    for n in sorted(stored):
        info = zipfile.ZipInfo(n, (2024, 1, 1, 0, 0, 0))
        z.writestr(info, stored[n])

if LIST:
    by_name = {}
    for key, i in texture_of.items():
        at, w, h, fmt, flags, sheet, k = textures[i]
        by_name[key[0]] = by_name.get(key[0], 0) + w * h * (4 if fmt == WORDS else 3)
    for n, b in sorted(by_name.items(), key=lambda kv: -kv[1])[:40]: print('  %7d  %s' % (b, n))
    by_folder = {}
    for n, b in by_name.items(): by_folder[n.split('/')[0]] = by_folder.get(n.split('/')[0], 0) + b
    print('  pictures by folder:', ', '.join('%s %d KB' % (k, v >> 10) for k, v in sorted(by_folder.items(), key=lambda kv: -kv[1])))
    print('  sounds:', ', '.join('%s %d' % (s[0], s[2] * s[4] >> 10) for s in sorted(sounds, key=lambda s: -s[2] * s[4])[:25]))
print('%s: %d textures, %d shaders of %d stages, %d models of %d surfaces, %d skins, %d runs of %d vertices in the level, %d sounds' % (
    os.path.basename(OUT_DAT), len(textures), len(shaders), len(stages_out), len(models), len(surfaces_out), len(skins), len(runs_out), len(world_vertices) // 16, len(sounds)))
print('%s: %d bytes (tables %d; %s); %s: %d bytes in %d files' % (os.path.basename(OUT_DAT), base + len(blob), base,
      ', '.join('%s %d KB' % (k, v >> 10) for k, v in sizes_by_kind.items()), os.path.basename(OUT_PAK), os.path.getsize(OUT_PAK), len(stored)))
j, g = packed_report['jpeg'], packed_report['png']
if j[0] or g[0]:
    print('%s: %d pictures in %d JPEG sheets (quality %d) of %d KB, %d KB as texels, a texel %.2f levels off on the mean and %d at most; %d PNG sheets of %d KB, %d KB as texels' % (
        os.path.basename(OUT_DAT), len(sheet_wanted), j[0], QUALITY, j[2] >> 10, j[1] >> 10, j[3] / max(j[1], 1), j[4], g[0], g[2] >> 10, g[1] >> 10))
print('%s: %s' % (os.path.basename(OUT_PAK), ', '.join('%s %d KB' % (n, len(v) >> 10) for n, v in sorted(stored.items(), key=lambda kv: -len(kv[1]))[:6])))
