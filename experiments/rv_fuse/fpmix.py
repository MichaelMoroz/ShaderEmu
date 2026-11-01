#!/usr/bin/env python3
"""What the old step does in a load: python fpmix.py PCSDIR ... : shares of RV instructions by float form, and how long the runs of floats are between other words."""
import os, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
_a = sys.argv[1:]
sys.argv = sys.argv[:1] + ['x', 'x', 'x']
from study import kind, load  # noqa: E402
F5 = {0: 'fadd', 1: 'fsub', 2: 'fmul', 3: 'fdiv', 11: 'fsqrt', 4: 'fsgnj', 5: 'fminmax', 24: 'fcvt.w', 26: 'fcvt.s', 28: 'fmv.x', 30: 'fmv.w', 20: 'fcmp'}
for d in _a:
    c, tot = collections.Counter(), 0
    pcs = {}
    for _, t, pc, w, n, l, s, nt, fl in load(d):
        e = pcs.get(pc)
        if e and e[0] == w: e[1] += n
        elif not e or n > e[1]: pcs[pc] = [w, n]
    runs = collections.Counter()
    for pc, (w, n) in pcs.items():
        tot += n
        k = kind(w)
        if k == 'fp': c[F5.get(w >> 27, 'fp?')] += n
        elif k in ('flw', 'fsw', 'fmadd'): c[k] += n
        elif k == 'mul' : c['mul' if ((w >> 12) & 7) == 0 else 'mulh/div'] += n
        elif k == 'amo': c['amo'] += n
        p = pcs.get(pc - 4)
        isf = k in ('fp', 'flw', 'fsw', 'fmadd')
        if isf and not (p and kind(p[0]) in ('fp', 'flw', 'fsw', 'fmadd')):
            at, m = pc, 0
            while at in pcs and kind(pcs[at][0]) in ('fp', 'flw', 'fsw', 'fmadd'): m += 1; at += 4
            runs[min(m, 12)] += n
    print(os.path.basename(d.rstrip('/\\')), f'{tot:,}', ' '.join(f'{a} {100 * b / tot:.1f}' for a, b in c.most_common(16)))
    t = sum(runs.values()) or 1
    print('   runs of floats (weighted by entries):', ' '.join(f'{a}:{100 * b / t:.0f}' for a, b in sorted(runs.items())))
