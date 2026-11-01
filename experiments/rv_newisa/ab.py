#!/usr/bin/env python3
"""A tick's variants against the tree in turn, from one snapshot on the card, warm: python ab.py TICK LOADS VAR[;VAR...] [--runs 3] [--secs 10]
VAR: defines joined by commas, or tick:FOLDER(,defines). Prints wall ms a frame and ns an instruction per side; appends to ab.jsonl."""
import argparse, json, os, re, subprocess, time
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
MAIN = subprocess.run(['git', '-C', ROOT, 'worktree', 'list', '--porcelain'], capture_output=True, text=True).stdout.split('\n')[0].split(' ', 1)[1]
BIN = os.path.join(ROOT, 'bin', 'rvc_harness.exe')
if not os.path.exists(BIN): BIN = os.path.join(MAIN, 'bin', 'rvc_harness.exe')
SNAPS = r'C:\Users\micha\agents_isa\ni'
def run(tick, defs, load, secs, env=None):
    snap = os.path.join(SNAPS, load, load + '.snap')
    a = [BIN, '--d3d11', '--rvc', os.path.join(ROOT, 'experiments', tick), '--define', 'NO_PAGING', '--image', os.path.join(MAIN, 'build', 'images', 'nommu'),
         '--load-state', snap, '--no-desktop', '--no-viz', '--no-stdin', '--seconds', str(secs), '--stats-after', '4']
    for d in defs: a += ['--define', d]
    e = dict(os.environ, GPU_QUEUE_WHO='rv_newisa ab')
    e.update(env or {})
    for tr in range(200):
        p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1')] + a, cwd=ROOT, capture_output=True, text=True, errors='replace', env=e)
        o = p.stdout + p.stderr
        if p.returncode != 75: break
        time.sleep(3)
    s = re.search(r'STATS seconds=.*instructions=(\d+).*frames=(\d+).*per_frame=([\d.]+)', o)
    g = re.search(r'tick ([\d.]+) ms, commit ([\d.]+) ms, device ([\d.]+) ms; wall ([\d.]+) ms', o)
    w = re.search(r'workers: instructions=(\d+)', o)
    if not s or not g:
        print('NO STATS', o[-1500:]); return None
    per, wall = float(s.group(3)), float(g.group(4))
    return dict(per=per, wall=wall, tick=float(g.group(1)), ns=1e6 * wall / max(per, 1), workers=int(w.group(1)) if w else 0, instr=int(s.group(1)))
ap = argparse.ArgumentParser(); ap.add_argument('tick'); ap.add_argument('loads'); ap.add_argument('vars'); ap.add_argument('--runs', type=int, default=3); ap.add_argument('--secs', type=int, default=9)
a = ap.parse_args()
sides = []
for v in a.vars.split(';'):
    bits = [x for x in v.split(',') if x]
    tick = a.tick
    if bits and bits[0].startswith('tick:'): tick = bits.pop(0)[5:]
    sides.append((v or 'tree', tick, bits))
for load in a.loads.split(','):
    res = {s[0]: [] for s in sides}
    for r in range(a.runs):
        for name, tick, defs in sides:
            x = run(tick, defs, load, a.secs)
            if x: res[name].append(x)
    base = None
    for name, tick, defs in sides:
        rr = res[name]
        if not rr: continue
        ns = sorted(x['ns'] for x in rr); med = ns[len(ns) // 2]
        base = base or med
        wk = sum(x['workers'] for x in rr) / len(rr)
        print(f"{load:8} {name:34} ns {med:7.1f} ({min(ns):.1f}-{max(ns):.1f})  x {base / med:.3f}  a pass {rr[0]['per']:7.1f}  wall {rr[0]['wall']:.3f} tick {rr[0]['tick']:.3f} workers {wk/1e6:.1f}M", flush=True)
        open(os.path.join(HERE, 'ab.jsonl'), 'a').write(json.dumps(dict(t=time.strftime('%H:%M'), load=load, var=name, tick=tick, runs=rr)) + '\n')
