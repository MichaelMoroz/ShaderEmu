"""Star Wars Jedi Knight II: Jedi Outcast's 2002 demo, in every image (docs/games/jk2.md, "The demo's data"): the
official installer fetched and checked against its sums into a cache outside the tree. Nothing of it goes into the
repository or linux/prebuilt. An image gets the installer itself, byte for byte (the licence, license.txt, LucasArts
2002: copies only in its original form and for no money; no changed or added files, no new levels or derived works):
the machine shows its licence, and unpacks its pak into memory once it is accepted (cabinst, docs/system/install.md).
For the build computer's own program the pak is taken out of it here (it is a Microsoft cabinet with a program in
front; nothing of it is run), only with --accept-eula, which accepts the licence.
   python linux/jk2/data.py fetch --accept-eula          the cache: jk2demo.exe, then base/assets0.pk3 and license.txt
   python linux/jk2/data.py host DIR --accept-eula       DIR/base/assets0.pk3 (a link where it can be), for the build
                                                        computer's program (JK2_HOST=1 linux/jk2/build.sh)
   python linux/jk2/data.py OVERLAY                      a private image's overlay: the installer in usr/share/jk2, and
                                                        the program (usr/bin/jk2.bin, usr/bin/jk2) from $JK2_PROGRAM or
                                                        program.sh
Exit 0: done. 3: something is missing, and a line says what; OVERLAY is then as it was.
   JK2_CACHE    the cache (~/.cache/shaderemu/jk2/demo)
   JK2_URL      where the installer comes from (the Internet Archive's copy of LucasArts' download)
   JK2_PROGRAM  a folder with usr/bin/jk2.bin already made flat, in place of building it
   JK2_WSL_HOME a private WSL home to build in"""
import hashlib, os, shutil, struct, sys, urllib.request, zlib, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
URL = 'https://archive.org/download/jk2demo/jk2demo.exe'
EXE = 'jk2demo.exe'
EXE_SIZE = 69680101
EXE_MD5 = '7d9b901df28f2f4b11f1b00d5d2a0ca5'          # (the Internet Archive's own listing: md5 and sha1 agree)
EXE_SHA256 = 'd3f8e1807ad6bdb81e6aef678a648017e27d86f341ff778527405ce8d4b80bfb'
# what is taken out of the cabinet, by its name there, with its sums
WANTED = {
    '\\Disk1\\GameData\\demo\\assets0.pk3': ('assets0.pk3', 64001989,
                                            '2550446678c25c293760dbf374c64b7d62e04290f3e9c87cad9994b8c0b7f1cd'),
    '\\Disk1\\Install\\license.txt': ('license.txt', 3529,
                                     'd8cf5cadfe85351aec41268c70ff787d7e16f3a6b886bb175b3113ee275ae459'),
}
DEMO_PAK_CHECKSUM = 1431467275    # what the 1.03 and 1.04 engines require of the demo's pak (survey.py computes it)


class Missing(Exception):
    pass


def digest(path, kind):
    h = hashlib.new(kind)
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def right_exe(path):
    return (os.path.exists(path) and os.path.getsize(path) == EXE_SIZE and digest(path, 'md5') == EXE_MD5
            and digest(path, 'sha256') == EXE_SHA256)


def fetch(cache):
    exe = os.path.join(cache, EXE)
    if right_exe(exe):
        return exe
    url = os.environ.get('JK2_URL', URL)
    print('jk2: fetching', url)
    part = exe + '.part'
    try:
        request = urllib.request.Request(url, headers={'User-Agent': 'ShaderEmu image build'})
        with urllib.request.urlopen(request, timeout=60) as r, open(part, 'wb') as f:
            shutil.copyfileobj(r, f, 1 << 20)
    except OSError as e:
        if os.path.exists(part):
            os.remove(part)
        raise Missing('the demo could not be fetched from %s: %s' % (url, e))
    if not right_exe(part):
        got = digest(part, 'md5')
        os.remove(part)
        raise Missing('what %s gave is not the demo (md5 %s, wanted %s)' % (url, got, EXE_MD5))
    os.replace(part, exe)
    return exe


def cabinet(path):
    """The cabinet inside the installer: (its offset in the file, the files [(name, size, folder, offset)], the
    folders [(first data block, blocks, compression)], the reserved bytes of a data block)."""
    with open(path, 'rb') as f:
        data = f.read(1 << 20)
    at = 0
    while True:
        at = data.find(b'MSCF\0\0\0\0', at)
        if at < 0:
            raise Missing('%s has no cabinet in its first megabyte' % path)
        size, = struct.unpack_from('<I', data, at + 8)
        if size + at == EXE_SIZE or size + at <= os.path.getsize(path):
            break
        at += 4
    (sig, _, size, _, files_at, _, minor, major, nfolders, nfiles, flags, _, _) = struct.unpack_from('<4sIIIIIBBHHHHH', data, at)
    p = at + 36
    folder_extra = data_extra = 0
    if flags & 4:
        header_extra, folder_extra, data_extra = struct.unpack_from('<HBB', data, p)
        p += 4 + header_extra
    for bit in (1, 2):              # the previous and next cabinets' names, if any
        if flags & bit:
            for _ in range(2):
                p = data.index(b'\0', p) + 1
    folders = []
    for _ in range(nfolders):
        first, blocks, kind = struct.unpack_from('<IHH', data, p)
        folders.append((at + first, blocks, kind))
        p += 8 + folder_extra
    files = []
    p = at + files_at
    for _ in range(nfiles):
        fsize, offset, folder, _, _, _ = struct.unpack_from('<IIHHHH', data, p)
        end = data.index(b'\0', p + 16)
        files.append((data[p + 16:end].decode('latin-1'), fsize, folder, offset))
        p = end + 1
    return at, files, folders, data_extra


def unpack(exe, out):
    """The WANTED files of the cabinet into out, by their short names. MSZIP: each data block is "CK" and a deflate
    stream whose dictionary is the block before it, uncompressed."""
    at, files, folders, data_extra = cabinet(exe)
    jobs = {}
    for name, size, folder, offset in files:
        if name in WANTED:
            jobs.setdefault(folder, []).append((offset, size, WANTED[name][0]))
    if len(sum(jobs.values(), [])) != len(WANTED):
        raise Missing('the cabinet in %s does not have the demo\'s files' % exe)
    os.makedirs(out, exist_ok=True)
    with open(exe, 'rb') as f:
        for folder, wanted in jobs.items():
            first, blocks, kind = folders[folder]
            if kind & 0xf != 1:
                raise Missing('the demo\'s files are not MSZIP-compressed (%d)' % kind)
            outs = [(o, s, open(os.path.join(out, n + '.part'), 'wb')) for o, s, n in wanted]
            last = b''
            pos = 0
            f.seek(first)
            for _ in range(blocks):
                _, packed, plain = struct.unpack('<IHH', f.read(8))
                f.read(data_extra)
                block = f.read(packed)
                if block[:2] != b'CK':
                    raise Missing('a block of the cabinet is not MSZIP')
                d = zlib.decompressobj(-15, zdict=last) if last else zlib.decompressobj(-15)
                text = d.decompress(block[2:])
                if len(text) != plain:
                    raise Missing('a block of the cabinet unpacked to %d bytes, not %d' % (len(text), plain))
                for o, s, w in outs:
                    lo, hi = max(o, pos), min(o + s, pos + plain)
                    if lo < hi:
                        w.write(text[lo - pos:hi - pos])
                pos += plain
                last = text[-32768:]
                if all(pos >= o + s for o, s, _ in outs):
                    break
            for _, _, w in outs:
                w.close()
    for name, (short, size, sha) in WANTED.items():
        part = os.path.join(out, short + '.part')
        if os.path.getsize(part) != size or digest(part, 'sha256') != sha:
            os.remove(part)
            raise Missing('%s came out of the cabinet not as it should (%d bytes)' % (short, os.path.getsize(part)))
        os.replace(part, os.path.join(out, short))


def demo(cache):
    """base/assets0.pk3 and license.txt in the cache, checked."""
    base = os.path.join(cache, 'base')
    pak = os.path.join(base, 'assets0.pk3')
    licence = os.path.join(cache, 'license.txt')
    whole = all(os.path.exists(p) for p in (pak, licence)) and \
        digest(pak, 'sha256') == WANTED['\\Disk1\\GameData\\demo\\assets0.pk3'][2]
    if not whole:
        exe = fetch(cache)
        print('jk2: taking the pak and the licence out of', exe)
        unpack(exe, base)
        os.replace(os.path.join(base, 'license.txt'), licence)
    sys.path.insert(0, HERE)
    import survey
    with zipfile.ZipFile(pak) as z:
        if survey.pak_checksum(z) != DEMO_PAK_CHECKSUM:
            raise Missing('%s is not the pak the demo\'s engine accepts' % pak)
    return pak, licence


def wsl_path(path):
    path = os.path.abspath(path)
    if os.name != 'nt':
        return path
    return '/mnt/' + path[0].lower() + path[2:].replace('\\', '/')


def program():
    """A folder with the flat program and its starter: $JK2_PROGRAM, or program.sh's (not yet run: docs/games/jk2.md)."""
    given = os.environ.get('JK2_PROGRAM')
    if given:
        if not os.path.exists(os.path.join(given, 'usr', 'bin', 'jk2.bin')):
            raise Missing('%s has no usr/bin/jk2.bin' % given)
        return given
    out = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'build', 'jk2', 'out')
    cmd = ['bash', wsl_path(os.path.join(HERE, 'program.sh')), wsl_path(out), os.environ.get('JK2_WSL_HOME', '')]
    if os.name == 'nt':
        cmd = ['wsl', '--'] + cmd
    print('jk2: building the program')
    import subprocess
    p = subprocess.run(cmd)
    if p.returncode or not os.path.exists(os.path.join(out, 'usr', 'bin', 'jk2.bin')):
        raise Missing('the program could not be built (exit %d): linux/jk2/program.sh' % p.returncode)
    return out


def put(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst):
        os.remove(dst)
    try:
        os.symlink(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        print(__doc__)
        return 2
    if args[0] in ('fetch', 'host') and '--accept-eula' not in sys.argv:
        print(__doc__)
        print('jk2: nothing done: the pak is taken out of the installer on this computer only with --accept-eula, which '
              'accepts its licence.')
        return 2
    cache = os.environ.get('JK2_CACHE') or os.path.join(os.path.expanduser('~'), '.cache', 'shaderemu', 'jk2', 'demo')
    os.makedirs(cache, exist_ok=True)
    try:
        if args[0] in ('fetch', 'host'):
            pak, licence = demo(cache)
            print('jk2: the demo\'s licence, accepted by --accept-eula:', licence)
            if args[0] == 'fetch':
                print('jk2: in the cache:', pak)
                return 0
            put(pak, os.path.join(args[1], 'base', 'assets0.pk3'))
            print('jk2: for the build computer\'s program: +set fs_basepath', os.path.abspath(args[1]))
            return 0
        exe = fetch(cache)
        overlay = args[0]
        built = program()
    except Missing as e:
        print('jk2: %s. Nothing was put in.' % e, file=sys.stderr)
        return 3
    # the installer decoded (4 November 2026: before, the installer itself, unpacked in the machine at every start): the
    # pak and the licence, checked (demo), where the machine links them after the licence is accepted there (cabinst
    # --installed, docs/system/install.md). The program and they together
    pak, licence = demo(cache)
    installed = os.path.join(overlay, 'usr', 'share', 'jk2', 'installed')
    if os.path.isdir(installed):
        shutil.rmtree(installed)
    os.makedirs(os.path.join(installed, 'base'))
    os.makedirs(os.path.join(installed, 'Disk1', 'Install'))
    os.makedirs(os.path.join(overlay, 'usr', 'bin'), exist_ok=True)
    shutil.copyfile(pak, os.path.join(installed, 'base', 'assets0.pk3'))
    shutil.copyfile(licence, os.path.join(installed, 'Disk1', 'Install', 'license.txt'))
    for name in ('jk2.bin', 'jk2'):
        shutil.copyfile(os.path.join(built, 'usr', 'bin', name), os.path.join(overlay, 'usr', 'bin', name))
    menu = os.path.join(built, 'usr', 'share', 'nxapps.77-jk2')
    if os.path.exists(menu):
        shutil.copyfile(menu, os.path.join(overlay, 'usr', 'share', 'nxapps.77-jk2'))
    print('jk2: in the image: the demo decoded, linked into place in the machine after its licence is accepted there')
    return 0


if __name__ == '__main__':
    sys.exit(main())
