#!/usr/bin/env python3
"""A tick's prices by instruction kind on the card (programs/classes, bare metal): python classes.py [TICK,TICK...] [--marks 0,1,2,3] [--reps N]
Each tick in turn, ns an instruction from mark to mark, by the harness's own STATS line."""
import argparse, os, re, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
NAMES = ['alu', 'load', 'store', 'two', 'call', 'stride', 'jumps', 'add']
ap = argparse.ArgumentParser(); ap.add_argument('ticks', nargs='?', default='rvc_lean'); ap.add_argument('--marks', default='0,1,2,3'); ap.add_argument('--reps', type=int, default=2)
a = ap.parse_args()
for rep in range(a.reps):
    for k in a.marks.split(','):
        k = int(k)
        row = []
        for t in a.ticks.split(','):
            cmd = ['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1'), os.path.join(ROOT, 'bin', 'rvc_harness.exe'), '--d3d11',
                   '--rvc', os.path.join('experiments', t), '--define', 'NO_PAGING', '--image', os.path.join(ROOT, 'logs', 'classes_img'), '--no-viz', '--no-stdin',
                   '--stats-from', 'P%d' % k, '--until', 'P%d' % (k + 1)] + ([] if os.environ.get('NOTO') else [])
            for _ in range(400):
                o = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace'); o = o.stdout + o.stderr
                if 'refused' not in o: break
            m = re.findall(r'^STATS seconds=([\d.]+) instructions=(\d+)', o, re.M)
            row.append('%s=%.1f' % (t, 1e9 * float(m[-1][0]) / max(1, int(m[-1][1]))) if m else t + '=? ' + o[-300:])
        print(NAMES[k], ' '.join(row), flush=True)
