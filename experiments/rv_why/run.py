#!/usr/bin/env python3
"""A load from its snapshot on the card (or --warp), to its end: python run.py LOAD TICK [--warp] [--secs N] [--tag T] [--env A=B] [--extra ARGS]
TICK a folder name under experiments/, as rvc_lean or rvc_lean_X. Appends to results.jsonl: wall from the per-pass log, instructions, what ended passes, the load's proof."""
import argparse, json, os, re, struct, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.abspath(os.path.join(HERE, '..', '..'))
MAIN = subprocess.run(['git', '-C', TREE, 'worktree', 'list', '--porcelain'], capture_output=True, text=True).stdout.split('\n')[0].split(' ', 1)[1]
OUT = os.environ.get('AOT_OUT', r'C:\Users\micha\agents_isa\aot')
UNTIL = {'tdawn': ('LOAD', r'done at frame.*'), 'gzip': ('LOAD', ''), 'png': ('LOAD', ''), 'tdawn0': ('LOAD', r'done at frame.*'), 'shell': ('LOAD', '')}
ap = argparse.ArgumentParser()
ap.add_argument('load'); ap.add_argument('tick'); ap.add_argument('--warp', action='store_true'); ap.add_argument('--secs', type=int, default=120); ap.add_argument('--tag', default='')
ap.add_argument('--env', action='append', default=[]); ap.add_argument('--extra', default=''); ap.add_argument('--out', default=os.path.join(HERE, 'results.jsonl')); ap.add_argument('--snap', default='')
a = ap.parse_args()
until, proof = UNTIL[a.load]
d = os.path.join(OUT, a.load)
os.makedirs(os.path.join(HERE, 'logs'), exist_ok=True)
stem = os.path.join(HERE, 'logs', f'{a.load}_{a.tick}' + (f'_{a.tag}' if a.tag else ''))
log, frames = stem + '.uart', stem + '.frames'
for f in (log, frames):
    if os.path.exists(f): os.remove(f)
exe = [os.path.join(MAIN, 'bin', 'rvc_harness.exe'), '--d3d11'] + (['--warp'] if a.warp else []) + ['--rvc', os.path.join(TREE, 'experiments', a.tick), '--define', 'NO_PAGING',
       '--image', os.path.join(MAIN, 'build', 'images', 'nommu'), '--load-state', a.snap or os.path.join(d, a.load + '.snap'), '--no-desktop', '--no-viz', '--no-stdin',
       '--frame-log', frames, '--seconds', str(a.secs), '--stats-after', '0', '--uart-log', log, '--until', until] + a.extra.split()
env = dict(os.environ, RVC_STATS_EVERY='1', GPU_QUEUE_WHO='rv_why run')
for e in a.env:
    k, v = e.split('=', 1); env[k] = v
q = os.path.join(TREE, 'tools', 'gpu_queue.ps1')
t0 = time.time()
for tr in range(4000):
    p = subprocess.run(['pwsh', '-NoProfile', '-File', q] + exe, cwd=MAIN, capture_output=True, text=True, errors='replace', env=env)
    out = p.stdout + p.stderr
    if p.returncode != 75: break
    time.sleep(2)
text = open(log, encoding='latin-1').read() if os.path.exists(log) else ''
r = dict(load=a.load, tick=a.tick, tag=a.tag, warp=a.warp, date=time.strftime('%m-%d %H:%M'), done='--until matched' in out, rc=p.returncode, host_s=round(time.time() - t0, 1))
WHY = {0: 'budget', 1: 'exit call', 2: 'csr full', 3: 'full', 6: 'uart', 7: 'fence.i', 8: 'copy', 9: 'wfi/pause', 10: 'fill'}
if os.path.exists(frames):
    raw = open(frames, 'rb').read()
    rec = list(struct.iter_unpack('<4I', raw[:len(raw) // 16 * 16]))
    if rec:
        ins = sum(x[1] for x in rec); why = {}
        for x in rec:
            w = why.setdefault(WHY.get(x[2] & 0xff, str(x[2] & 0xff)), [0, 0]); w[0] += 1; w[1] += x[1]
        r.update(wall_ms=round((rec[-1][3] - rec[0][3]) / 1000, 1), passes=len(rec), core0=ins, a_pass=round(ins / len(rec), 1),
                 why={k: dict(share=round(100 * v[0] / len(rec), 1), a_pass=round(v[1] / v[0])) for k, v in sorted(why.items(), key=lambda e: -e[1][0])})
r['proof'] = re.findall(proof, text)[-1:] if proof else []
r['oops'] = len(re.findall(r'Oops|unhandled signal|Kernel panic|Segmentation', text))
r['stats'] = [l[:200] for l in out.split('\n') if re.match(r'(STATS|\[harness\] \d+)', l)][-4:]
open(a.out, 'a').write(json.dumps(r) + '\n')
print(json.dumps({k: v for k, v in r.items() if k not in ('date',)}))
if not r['done']: print('NOT DONE:', out[-1500:], '\nCONSOLE:', text[-500:])
