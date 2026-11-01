#!/usr/bin/env python3
"""Scratch array variants against the tree in turn, from one snapshot on the card, warm (rv_newisa/ab.py with the clock read and a blob for each side):
python ab.py LOADS VAR[;VAR...] [--runs 3] [--secs 9] [--out ab.jsonl]. VAR: defines joined by commas, or nothing for the tree.
Apply lean_sc.patch to a copy of rvc_lean named experiments/rvc_lean_sc first."""
import argparse, json, os, re, subprocess, threading, time
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
MAIN = subprocess.run(['git', 'worktree', 'list', '--porcelain'], cwd=ROOT, capture_output=True, text=True).stdout.split('\n')[0].split(' ', 1)[1]
BIN = os.path.join(MAIN, 'bin', 'rvc_harness.exe')
SNAPS = r'C:\Users\micha\agents_isa\ni'
CACHE = os.environ.get('SC_CACHE', os.path.join(ROOT, 'build', 'sc_cache'))
def clocks(stop, out):
    while not stop.wait(1.0):
        q = subprocess.run(['nvidia-smi', '--query-gpu=clocks.gr,utilization.gpu', '--format=csv,noheader,nounits'], capture_output=True, text=True).stdout.split(',')
        if len(q) == 2 and int(q[1]) >= 50: out.append(int(q[0]))
def run(name, defs, load, secs):
    a = [BIN, '--d3d11', '--rvc', os.path.join(ROOT, 'experiments', 'rvc_lean_sc'), '--define', 'NO_PAGING', '--image', os.path.join(MAIN, 'build', 'images', 'nommu'),
         '--cache', os.path.join(CACHE, name.replace(',', '_').replace('=', '')), '--load-state', os.path.join(SNAPS, load, load + '.snap'),
         '--no-desktop', '--no-viz', '--no-stdin', '--seconds', str(secs), '--stats-after', '4']
    for d in defs: a += ['--define', d]
    e = dict(os.environ, GPU_QUEUE_WHO='rv_scratch ab')
    stop, seen = threading.Event(), []
    th = threading.Thread(target=clocks, args=(stop, seen)); th.start()
    for tr in range(200):
        p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1')] + a, cwd=ROOT, capture_output=True, text=True, errors='replace', env=e)
        o = p.stdout + p.stderr
        if p.returncode != 75: break
        time.sleep(3)
    stop.set(); th.join()
    s = re.search(r'STATS seconds=.*instructions=(\d+).*frames=(\d+).*per_frame=([\d.]+)', o)
    g = re.search(r'tick ([\d.]+) ms, commit ([\d.]+) ms, device ([\d.]+) ms; wall ([\d.]+) ms', o)
    w = re.search(r'workers: instructions=(\d+)', o)
    if not s or not g:
        print('NO STATS', name, p.returncode, o[-800:], flush=True); return None
    per, wall = float(s.group(3)), float(g.group(4))
    seen.sort()
    return dict(per=per, wall=wall, tick=float(g.group(1)), ns=1e6 * wall / max(per, 1), workers=int(w.group(1)) if w else 0, instr=int(s.group(1)), clock=seen[len(seen) // 2] if seen else 0, clock_min=seen[0] if seen else 0)
ap = argparse.ArgumentParser(); ap.add_argument('loads'); ap.add_argument('vars'); ap.add_argument('--runs', type=int, default=3); ap.add_argument('--secs', type=int, default=9); ap.add_argument('--out', default='ab.jsonl')
a = ap.parse_args()
sides = [(v or 'tree', [x for x in v.split(',') if x]) for v in a.vars.split(';')]
for r in range(a.runs):
    for load in a.loads.split(','):
        for name, defs in sides:
            x = run(name, defs, load, a.secs)
            if x:
                print(f"{time.strftime('%H:%M:%S')} {load:6} {r} {name:18} ns {x['ns']:7.1f} tick {x['tick']:.3f} wall {x['wall']:.3f} pass {x['per']:.0f} clock {x['clock']} (min {x['clock_min']}) w {x['workers']/1e6:.1f}M", flush=True)
                open(os.path.join(HERE, a.out), 'a').write(json.dumps(dict(t=time.strftime('%H:%M:%S'), load=load, var=name, run=r, **x)) + '\n')
