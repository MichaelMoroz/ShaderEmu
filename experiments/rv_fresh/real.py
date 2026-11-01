#!/usr/bin/env python3
"""Real code on the card: python real.py SHADER LOAD[,LOAD] [--reps 4] [-D X=V] [--check] [--note T]
From a load's snapshot up to the first thing the tick lacks; the first go of each run is not timed. --check holds the end's registers and every byte of RAM against snapsim.py."""
import argparse, os, re, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
SNAP = os.environ.get('AGENTS_ISA', r'C:\Users\micha\agents_isa')
sys.path.insert(0, HERE)
import snapsim
ap = argparse.ArgumentParser(); ap.add_argument('shader'); ap.add_argument('loads'); ap.add_argument('--reps', default='4'); ap.add_argument('-D', action='append', default=[]); ap.add_argument('--check', action='store_true'); ap.add_argument('--note', default=''); ap.add_argument('--snapfile', default=''); ap.add_argument('--twice', action='store_true')
a = ap.parse_args()
empty = os.path.join(ROOT, 'logs', 'fresh_empty.bin'); open(empty, 'wb').write(b'\0' * 16)
for load in a.loads.split(','):
    snap = a.snapfile or os.path.join(SNAP, 'snap', load, load + '.snap')
    dump = os.path.join(ROOT, 'logs', 'fresh_ram.bin')
    cmd = ['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1'), os.path.join(ROOT, 'bin', 'fresh.exe'), os.path.join(HERE, a.shader), empty, '--core', '8x8', '--snap', snap, '--reps', a.reps,
           '--commit', os.path.join(HERE, 'steps', 'commit.hlsl'), '368']
    if a.check: cmd += ['--dump-ram', dump]
    if a.twice: cmd += ['--twice']
    for d in a.D: cmd += ['-D', d]
    o = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace').stdout
    m = re.search(r'SNAP .*', o)
    print(load, m.group(0) if m else o[-800:], flush=True)
    if not m: continue
    open(os.path.join(HERE, 'steps.log'), 'a').write(f"{time.strftime('%H:%M')} {a.shader} real {load} {' '.join(a.D)} {m.group(0)} {a.note}\n")
    if a.check:
        count = int(re.search(r'count=(\d+)', o).group(1))
        sn = snapsim.Snap(snap); x, pc = snapsim.regs(sn)
        r, *_ = snapsim.run(sn, x, pc, count)
        got = []
        for l in re.findall(r'texel \d: (\w+) (\w+) (\w+) (\w+)', o): got += [int(v, 16) for v in l]
        bad = [k for k in range(32) if got[k] != x[k]]
        print('  registers: %d differ %s; pc model %08x' % (len(bad), bad[:6], r['pc']))
        base = snapsim.HDR + 64 * 2048 * 16
        ramf = open(dump, 'rb').read()
        diff = 0; same = 0
        for at, val in sn.writes.items():
            if ramf[at & 0x7fffffff] != val: diff += 1
            else: same += 1
        # and nothing else changed
        import numpy as np
        before = np.frombuffer(sn.m, dtype=np.uint8, offset=base, count=min(len(ramf), len(sn.m) - base))
        after = np.frombuffer(ramf, dtype=np.uint8, count=len(before))
        changed = np.nonzero(before != after)[0]
        extra = sum(1 for c in changed if (int(c) | 0x80000000) not in sn.writes)
        print('  memory: %d bytes the model wrote are right, %d wrong; %d bytes changed that the model did not write' % (same, diff, extra))
