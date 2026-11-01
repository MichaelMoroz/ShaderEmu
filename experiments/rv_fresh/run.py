#!/usr/bin/env python3
"""A step's measure: python run.py SHADER PROG [--core 4x8] [--budget 8192] [--passes 800] [-D X=V] [--reps 3] [--note TEXT] [--dump 8] [--warp]
PROG names a function of progs.py. Goes through the queue, prints the runs' RESULT lines and their middle ns; a line in steps.log."""
import argparse, os, re, statistics, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import progs
ap = argparse.ArgumentParser(); ap.add_argument('shader'); ap.add_argument('prog'); ap.add_argument('--core', default='4x8'); ap.add_argument('--budget', default='8192'); ap.add_argument('--passes', default='800')
ap.add_argument('-D', action='append', default=[]); ap.add_argument('--reps', type=int, default=3); ap.add_argument('--note', default=''); ap.add_argument('--dump', default='0'); ap.add_argument('--warp', action='store_true'); ap.add_argument('--commit', default='')
a = ap.parse_args()
ram = os.path.join(ROOT, 'logs', 'fresh_%s.bin' % a.prog); os.makedirs(os.path.dirname(ram), exist_ok=True)
open(ram, 'wb').write(getattr(progs, a.prog)())
cmd = ['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1'), os.path.join(ROOT, 'bin', 'fresh.exe'), os.path.join(HERE, a.shader), ram, '--core', a.core, '--budget', a.budget, '--passes', a.passes, '--dump', a.dump]
if a.warp: cmd += ['--warp']
if a.commit: f, c = a.commit.split(','); cmd += ['--commit', os.path.join(HERE, f), c]
for d in a.D: cmd += ['-D', d]
ns = []
for i in range(a.reps):
    o = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace').stdout
    m = re.search(r'RESULT .*', o)
    print(m.group(0) if m else o[-800:])
    if a.dump != '0' and i == a.reps - 1: print('\n'.join(l for l in o.splitlines() if l.startswith('texel')))
    if m:
        ns.append((float(re.search(r'ns_a_instr=([\d.]+)', o).group(1)), float(re.search(r'pass_us=([\d.]+)', o).group(1)), re.search(r'tick_ns=([\d.]+)', o).group(1), re.search(r'commit_us=([\d.]+)', o).group(1)))
if ns:
    line = f"{time.strftime('%H:%M')} {a.shader} {a.prog} core={a.core} budget={a.budget} {' '.join(a.D)} ns={statistics.median(n[0] for n in ns):.2f} pass_us={statistics.median(n[1] for n in ns):.1f} tick_ns={ns[-1][2]} commit_us={ns[-1][3]} {a.note}"
    print(line); open(os.path.join(HERE, 'steps.log'), 'a').write(line + '\n')
