#!/usr/bin/env python3
"""The wall of each load's work split from its per-pass log, with a pass's fixed cost measured, not fitted:
fixed = an empty tick (0.036 ms, rv_fuse's ladder) + the run's own commit, device and wall-over-timestamps a pass.
Core 0's price is what passes with no worker cost over that; in a pass with workers, what is over core 0's instructions at
that price is the workers'."""
import json, os, re, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
last = {}
for l in open(os.path.join(HERE, 'results.jsonl')):
    r = json.loads(l); last[r['load']] = r
print('%-7s %7s %6s %6s %6s %7s %6s | %5s %6s %6s %6s %6s %6s | %5s %5s' % ('load', 'wall ms', 'c0 M', 'wk M', 'instr', 'c0 ns', 'wk ns', 'idle', 'fixed', 'early', 'c0', 'wk', 'other', 'full', 'cores'))
for load, r in last.items():
    stem = os.path.join(HERE, 'logs', load)
    d = np.fromfile(stem + '.frames', dtype=np.uint32).reshape(-1, 4).astype(np.int64)
    us = np.diff(d[:, 3], prepend=d[0, 3]).astype(float)
    w = np.zeros((len(d), 4), dtype=np.int64)
    if os.path.exists(stem + '.frames.workers'):
        x = np.fromfile(stem + '.frames.workers', dtype=np.uint32).reshape(-1, 4).astype(np.int64)
        w[-len(x):] = x[-len(d):]
    n0 = int(r['marks'][0][1])
    n1 = int(r['marks'][1][1]) if len(r['marks']) > 1 and load != 'boot' else 0
    n1 = r.get('cut', n1); lo, hi = (0, n0) if load == 'boot' else (n0, n0 + n1)
    d, us, w = d[lo:hi], us[lo:hi], w[lo:hi]
    t, c, dv, wl = r['gpu_per_frame']
    fix = 36 + 1000 * (c + dv + max(wl - t - c - dv, 0))
    n = d[:, 1]; st = d[:, 2] & 255
    wall = us.sum()
    idle = np.zeros(len(d), bool)
    if load == 'boot':
        idle = (st == 9) & (w[:, 0] == 0)
    idle_ms = us[idle].sum()
    on = w[:, 0] > 0
    alone = ~on & ~idle
    rate = (us[alone] - fix).sum() / max(n[alone].sum(), 1)
    fixed = fix * (~idle).sum()
    ideal = n[~idle].sum() / 8192
    early = fix * max((~idle).sum() - ideal, 0)
    c0 = rate * n[~idle].sum()
    wk = (us[on] - fix - rate * n[on]).sum()
    wkrate = wk / max(w[:, 0].sum(), 1)
    other = wall - idle_ms - fixed - c0 - wk
    p = lambda v: 100 * v / wall
    print('%-7s %7.0f %6.1f %6.1f %6.0f %7.1f %6.1f | %5.1f %6.1f %6.1f %6.1f %6.1f %6.1f | %5.1f %5.1f' % (load, wall / 1000, n.sum() / 1e6, w[:, 0].sum() / 1e6, n.mean(), rate * 1000, wkrate * 1000, p(idle_ms), p(fixed - early), p(early), p(c0), p(wk), p(other),
          100 * (st == 3).mean(), w[on, 1].mean() if on.any() else 0))
