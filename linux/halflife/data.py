"""Uplink's data into an image's overlay, where the engine is (linux/nommu/image.py calls it): the
installer fetched and unpacked (data.sh), pak0 made smaller (repack.py), the files the game reads,
and what the launcher had (docs/games/halflife.md, "The start"): its splash, the films, the menu's
sounds, the readme and the order page. Valve's data goes only into build/: never into
linux/prebuilt or the repository.
   python linux/halflife/data.py OVERLAY"""
import os, re, shutil, struct, subprocess, sys, tempfile
from PIL import Image
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
B = os.path.join(ROOT, 'build', 'halflife')
# (not liblist.gam: newer than gameinfo.txt, the engine would take the game's type and first map from it)
FILES = ('decals.wad', 'gfx.wad', 'cached.wad', 'default.cfg', 'skill.cfg', 'settings.scr', 'Titles.txt')


def wsl(path):
    return '/mnt/' + path[0].lower() + path[2:].replace('\\', '/')


def one_sound_chunk(avi):
    """The AVI with its sound in one chunk before the frames (the card plays it where it lies in the
    ROM: avi_shaderemu.c), its index made again."""
    def chunks(d, p, end):
        while p + 8 <= end:
            cid, size = d[p:p + 4], struct.unpack('<I', d[p + 4:p + 8])[0]
            yield cid, p, size
            p += 8 + size + (size & 1)
    head = movi = None
    for cid, p, size in chunks(avi, 12, len(avi)):
        if cid == b'LIST' and avi[p + 8:p + 12] == b'hdrl':
            head = avi[p:p + 8 + size + (size & 1)]
        elif cid == b'LIST' and avi[p + 8:p + 12] == b'movi':
            movi = (p + 12, p + 8 + size)
    if not head or not movi:
        return avi
    flags = {}
    for cid, p, size in chunks(avi, 12, len(avi)):
        if cid == b'idx1':
            for i in range(p + 8, p + 8 + size, 16):
                ck, fl, off, _ = struct.unpack('<4sIII', avi[i:i + 16])
                flags[off] = fl
    sound, frames = bytearray(), []
    for cid, p, size in chunks(avi, movi[0], movi[1]):
        if cid[2:] == b'wb':
            sound += avi[p + 8:p + 8 + size]
        elif cid[2:] in (b'dc', b'db', b'pc'):
            frames.append((cid, avi[p + 8:p + 8 + size], flags.get(p - movi[0] + 4, flags.get(p, 0x10))))
    if not sound:
        return avi
    body, index = bytearray(b'movi'), bytearray()
    for cid, data, fl in [(b'01wb', bytes(sound), 0x10)] + frames:
        index += struct.pack('<4sIII', cid, fl, len(body), len(data))
        body += cid + struct.pack('<I', len(data)) + data + b'\0' * (len(data) & 1)
    out = head + b'LIST' + struct.pack('<I', len(body)) + body + b'idx1' + struct.pack('<I', len(index)) + index
    return b'RIFF' + struct.pack('<I', 4 + len(out)) + b'AVI ' + out


G = os.path.join(sys.argv[1], 'usr', 'share', 'halflife', 'valve')
subprocess.run(['wsl', '--', 'bash', wsl(os.path.join(ROOT, 'linux', 'halflife', 'data.sh'))], check=True)
U = os.path.join(B, 'uplink')
V = os.path.join(U, 'valve')
pak = os.path.join(V, 'pak0.PAK')
with tempfile.TemporaryDirectory() as extra:
    # the menu's sounds (mainui's media/launch_*.wav) in pak0, where the card plays sounds from
    os.makedirs(os.path.join(extra, 'sound', 'media'))
    for n in os.listdir(os.path.join(U, 'media')):
        if n.lower().endswith('.wav'):
            shutil.copy(os.path.join(U, 'media', n), os.path.join(extra, 'sound', 'media', n.lower()))
    subprocess.run([sys.executable, os.path.join(ROOT, 'linux', 'halflife', 'repack.py'), pak, G,
                    os.path.join(B, 'repack-cache'), wsl(os.path.join(B, 'adpcm_encode')), extra], check=True)
for n in FILES:
    shutil.copy(os.path.join(V, n), G)
# use on E, as the menu's defaults (kb_def.lst) and later Half-Life have it, not the demo's Tab
cfg = os.path.join(G, 'default.cfg')
text = open(cfg, 'rb').read()
assert re.search(rb'bind\s+TAB\s+\+use', text), 'default.cfg: no TAB +use to move'
open(cfg, 'wb').write(re.sub(rb'bind(\s+)TAB(\s+)\+use', rb'bind\1e\2+use', text))
shutil.copy(os.path.join(U, 'delta.lst'), G)
shutil.copy(os.path.join(U, 'readme.txt'), G)
# Sierra's licence, shown and asked by the starter before the game (cabinst --installed ... -: docs/system/install.md)
os.makedirs(os.path.join(G, '..', 'licence'), exist_ok=True)
shutil.copy(os.path.join(U, 'eula.txt'), os.path.join(G, '..', 'licence', 'eula.txt'))
# the launcher's splash while the engine loads (vid_shaderemu.c), as a picture the window system draws
Image.open(os.path.join(U, 'logo.bmp')).convert('RGB').save(os.path.join(G, '..', 'logo.ppm'))
# the films: the opening one (StartupVids.txt) with its sound in one piece, the menu's logo; the order page
M = os.path.join(G, 'media')
shutil.rmtree(M, ignore_errors=True)
shutil.copytree(os.path.join(U, 'media', 'order'), os.path.join(M, 'order'))
open(os.path.join(M, 'intro.avi'), 'wb').write(one_sound_chunk(open(os.path.join(U, 'media', 'intro.avi'), 'rb').read()))
shutil.copy(os.path.join(U, 'media', 'uplink.avi'), os.path.join(M, 'logo.avi'))
open(os.path.join(M, 'StartupVids.txt'), 'w', newline='\n').write('media/intro.avi\n')
# the menu's pictures pak0 has itself come from pak0
with open(pak, 'rb') as f:
    _, at, size = struct.unpack('<4sii', f.read(12))
    f.seek(at)
    have = {os.path.splitext(f.read(64).split(b'\0')[0].decode().lower())[0] for _ in range(size // 64)}
shell = os.path.join(G, 'extras.pk3dir', 'gfx', 'shell')
for n in os.listdir(shell) if os.path.isdir(shell) else []:
    if 'gfx/shell/' + os.path.splitext(n.lower())[0] in have:
        os.remove(os.path.join(shell, n))
print('Uplink in', os.path.relpath(G, ROOT))
