"""Age of Empires into an image's overlay, only when asked (linux/nommu/image.py NAME --aoe, or AOE_IMAGE=1):
the 1997 trial fetched and checked, unpacked, its data made (make_data.py), in a cache outside the tree, and the
program built (program.sh). Nothing of it goes into the repository or linux/prebuilt. By asking for it the user
accepts the trial's EULA: docs/games/aoe.md.
   python linux/aoe/data.py OVERLAY
Exit 0: both are in OVERLAY. Otherwise OVERLAY is as it was, and a line says what is missing.
   AOE_CACHE    the cache (~/.cache/shaderx86/aoe)
   AOE_URL      where the trial comes from
   AOE_PROGRAM  a folder with usr/bin/aoe.bin already made flat, in place of building it
   AOE_WSL_HOME a private WSL home to build in"""
import hashlib, os, shutil, subprocess, sys, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
URL = 'https://archive.org/download/aoe-trial-1997/MSAoE.exe'
MD5 = 'dadea8f91c7e2b70ff5c2be7aad6c682'
SHA256 = '60e01bd94cc88af64fd2ca40a75f3df783b8ea7725971ddd02c60f4d11e96973'
TRIAL = 'MSAoE.exe'


class Missing(Exception):
    pass


def digest(path, kind):
    h = hashlib.new(kind)
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def right(path):
    return os.path.exists(path) and digest(path, 'md5') == MD5 and digest(path, 'sha256') == SHA256


def fetch(cache):
    exe = os.path.join(cache, TRIAL)
    if right(exe):
        print('aoe: the trial is in the cache')
        return exe
    url = os.environ.get('AOE_URL', URL)
    print('aoe: fetching', url)
    part = exe + '.part'
    try:
        request = urllib.request.Request(url, headers={'User-Agent': 'ShaderX86 image build'})
        with urllib.request.urlopen(request, timeout=60) as r, open(part, 'wb') as f:
            shutil.copyfileobj(r, f, 1 << 20)
    except OSError as e:
        if os.path.exists(part):
            os.remove(part)
        raise Missing('the trial could not be fetched from %s: %s' % (url, e))
    if not right(part):
        got = digest(part, 'md5')
        os.remove(part)
        raise Missing('what %s gave is not the trial (md5 %s, wanted %s)' % (url, got, MD5))
    os.replace(part, exe)
    return exe


def seven_zip():
    for p in (shutil.which('7z'), shutil.which('7za'), r'C:\Program Files\7-Zip\7z.exe', r'C:\Program Files (x86)\7-Zip\7z.exe'):
        if p and os.path.exists(p):
            return p
    raise Missing('7-Zip is wanted to unpack the trial, and there is none')


def fresh(folder):
    if os.path.exists(folder):
        shutil.rmtree(folder)
    os.makedirs(folder)


def data(cache):
    """The made data, in the cache. A folder is whole if its mark says what made it."""
    with open(os.path.join(HERE, 'make_data.py'), 'rb') as f:
        made_by = hashlib.md5(f.read())
    with open(os.path.join(HERE, 'dat34.py'), 'rb') as f:
        made_by.update(f.read())
    mark_text = MD5 + ' ' + made_by.hexdigest()
    out = os.path.join(cache, 'data')
    mark = os.path.join(cache, 'data.mark')
    if os.path.exists(mark) and open(mark).read() == mark_text and os.path.isdir(out):
        print('aoe: the data is in the cache')
        return out
    exe = fetch(cache)
    trial = os.path.join(cache, 'trial')
    fresh(trial)
    p = subprocess.run([seven_zip(), 'x', '-y', '-o' + trial, exe], stdout=subprocess.DEVNULL)
    if p.returncode:
        raise Missing('7-Zip could not unpack %s (exit %d)' % (exe, p.returncode))
    print('aoe: making the data')
    if os.path.exists(mark):
        os.remove(mark)
    fresh(out)
    p = subprocess.run([sys.executable, os.path.join(HERE, 'make_data.py'), trial, out], stdout=subprocess.DEVNULL)
    if p.returncode:
        raise Missing('make_data.py failed (exit %d)' % p.returncode)
    if not os.path.exists(os.path.join(out, 'Empires.dat')):
        raise Missing('make_data.py made no Empires.dat')
    with open(mark, 'w') as f:
        f.write(mark_text)
    return out


def wsl_path(path):
    path = os.path.abspath(path)
    if os.name != 'nt':
        return path
    return '/mnt/' + path[0].lower() + path[2:].replace('\\', '/')


def program():
    """A folder with the flat program, its starter and the Start menu's line."""
    given = os.environ.get('AOE_PROGRAM')
    if given:
        return given
    out = os.path.join(ROOT, 'build', 'aoe', 'out')
    home = os.environ.get('AOE_WSL_HOME', '')
    cmd = ['bash', wsl_path(os.path.join(HERE, 'program.sh')), wsl_path(out), home]
    if os.name == 'nt':
        cmd = ['wsl', '--'] + cmd
    print('aoe: building the program')
    p = subprocess.run(cmd)
    if p.returncode or not os.path.exists(os.path.join(out, 'usr', 'bin', 'aoe.bin')):
        raise Missing('the program could not be built (exit %d): linux/aoe/program.sh' % p.returncode)
    return out


def main():
    overlay = sys.argv[1]
    cache = os.environ.get('AOE_CACHE') or os.path.join(os.path.expanduser('~'), '.cache', 'shaderx86', 'aoe')
    os.makedirs(cache, exist_ok=True)
    try:
        made = data(cache)
        built = program()
    except Missing as e:
        print('aoe: %s. The image goes on without the game.' % e, file=sys.stderr)
        return 3
    # program and data together or not at all, so that nothing below can leave half
    shutil.copytree(made, os.path.join(overlay, 'usr', 'share', 'aoe'), dirs_exist_ok=True)
    os.makedirs(os.path.join(overlay, 'usr', 'bin'), exist_ok=True)
    for name in ('aoe.bin', 'aoe'):
        shutil.copyfile(os.path.join(built, 'usr', 'bin', name), os.path.join(overlay, 'usr', 'bin', name))
    shutil.copyfile(os.path.join(built, 'usr', 'share', 'nxapps.76-aoe'), os.path.join(overlay, 'usr', 'share', 'nxapps.76-aoe'))
    print('aoe: in the image')
    return 0


if __name__ == '__main__':
    sys.exit(main())
