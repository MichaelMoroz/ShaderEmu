#!/usr/bin/env python3
"""How many RV instructions a texel of ops takes in order, by a profile's counts, for shapes of bundle.
    python packing.py PCSDIR [PCSDIR ...]
A shape is a string of slots a bundle has, in the order they run: a arithmetic of the unit or sum, s a sum, m a load or store,
f a float operation, c control. An op takes the first slot from where the last one stood that can hold it; a target or a change of
count ends the bundle, and so does control. A lui/auipc+addi is one op (a constant within a slot's word is assumed); no reordering."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
_a = sys.argv[1:]
sys.argv = sys.argv[:1] + ['x', 'x', 'x']
from study import kind, load  # noqa: E402

SHAPES = ['ac', 'asc', 'mc', 'msc', 'amc', 'amsc', 'aamc', 'aamsc', 'aamac', 'amamc', 'mamc', 'amasc', 'aammc', 'afmc', 'amafc', 'aafmc', 'amamfc', 'amfmc']


def sx(v, b):
    return (v ^ (1 << (b - 1))) - (1 << (b - 1))


def imm_b(w): return sx(((w >> 31) << 12) | ((w & 0x80) << 4) | ((w >> 20) & 0x7e0) | ((w >> 7) & 0x1e), 13)
def imm_j(w): return sx(((w >> 31) << 20) | (w & 0xff000) | ((w >> 9) & 0x800) | ((w >> 20) & 0x7fe), 21)


def cls(w):
    k = kind(w)
    if k in ('alui', 'alur'):
        f3 = (w >> 12) & 7
        sub = k == 'alur' and (w >> 30) & 1
        return 's' if f3 == 0 and not sub else 'a'
    if k in ('lui', 'auipc'): return 's'
    if k in ('load', 'store', 'flw', 'fsw'): return 'm'
    if k in ('branch', 'jal', 'jalr'): return 'c'
    if k == 'fp': return 'f'
    return None


for d in _a:
    pcs = {}
    for c, t, pc, w, n, l, s, nt, fl in load(d):
        e = pcs.get(pc)
        if e and e[0] == w: e[1] += n
        elif not e or n > e[1]: pcs[pc] = [w, n]
    starts = set()
    for pc, (w, n) in pcs.items():
        k = kind(w)
        if k == 'branch': starts |= {pc + imm_b(w), pc + 4}
        elif k == 'jal': starts |= {pc + imm_j(w), pc + 4}
        elif cls(w) is None or k == 'jalr': starts.add(pc + 4)
        p = pcs.get(pc - 4)
        if not p or p[1] != n: starts.add(pc)
    print(os.path.basename(d.rstrip('/\\')))
    total = sum(v[1] for v in pcs.values())
    mix = {}
    for w, n in pcs.values(): mix[cls(w)] = mix.get(cls(w), 0) + n
    print('   ' + ', '.join(f'{k}: {100 * v / total:.1f}%' for k, v in sorted(mix.items(), key=lambda kv: -kv[1])))
    for shape in SHAPES:
        rv = bundles = 0
        unit_ok = {'a': 'as', 's': 'sa', 'm': 'm', 'f': 'f', 'c': 'c'}
        for pc in sorted(pcs):
            if pc not in starts and pc - 4 in pcs: continue
            at = pc
            while at in pcs:
                w0, n0 = pcs[at]
                slot = 0
                count = 0
                while at in pcs and (count == 0 or at not in starts):
                    c = cls(pcs[at][0])
                    if c is None:
                        if count == 0:
                            count = 1
                            at += 4
                        break
                    q = slot
                    while q < len(shape) and shape[q] not in unit_ok[c]: q += 1
                    if q == len(shape):
                        if count == 0:   # no slot of its kind in this shape: by a slow way, a bundle of its own
                            count = 1
                            at += 4
                        break
                    slot = q + 1
                    count += 1
                    at += 4
                    if c == 'c': break
                bundles += n0
                rv += n0 * count
        print(f'   {shape:8s} {rv / bundles:.2f}')
