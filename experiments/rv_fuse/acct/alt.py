#!/usr/bin/env python3
"""Ticks side by side in one run on one clock (RVC_TICK_ALT), from a load's snapshot: python alt.py TICKS(a,b) [--loads shell,doom,quake,nxray] [--frames 8000] [--reps 2]
Prints each tick's ms of a frame's tick and the guest's last line."""
import argparse, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run import run, ROOT
ap = argparse.ArgumentParser(); ap.add_argument('ticks'); ap.add_argument('--loads', default='shell,doom,quake,nxray'); ap.add_argument('--frames', type=int, default=8000); ap.add_argument('--reps', type=int, default=2); ap.add_argument('--snap', default='')
a = ap.parse_args()
alt = ';'.join(os.path.join(ROOT, 'experiments', t) for t in a.ticks.split(','))
for rep in range(a.reps):
    for load in a.loads.split(','):
        r = run('alt_' + load, load, 'rvc_lean', '', 0, a.frames, {'RVC_TICK_ALT': alt}, quiet=True)
        rows = re.findall(r'^STATS tick (\d+) (\S+): ([\d.]+) ms a frame', r['stdout'], re.M)
        base = float(rows[0][2]) if rows else 0
        print(load, 'bad', r['bad'], 'clock', r['clock'], ' '.join(f"{os.path.basename(n.rstrip('/'))}={float(v):.4f}({100 * (base / float(v) - 1):+.2f}%)" for _, n, v in rows), '|', r['tail'].strip().split('\n')[-1][:70], flush=True)
        if not rows: print(r['stdout'][-600:])
