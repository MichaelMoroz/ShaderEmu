#!/usr/bin/env python3
"""Fuses a snapshot's code in place from a profile of the run that made it (snaps.ps1).

    python fuse_snap.py IN.snap PCSDIR OUT.snap --set addi

Only words the profile saw run, and still in RAM as they ran, are changed: a first word's bit 0 cleared when the next
word is a plain instruction of the set and the two lie in one texel. No branch-target analysis is needed: a jump to
the second word finds it as it was. Pairs are taken along the code by the hottest first.
"""
import argparse, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from second import name, FIRST  # noqa: E402  (its body runs on no folders here)
from study import kind, load, rd  # noqa: E402

HDR, W, ROW0 = 32, 2048, 64
ARITH = {'addi', 'add', 'sub'}
BR = {'b.eq', 'b.ne', 'b.lt', 'b.ge', 'b.ltu', 'b.geu'}
FADD = 'fadd.s'
SETS = {'addi': {'addi'}, 'arith': ARITH, 'addbr': ARITH | BR, 'addbrf': ARITH | BR | {'fsimple'}}

ap = argparse.ArgumentParser()
ap.add_argument('snap')
ap.add_argument('pcs')
ap.add_argument('out')
ap.add_argument('--set', default='addi')
ap.add_argument('--straddle', action='store_true')
ap.add_argument('--fp', action='store_true', help='flw, fsw, F ops as first ops')
ap.add_argument('--min', type=int, default=1, help='fuse only pairs run so many times')
a = ap.parse_args()
S = SETS[a.set]

raw = np.fromfile(a.snap, dtype=np.uint8)
H = int(raw[:16].view(np.uint32)[3])
base = HDR + ROW0 * W * 16


def at(pc):
    return base + (pc & 0x7fffffff)


rows = load(a.pcs)
pcs = {}
for c, t, pc, w, n, l, s, nt, fl in rows:
    e = pcs.get(pc)
    if e:
        # two words seen at one place (a page used again): the one in RAM now counts below
        if e[0] == w: e[1] += n
        elif n > e[1]: pcs[pc] = [w, n]
    else:
        pcs[pc] = [w, n]
words = raw[base:base + ((1 << 28) + (1 << 25))].view(np.uint32) if False else None
cands = []
stale = 0
for pc, (w, n) in pcs.items():
    if pc < 0x80000000: continue
    if kind(w) not in ('alui', 'alur', 'load', 'store') + (('flw', 'fsw', 'fp') if a.fp else ()): continue
    if kind(w) in ('alui', 'alur', 'load') and (w >> 7) & 31 == 0: continue
    if (w & 3) != 3: continue
    if kind(w) == 'fp' and rd(w) < 32: continue   # (a float op's integer result is written after the second op)
    if ((pc >> 2) & 3 == 3 and not a.straddle) or pc & 3: continue
    nx = pcs.get(pc + 4)
    if not nx: continue
    nn = 'fsimple' if kind(nx[0]) == 'fp' and (nx[0] >> 25) in (0, 4, 8) else name(nx[0])
    if nn not in S: continue
    if (nx[0] >> 7) & 31 == 0 and kind(nx[0]) not in ('branch', 'fp'): continue
    if min(n, nx[1]) < a.min: continue
    cands.append((min(n, nx[1]), pc))
cands.sort(reverse=True)
used = set()
fused = 0
merged = 0
total = sum(v[1] for v in pcs.values())
for c, pc in cands:
    if pc in used or pc + 4 in used: continue
    o = at(pc)
    now = int.from_bytes(raw[o:o + 4].tobytes(), 'little')
    nxt = int.from_bytes(raw[o + 4:o + 8].tobytes(), 'little')
    if now != pcs[pc][0] or nxt != pcs[pc + 4][0]:
        stale += 1
        continue
    used |= {pc, pc + 4}
    raw[o] = raw[o] & 0xfe
    fused += 1
    merged += c
raw.tofile(a.out)
print(f'{fused:,} words fused ({stale:,} no longer in RAM as they ran); over the profile {merged:,} of {total:,} instructions merged, {100 * merged / total:.1f}%')
