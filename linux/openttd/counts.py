"""The counts of openttd.profile.gz (docs/games/openttd.md, "The compiler"). Each run's console is too slow for the counts
as text, so a run's stderr is packed in the guest and carried over the console in base64.

    OPENTTD_PROFILE=count bash linux/nommu/build.sh openttd/build.sh     (with OPENTTD_OUT a folder of its own)
    python linux/openttd/elf2bflt ... ; an image with that openttd.bin:  python linux/nommu/image.py ottcnt
    python linux/openttd/counts.py build/images/ottcnt DIR               (DIR/K.gc: three minutes)
    python3 linux/tdawn/profile.py make OUT DIR/*.gc && gzip -9 -c OUT > linux/openttd/openttd.profile.gz

The drawing on two workers alone: with more, stores to one counter in one pass lose all but one."""
import base64, gzip, os, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
image, out = sys.argv[1:3]
B = '/usr/share/openttd/busy.sav'
CMDS = {
    'play': f'OPENTTD_PLAY=2 OPENTTD_WORKERS=2 openttd -g {B}',
    'ticks': f'OPENTTD_WARM=100 openttd -v null:ticks=300 -s null -m null -g {B}',
    'world': 'cp /usr/share/openttd/test.cfg /tmp/t.cfg; openttd.bin -v null:ticks=200 -s null -m null -g -G 1 -c /tmp/t.cfg',
    'paused': f'OPENTTD_PLAY=1 OPENTTD_PLAY_SHORT=1 OPENTTD_WORKERS=7 OPENTTD_PAUSE=1 openttd -g {B}',
    'follow': f'OPENTTD_PLAY=1 OPENTTD_PLAY_PART=follow OPENTTD_WORKERS=2 openttd -g {B}',
    'build': f'OPENTTD_PLAY=2 OPENTTD_PLAY_PART=build OPENTTD_WORKERS=2 openttd -g {B}',
}


def run(k):
    log = os.path.join(out, k + '.log')
    if os.path.exists(log):
        os.remove(log)
    cmd = '(' + CMDS[k] + ') 2>/tmp/gc >/tmp/gco; gzip -1 /tmp/gc; echo B6-GO; base64 /tmp/gc.gz; echo B6-END'
    env = dict(os.environ, RVC_THREADS='4')
    subprocess.run([os.path.join(ROOT, 'bin', 'rvc_cpu.exe'), '--quiet', '--ips', '2000000', '--uart-log', log, '--wall', '2400',
                    '--image', image, '--expect', '/ # ', '--send', "nano-X -p & sleep 2; " + cmd + "; echo LX-''DONE\\n",
                    '--until', 'LX-DONE'], cwd=ROOT, env=env, capture_output=True)
    m = re.search(r'B6-GO\r?\n(.*?)B6-END', open(log, encoding='latin-1').read(), re.S)
    data = gzip.decompress(base64.b64decode(re.sub(r'\s', '', m[1])))
    open(os.path.join(out, k + '.gc'), 'wb').write(data)
    return k, len(data), data.count(b'GC end')


os.makedirs(out, exist_ok=True)
with ThreadPoolExecutor(len(CMDS)) as ex:
    for r in ex.map(run, CMDS):
        print(r, flush=True)
