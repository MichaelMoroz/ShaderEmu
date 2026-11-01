"""OpenTTD's title under rvc_harness --cpu --warp, without --cpu-fresh: its window as the GPU's lists draw it, against the same
window drawn by the 8-bit blitter (OPENTTD_DRAW=cpu), and how much of it is the buttons' yellow and the frame's brown. rvc_cpu draws
no list and OPENTTD_CHECK looks at no title: docs/games/openttd.md, "The picture on the GPU".

    python tools/openttd_title.py [--harness EXE] [--image DIR] [--sleep S] [--keep DIR]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOX = (252, 83, 386, 395)   # the title window, at 640 x 480


def capture(a, env, out):
    cmd = f"export NANOX_SIZE=640x480; nx; sleep 3; {env} openttd >/tmp/g.log 2>&1 </dev/null & sleep {a.sleep}; echo LX-''DONE\n"
    snap = out + '.snap'
    run = ['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1'), a.harness, '--cpu', '--warp', '--image', a.image, '--no-viz',
           '--no-stdin', '--seconds', str(a.sleep * 3 + 60), '--uart-log', out + '.log', '--expect', '/ # ', '--send', cmd, '--until', 'LX-DONE', '--save-state', snap]
    subprocess.run(run, cwd=ROOT, capture_output=True, env=dict(os.environ, RVC_THREADS='8'))
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'snapshot_screen.py'), snap, out + '.png'], check=True, capture_output=True)
    os.remove(snap)
    return np.asarray(Image.open(out + '.png').convert('RGB'), dtype=int)


def shares(p):
    """Of the window: yellow-orange (the buttons), brown (the frame), navy (what a stale list drew)."""
    r, g, b = p[..., 0], p[..., 1], p[..., 2]
    yellow = (r > 200) & (g > 120) & (g < 210) & (b < 90)
    brown = (r > 70) & (r > g) & (g > b) & (r - b > 30) & ~yellow
    navy = (b > r + 40) & (b > g + 40) & (b < 170)
    return yellow.mean(), brown.mean(), navy.mean()


def drive(a, d):
    """openttd-depot's scene with the lists drawn: test_all's rvc_cpu draws none."""
    log = os.path.join(d, 'drive.log')
    if os.path.exists(log):
        os.remove(log)
    cmd = "nano-X -p & sleep 2; OPENTTD_MADE_CHECK=1 OPENTTD_PLAY=1 OPENTTD_PLAY_PART=window OPENTTD_PLAY_SCENE=depot openttd -g /usr/share/openttd/busy.sav; echo LX-''DONE\n"
    subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1'), a.harness, '--cpu', '--warp', '--image', a.image, '--no-viz', '--no-stdin',
                    '--seconds', '500', '--uart-log', log, '--expect', '/ # ', '--send', cmd, '--until', 'LX-DONE'], cwd=ROOT, capture_output=True, env=dict(os.environ, RVC_THREADS='8'))
    bad = True
    lines = [l.strip() for l in open(log, encoding='latin-1') if 'depot drive' in l and not l.startswith('/')]
    stages = {'out of the depot': r'[2-5]\d\d', 'back to a depot': r'\d{2,}', 'on the road': r'[23]\d\d'}
    for l in lines[-3:]:
        print(l)
    if len(lines) >= 3:
        bad = False
        for l, (name, shown) in zip(lines[-3:], stages.items()):
            bad |= not re.search(name + r': \d+ pictures, moved \d{2,}, [0-2] off the vehicle .*, shown ' + shown + ' and not seen ([0-9]|1[0-9])$', l)
    print('drive with lists drawn:', 'FAIL' if bad else 'PASS')
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--harness', default=os.path.join(ROOT, 'bin', 'rvc_harness.exe'))
    ap.add_argument('--image', default=os.path.join(ROOT, 'build', 'images', 'nommu'))
    ap.add_argument('--sleep', type=int, default=60)
    ap.add_argument('--keep')
    ap.add_argument('--no-drive', action='store_true')
    a = ap.parse_args()
    d = a.keep or tempfile.mkdtemp()
    os.makedirs(d, exist_ok=True)
    x0, y0, x1, y1 = BOX
    lists = capture(a, '', os.path.join(d, 'lists'))[y0:y1, x0:x1]
    cpu = capture(a, 'OPENTTD_DRAW=cpu', os.path.join(d, 'cpu'))[y0:y1, x0:x1]
    differ = (lists != cpu).any(axis=2).mean()
    bad = False
    for name, p in (('lists', lists), ('cpu', cpu)):
        y, b, n = shares(p)
        print(f'{name}: yellow {y:.1%}, brown {b:.1%}, navy {n:.1%}')
        bad |= y < 0.4 or b < 0.03 or n > 0.01
    print(f'lists against cpu: {differ:.2%} of the window differs')
    bad |= differ > 0.01
    if not a.no_drive:
        bad |= drive(a, d)
    print('FAIL' if bad else 'PASS')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
