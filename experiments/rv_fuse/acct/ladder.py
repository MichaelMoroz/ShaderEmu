#!/usr/bin/env python3
"""ms a pass against the pass's budget, each tick, each load, whole runs in turn from one snapshot: is 1.03 ms a pass's fixed part?   python ladder.py [--loads doom,shell] [--reps 2]"""
import argparse, sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run import run
ap = argparse.ArgumentParser(); ap.add_argument('--loads', default='doom,shell'); ap.add_argument('--reps', type=int, default=2); ap.add_argument('--budgets', default='2,256,1024,2048,4096,8192'); ap.add_argument('--total', type=float, default=3e7)
a = ap.parse_args()
for rep in range(a.reps):
    for load in a.loads.split(','):
        for n in map(int, a.budgets.split(',')):
            fr = int(max(3000, min(a.total / n, 120000)))
            for t, c in (('rvc_lean', ''), ('rvc_lean_w3x', load + '.3.2.code')):
                r = run(f'ladder_{load}', load, t, c, n, fr, {'RVC_PASS_PROF': '1'}, quiet=True)
                print(load, t, 'N', n, 'frames', fr, 'ms %.4f' % r.get('ms', 0), 'per %.1f' % r.get('per', 0), 'bad', r['bad'], 'clock', r['clock'], '|', ' '.join(r['prof'][-2:])[:230], flush=True)
