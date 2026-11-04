"""Age of Empires into an image's overlay, every image's (linux/nommu/image.py; AOE_IMAGE=0 for a private test one): the
1997 trial's installer fetched and checked against its sums into a cache outside the tree, put into the image byte for
byte (its EULA allows copies of the whole trial, with the EULA, not for commercial purposes), and the program built
(program.sh). The machine shows the EULA read out of the installer, and unpacks the trial into memory once it is
accepted there (cabinst, docs/system/install.md); aoeprep makes what the port reads from the trial's files
(docs/games/aoe.md). Nothing of it goes into the repository or linux/prebuilt.
   python linux/aoe/data.py OVERLAY
Exit 0: both are in OVERLAY. Otherwise OVERLAY is as it was, and a line says what is missing.
   AOE_CACHE    the cache (~/.cache/shaderemu/aoe)
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
# The instruments of its music (the trial has none: Windows had them): DMXOPL's General MIDI bank, MIT, the copy OpenTTD fetches
BANK_URL = 'https://raw.githubusercontent.com/sneakernets/DMXOPL/e83c144a2e6d3c283e20f9d16d011126931892f5/GENMIDI.op2'
BANK_SHA256 = '92fc03c7734c45b1ece2bfad1f79d1abbd2b27ec6b45fbfb1c5a74f5ce0138c9'
BANK = 'genmidi.op2'


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
        request = urllib.request.Request(url, headers={'User-Agent': 'ShaderEmu image build'})
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


def bank(cache):
    """The music's instruments in the cache, or None: the game then plays with no music."""
    path = os.path.join(cache, BANK)
    if os.path.exists(path) and digest(path, 'sha256') == BANK_SHA256:
        return path
    part = path + '.part'
    try:
        request = urllib.request.Request(BANK_URL, headers={'User-Agent': 'ShaderEmu image build'})
        with urllib.request.urlopen(request, timeout=60) as r, open(part, 'wb') as f:
            shutil.copyfileobj(r, f)
    except OSError as e:
        print('aoe: the music\'s instruments could not be fetched: %s. The game goes in with no music.' % e, file=sys.stderr)
        return None
    if digest(part, 'sha256') != BANK_SHA256:
        os.remove(part)
        print('aoe: what %s gave is not the bank. The game goes in with no music.' % BANK_URL, file=sys.stderr)
        return None
    os.replace(part, path)
    return path


def wsl_path(path):
    path = os.path.abspath(path)
    if os.name != 'nt':
        return path
    return '/mnt/' + path[0].lower() + path[2:].replace('\\', '/')


def program():
    """A folder with the flat program, its starter, aoeprep and the Start menu's line."""
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
    cache = os.environ.get('AOE_CACHE') or os.path.join(os.path.expanduser('~'), '.cache', 'shaderemu', 'aoe')
    os.makedirs(cache, exist_ok=True)
    try:
        exe = fetch(cache)
        built = program()
        instruments = bank(cache)
    except Missing as e:
        print('aoe: %s. The image goes on without the game.' % e, file=sys.stderr)
        return 3
    # program and installer together or not at all, so that nothing below can leave half
    share = os.path.join(overlay, 'usr', 'share', 'aoe')
    os.makedirs(share, exist_ok=True)
    shutil.copyfile(exe, os.path.join(share, TRIAL))
    if instruments:
        shutil.copyfile(instruments, os.path.join(share, BANK))
    os.makedirs(os.path.join(overlay, 'usr', 'bin'), exist_ok=True)
    for name in ('aoe.bin', 'aoe', 'aoeprep'):
        shutil.copyfile(os.path.join(built, 'usr', 'bin', name), os.path.join(overlay, 'usr', 'bin', name))
    shutil.copyfile(os.path.join(built, 'usr', 'share', 'nxapps.76-aoe'), os.path.join(overlay, 'usr', 'share', 'nxapps.76-aoe'))
    print('aoe: in the image: the installer, unpacked in the machine after its EULA is accepted there')
    return 0


if __name__ == '__main__':
    sys.exit(main())
