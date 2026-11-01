#!/usr/bin/env python3
"""A longer pass: the lean tick at budgets over 8192, whole runs in turn from one snapshot: instructions a second, the guest's own figure, what ended passes.  python longer.py [--loads ...] [--budgets ...] [--reps 2] [--total 5e7]"""
import argparse, os, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run import run, ROOT
ap = argparse.ArgumentParser(); ap.add_argument('--loads', default='shell,doom,quake,nxray'); ap.add_argument('--budgets', default='8192,12288,16384'); ap.add_argument('--reps', type=int, default=2); ap.add_argument('--total', type=float, default=4.5e7); ap.add_argument('--tick', default='rvc_lean'); ap.add_argument('--code', default='')
a = ap.parse_args()
for rep in range(a.reps):
    for load in a.loads.split(','):
        for n in map(int, a.budgets.split(',')):
            fr = int(a.total / n) if load != 'nxray' else 6000
            r = run('longer', load, a.tick, a.code, n, fr, quiet=True)
            o = r['stdout']
            ipsall = re.findall(r'all cores ips=(\d+)', o)
            util = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'tick_util.py'), os.path.join(ROOT, 'logs', 'acct_longer.frames')], capture_output=True, text=True, errors='replace').stdout.split('\n')
            full = [l for l in util if l.startswith('write cache full')]
            guest = [l for l in r['tail'].split('\n') if l.strip()]
            print(load, 'N', n, 'frames', fr, 'ms %.3f' % r.get('ms', 0), 'per %.0f' % r.get('per', 0), 'M/s %.3f' % (r.get('ips', 0) / 1e6), 'all %.1f' % (int(ipsall[-1]) / 1e6 if ipsall else 0), 'bad', r['bad'], 'clock', r['clock'], '|', (full[0][20:60].strip() if full else 'none full'), '|', (guest[-1][:90] if guest else ''), flush=True)
