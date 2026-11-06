"""The counts of imgui.profile (docs/apps/imgui.md, "Played counts"): imdemo's frames in each of its scenes, counted.

    IMGUI_PROFILE=count bash linux/nommu/build.sh imgui/build.sh     (~/nm/work/src/imgui-count-build/root/usr/bin/imdemo)
    that imdemo made flat over a copy of build/nommu/flatroot, and an image of it:  python linux/nommu/image.py imcnt ROOT
    python linux/imgui/counts.py build/images/imcnt DIR                          (DIR/K.gc, a few minutes)
    python3 linux/tdawn/profile.py make linux/imgui/imgui.profile DIR/*.gc

A run's counters (stderr) are packed in the guest and carried over the console in base64."""
import base64, gzip, os, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
image, out = sys.argv[1:3]
F = 'IMDEMO_FRAMES=300 '
CMDS = {
    # the program's own window, held and with its clock; the library's demo window, at its top and scrolled
    'held': F + 'IMDEMO_HOLD=1 imdemo',
    'live': F + 'imdemo',
    'demo': F + 'IMDEMO_HOLD=1 IMDEMO_SHOW=1 imdemo',
    'scrolled': F + 'IMDEMO_HOLD=1 IMDEMO_SHOW=1 IMDEMO_SCROLL=300 imdemo',
}


def run(k):
    log = os.path.join(out, k + '.log')
    if os.path.exists(log):
        os.remove(log)
    cmd = '(' + CMDS[k] + ') 2>/tmp/gc >/tmp/gco; gzip -1 /tmp/gc; echo B6-GO; base64 /tmp/gc.gz; echo B6-END'
    env = dict(os.environ, RVC_THREADS='2')
    while subprocess.run([os.path.join(ROOT, 'bin', 'rvc_cpu.exe'), '--quiet', '--uart-log', log, '--wall', '2400',
                          '--image', image, '--expect', '/ # ', '--send', 'nano-X -p & sleep 2; ' + cmd + "; echo LX-''DONE\\n",
                          '--until', 'LX-DONE'], cwd=ROOT, env=env, capture_output=True).returncode == 75:
        time.sleep(20)   # (a GPU run holds the lock)
    m = re.search(r'B6-GO\r?\n(.*?)B6-END', open(log, encoding='latin-1').read(), re.S)
    data = gzip.decompress(base64.b64decode(re.sub(r'\s', '', m[1])))
    open(os.path.join(out, k + '.gc'), 'wb').write(data)
    return k, len(data), data.count(b'GC end')


os.makedirs(out, exist_ok=True)
with ThreadPoolExecutor(len(CMDS)) as ex:
    for r in ex.map(run, CMDS):
        print(r, flush=True)
