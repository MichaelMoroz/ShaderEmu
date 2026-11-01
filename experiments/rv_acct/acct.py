#!/usr/bin/env python3
"""Where the wall time of a real load goes on the lean tick, flat on the card.
    python acct.py LOAD [--tag TEXT] [--tick DIR] [--image DIR] [--env A=B ...] [--extra "ARGS"] [--no-time]
Runs through tools/gpu_queue.ps1 from the main checkout's bin and image, reads the per-pass log and writes one line to
results.jsonl. Each pass's wall is the gap between readbacks. See NOTES.md for how the columns are made."""
import argparse, bisect, json, os, re, struct, subprocess, sys, time
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.abspath(os.path.join(HERE, '..', '..'))
MAIN = subprocess.run(['git', '-C', TREE, 'worktree', 'list', '--porcelain'], capture_output=True, text=True).stdout.split('\n')[0].split(' ', 1)[1]
LOGS = os.path.join(HERE, 'logs')
PRE = 'nano-X -p & sleep 2; '
TD = 'TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 '
A = 'echo H2H"" A; '
B = '; echo H2H"" B'
LOADS = {
    'boot': (A + 'echo H2H"" B', 60, ''),
    'gzip': ('seq 1 20000 > /tmp/g; ' + A + 'gzip -c /tmp/g > /dev/null; gzip -c /tmp/g > /dev/null' + B, 120, ''),
    'jpeg': (A + 'NXVIEW_SUM=1 nxview --decode /usr/share/picture-fox.jpg /tmp/b.ppm' + B, 120, r'nxview: .*sum'),
    'png': (A + 'NXVIEW_SUM=1 nxview --decode /usr/share/picture-fox.png /tmp/a.ppm' + B, 120, r'nxview: .*sum'),
    'mp3': (A + 'media_test audio /usr/share/play-test.mp3 /tmp/o.raw 63' + B, 120, r'audio .*sum|sum'),
    'mp3_1': (A + 'media_test audio /usr/share/play-test.mp3 /tmp/o.raw 0' + B, 200, r'audio .*sum|sum'),
    'flac': (A + 'media_test audio /usr/share/play-test.flac /tmp/o.raw 63' + B, 120, r'audio .*sum|sum'),
    'mpeg': (A + 'media_test video "/root/Videos/Big Buck Bunny.mpg" 8' + B, 170, r'sum'),
    'avi': (A + 'media_test video "/root/Videos/Big Buck Bunny.avi" 16' + B, 170, r'sum'),
    'tdawn': (PRE + A + TD + 'TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL' + B, 280, r'done at frame'),
    # the same with no test's sum in what is timed: nxview's and the pictures' are switched off, the audio's ends at the last worker (--cut)
    'jpeg_ns': (A + 'nxview --decode /usr/share/picture-fox.jpg /tmp/b.ppm' + B, 120, r'nxview: '),
    'png_ns': (A + 'nxview --decode /usr/share/picture-fox.png /tmp/a.ppm' + B, 120, r'nxview: '),
    'mp3_ns': (A + 'media_test audio /usr/share/play-test.mp3 /tmp/o.raw 63' + B, 120, r'audio .*sum'),
    'flac_ns': (A + 'media_test audio /usr/share/play-test.flac /tmp/o.raw 63' + B, 120, r'audio .*sum'),
    'mpeg_ns': (A + 'MEDIA_TEST_NOSUM=1 media_test video "/root/Videos/Big Buck Bunny.mpg" 8' + B, 170, r'video: '),
    'avi_ns': (A + 'MEDIA_TEST_NOSUM=1 media_test video "/root/Videos/Big Buck Bunny.avi" 16' + B, 170, r'video: '),
    'mpeg24': (A + 'MEDIA_TEST_NOSUM=1 media_test video "/root/Videos/Big Buck Bunny.mpg" 24' + B, 170, r'video: '),
    'mpeg64': (A + 'MEDIA_TEST_NOSUM=1 media_test video "/root/Videos/Big Buck Bunny.mpg" 64' + B, 170, r'video: '),
    'avi64': (A + 'MEDIA_TEST_NOSUM=1 media_test video "/root/Videos/Big Buck Bunny.avi" 64' + B, 170, r'video: '),
    'mp3_half': (A + 'media_test audio /usr/share/play-test.mp3 /tmp/o.raw 63 11520' + B, 120, r'audio .*sum'),
    'mp3_255': (A + 'media_test audio /usr/share/play-test.mp3 /tmp/o.raw 255 5760' + B, 120, r'audio .*sum'),
}
STALL = {0: 'budget', 1: 'exit call', 2: 'csr full', 3: 'write full', 6: 'uart', 7: 'fence.i', 8: 'copy', 9: 'wfi/pause', 10: 'fill'}
K0, K1 = 0x82001000, 0x823cf868    # the kernel's text and data (nm of ~/nm/k-rom/vmlinux, the image's)
KNM = os.path.join(HERE, 'vmlinux.nm')
names = ([], [])
if os.path.exists(KNM):
    for l in open(KNM):
        p = l.split()
        if len(p) == 3 and p[1] in 'tTwW':
            names[0].append(int(p[0], 16)); names[1].append(p[2])
def sym(pc):
    if not K0 <= pc < K1: return 'user'
    i = bisect.bisect_right(names[0], pc) - 1
    return names[1][i] if i >= 0 else '?'
IDLE = {'arch_cpu_idle', 'default_idle_call', 'do_idle', 'cpu_startup_entry', 'cpuidle_idle_call', 'zeroed_idle'}

def account(frames, lo, hi):
    d = np.fromfile(frames, dtype=np.uint32).reshape(-1, 4).astype(np.int64)
    us = np.diff(d[:, 3], prepend=d[0, 3]).astype(float)
    w = None
    if os.path.exists(frames + '.workers'):
        w = np.fromfile(frames + '.workers', dtype=np.uint32).reshape(-1, 4).astype(np.int64)
        if len(w) != len(d):      # (the first readback has a clock and no workers' line, or the other way)
            k = min(len(w), len(d)); w = w[-k:]; d = d[-k:]; us = us[-k:]
            lo = max(0, lo - (len(np.fromfile(frames, dtype=np.uint32)) // 4 - k)); hi = lo + (hi - lo)
    d, us = d[lo:hi], us[lo:hi]
    wk = w[lo:hi] if w is not None else np.zeros((len(d), 4), dtype=np.int64)
    n = d[:, 1]; st = d[:, 2] & 0xff
    kinds = {}
    cls = []
    for pc, s, c in zip(d[:, 0], st, n):
        pc, s, c = int(pc), int(s), int(c)
        name = sym(pc)
        if s == 9 or name == 'zeroed_idle':
            k = 'idle' if name in IDLE else 'rest' if name == 'shaderemu_gpu_rest' else 'wait gpu' if 'shaderemu_gpu' in name else ('wait kernel ' + name) if name != 'user' else 'pause user'
        elif c >= 8192: k = 'full budget'
        else: k = STALL.get(s, str(s))
        cls.append(k)
    cls = np.array(cls)
    workers_on = wk[:, 0] > 0
    # dt = a + b * core0 + c * worker instructions, on the passes with no outlier
    X = np.stack([np.ones(len(n)), n, wk[:, 0]], axis=1).astype(float)
    keep = us < np.percentile(us, 99.5) + 1
    coef = np.linalg.lstsq(X[keep], us[keep], rcond=None)[0]
    a, b, c = [float(x) for x in coef]
    wall = float(us.sum())
    out = dict(wall_ms=round(wall / 1000, 1), passes=int(len(d)), core0=int(n.sum()), workers=int(wk[:, 0].sum()), fit_us=dict(a=round(a, 1), b_ns=round(b * 1000, 1), c_ns=round(c * 1000, 2)),
               core0_a_pass=round(float(n.mean()), 1) if len(n) else 0,
               most_us=[round(float(x)) for x in np.sort(us)[-5:]])
    groups = {}
    for k in sorted(set(cls.tolist())):
        m = cls == k
        groups[k] = dict(passes=int(m.sum()), share=round(100 * float(m.mean()), 1), wall=round(100 * float(us[m].sum()) / wall, 1), ms=round(float(us[m].sum()) / 1000, 1),
                         a_pass=round(float(n[m].mean())), us_a_pass=round(float(us[m].mean())), worker_passes=int(workers_on[m].sum()))
    out['kinds'] = groups
    # shares of the wall. Nothing run: an idle pass with no worker. Fixed: the fit's a over the rest. Instructions: b and c.
    idle_nowork = np.isin(cls, ['idle']) & ~workers_on
    fixed = a * float((~idle_nowork).sum()) / wall
    idle = float(us[idle_nowork].sum()) / wall
    i0 = b * float(n[~idle_nowork].sum()) / wall
    iw = c * float(wk[:, 0].sum()) / wall
    out['shares'] = dict(idle=round(100 * idle, 1), fixed=round(100 * fixed, 1), core0=round(100 * i0, 1), workers=round(100 * iw, 1), rest=round(100 * (1 - idle - fixed - i0 - iw), 1))
    # time with core 0 waiting while workers run, and what a worker's pass was
    waitc = np.isin(cls, ['rest', 'pause user', 'wait gpu']) | np.char.startswith(cls, 'wait kernel')
    out['core0_waits_with_workers'] = round(100 * float(us[waitc & workers_on].sum()) / wall, 1)
    out['worker_passes'] = dict(with_any=int(workers_on.sum()), cores_a_pass=round(float(wk[workers_on, 1].mean()), 1) if workers_on.any() else 0,
                                a_core=round(float(wk[:, 0].sum() / max(wk[:, 1].sum(), 1))), whole=round(100 * float(wk[:, 2].sum() / max(wk[:, 1].sum(), 1)), 1))
    # where idle and early ends were, by symbol
    pl = {}
    for pc, k, u in zip(d[:, 0], cls, us):
        if k != 'full budget':
            key = k + ' @ ' + sym(int(pc))
            x = pl.setdefault(key, [0, 0.0]); x[0] += 1; x[1] += u
    out['places'] = {k: [v[0], round(v[1] / 1000, 1)] for k, v in sorted(pl.items(), key=lambda e: -e[1][1])[:10]}
    return out

ap = argparse.ArgumentParser()
ap.add_argument('load'); ap.add_argument('--tag', default=''); ap.add_argument('--tick', default=''); ap.add_argument('--image', default='')
ap.add_argument('--env', action='append', default=[]); ap.add_argument('--extra', default=''); ap.add_argument('--no-time', action='store_true')
ap.add_argument('--cut', action='store_true'); ap.add_argument('--out', default=os.path.join(HERE, 'results.jsonl')); ap.add_argument('--cmd', default=''); ap.add_argument('--seconds', type=int, default=0)
a = ap.parse_args()
cmd, secs, proof = LOADS.get(a.load, (a.cmd, a.seconds or 120, ''))
if a.cmd: cmd = a.cmd
if a.seconds: secs = a.seconds
os.makedirs(LOGS, exist_ok=True)
stem = os.path.join(LOGS, a.load + (f'_{a.tag}' if a.tag else ''))
log, frames = stem + '.uart', stem + '.frames'
for f in (log, frames, frames + '.workers'):
    if os.path.exists(f): os.remove(f)
exe = [os.path.join(MAIN, 'bin', 'rvc_harness.exe'), '--d3d11', '--rvc', a.tick or r'experiments\rvc_lean', '--define', 'NO_PAGING',
       '--image', a.image or os.path.join(MAIN, 'build', 'images', 'nommu'), '--no-desktop', '--no-viz', '--no-stdin'] + a.extra.split()
exe += ['--frame-log', frames, '--seconds', str(secs), '--stats-after', '0', '--uart-log', log, '--expect', '/ # ', '--send', cmd + '\\n', '--until', 'H2H B']
env = dict(os.environ, RVC_MARK='H2H ', GPU_QUEUE_WHO='rv_acct')
if not a.no_time: env['RVC_STATS_EVERY'] = '1'
for e in a.env:
    k, v = e.split('=', 1); env[k] = v
q = os.path.join(TREE, 'tools', 'gpu_queue.ps1')
for tr in range(4000):
    p = subprocess.run(['pwsh', '-NoProfile', '-File', q] + exe, cwd=MAIN, capture_output=True, text=True, errors='replace', env=env)
    out = p.stdout + p.stderr
    if 'refused' not in out.lower(): break
    time.sleep(2)
open(stem + '.out', 'w', encoding='utf-8').write(out)
text = open(log, encoding='latin-1').read() if os.path.exists(log) else ''
marks = re.findall(r'mark \d+: tick ([\d.]+) ms a frame over (\d+) frames', out)
r = dict(load=a.load, tag=a.tag, tick=a.tick, date=time.strftime('%m-%d %H:%M'), done='--until matched' in out, marks=marks)
if marks:
    n0 = int(marks[0][1]); n1 = int(marks[1][1]) if len(marks) > 1 else 0
    r['boot'] = account(frames, 0, n0)
    if a.cut and n1 and os.path.exists(frames + '.workers'):
        wk_ = np.fromfile(frames + '.workers', dtype=np.uint32).reshape(-1, 4)
        t_ = np.fromfile(frames, dtype=np.uint32).reshape(-1, 4)
        wk_ = wk_[-len(t_):]; on_ = np.nonzero(wk_[n0:n0 + n1, 0])[0]
        if len(on_): n1 = int(on_[-1]) + 1; r['cut'] = n1
    if n1: r['work'] = account(frames, n0, n0 + n1)
    if len(marks) > 1: r['tick_ms'] = round(float(marks[1][0]) * n1, 1)
m = re.search(r'STATS gpu per frame: tick ([\d.]+) ms, commit ([\d.]+) ms, device ([\d.]+) ms; wall ([\d.]+) ms', out)
if m: r['gpu_per_frame'] = [float(x) for x in m.groups()]
r['proof'] = re.findall(r'[^\n]*(?:' + proof + r')[^\n]*', text)[-4:] if proof else []
r['oops'] = len(re.findall(r'Oops|unhandled signal|Kernel panic|Segmentation', text))
open(a.out, 'a').write(json.dumps(r) + '\n')
print(json.dumps(r, indent=None))
if not r['done']: print('NOT DONE:', out[-800:], '\nCONSOLE:', text[-500:])
