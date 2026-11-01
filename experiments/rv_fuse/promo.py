#!/usr/bin/env python3
"""What a translator could take out of memory, by a profile's counts: python promo.py PCSDIR [...]
Of each load's and store's executed count, its base, and inside its extended basic block (a run with no entry in the middle, branches
out allowed, up to a call) what a register could serve. Only what the profile's words and counts say: a count is the word's own."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
_a = sys.argv[1:]
sys.argv = sys.argv[:1] + ['x', 'x', 'x']
from study import kind, load  # noqa: E402
def sx(v, b): return (v ^ (1 << (b - 1))) - (1 << (b - 1))
def imm_i(w): return sx(w >> 20, 12)
def imm_s(w): return sx(((w >> 25) << 5) | ((w >> 7) & 31), 12)
def imm_b(w): return sx(((w >> 31) << 12) | ((w & 0x80) << 4) | ((w >> 20) & 0x7e0) | ((w >> 7) & 0x1e), 13)
def imm_j(w): return sx(((w >> 31) << 20) | (w & 0xff000) | ((w >> 9) & 0x800) | ((w >> 20) & 0x7fe), 21)
rd = lambda w: (w >> 7) & 31
ra = lambda w: (w >> 15) & 31
rb = lambda w: (w >> 20) & 31
SAVED = {1, 8, 9} | set(range(18, 28))
for d in _a:
    pcs = {}
    for c, t, pc, w, n, l, s, nt, fl in load(d):
        e = pcs.get(pc)
        if e and e[0] == w: e[1] += n
        elif not e or n > e[1]: pcs[pc] = [w, n]
    targets = set()
    for pc, (w, n) in pcs.items():
        k = kind(w)
        if k == 'branch': targets.add(pc + imm_b(w))
        elif k == 'jal': targets.add(pc + imm_j(w))
        elif k == 'jalr': targets.add(pc + 4)
        p = pcs.get(pc - 4)
        if p and p[1] != n and not kind(p[0]) in ('branch',): targets.add(pc)   # another way in, by the counts
    tot = sum(v[1] for v in pcs.values())
    cnt = dict(load=0, store=0, l_sp=0, s_sp=0, l_gp=0, s_gp=0, fw=0, reload=0, dead=0, same_texel_prev=0, save=0, rest=0, loads_same_base=0)
    for pc in sorted(pcs):
        if pc - 4 in pcs and pc not in targets: continue
        at, seen, base_val, last_texel, pending = pc, {}, {}, None, {}
        gen = [0] * 32     # how many times a register has changed in the block: (reg, gen, off) names a slot
        while at in pcs:
            w, n = pcs[at]
            if at != pc and at in targets: break
            k = kind(w)
            if k == 'load':
                cnt['load'] += n
                if ra(w) == 2: cnt['l_sp'] += n
                if ra(w) == 3: cnt['l_gp'] += n
                key = (ra(w), gen[ra(w)], imm_i(w))
                if key in seen: cnt['fw' if seen[key] == 's' else 'reload'] += n
                seen[key] = 'l'
                if rd(w) in SAVED and ra(w) == 2: cnt['rest'] += n
                tx = (ra(w), gen[ra(w)], imm_i(w) >> 4)
                if tx == last_texel: cnt['same_texel_prev'] += n
                last_texel = tx
                if rd(w): gen[rd(w)] += 1
            elif k == 'store':
                cnt['store'] += n
                if ra(w) == 2: cnt['s_sp'] += n
                if ra(w) == 3: cnt['s_gp'] += n
                key = (ra(w), gen[ra(w)], imm_s(w))
                if key in pending: cnt['dead'] += pending[key]
                pending[key] = n
                # a store through another base may alias any slot but the stack's
                if ra(w) != 2: seen = {q: v for q, v in seen.items() if q[0] == 2}
                seen[key] = 's'
                if rb(w) in SAVED and ra(w) == 2: cnt['save'] += n
                tx = (ra(w), gen[ra(w)], imm_s(w) >> 4)
                if tx == last_texel: cnt['same_texel_prev'] += n
                last_texel = tx
            elif k in ('jal', 'jalr') or k in ('sys', 'fence', 'amo', 'other'):
                break
            elif k == 'branch':
                pending = {}
            else:
                if rd(w) and k in ('alui', 'alur', 'lui', 'auipc', 'mul', 'fp'): gen[rd(w)] += 1
            at += 4
    L, S = cnt['load'], cnt['store']
    print(f"{os.path.basename(d.rstrip('/'))}: {tot:,} instructions; loads {100 * L / tot:.1f}%, stores {100 * S / tot:.1f}%")
    print(f"   by sp: loads {100 * cnt['l_sp'] / max(L, 1):.1f}%, stores {100 * cnt['s_sp'] / max(S, 1):.1f}%; by gp: {100 * cnt['l_gp'] / max(L, 1):.1f}%, {100 * cnt['s_gp'] / max(S, 1):.1f}%")
    print(f"   sp loads into a saved register {100 * cnt['rest'] / max(L, 1):.1f}% of loads, sp stores of a saved register {100 * cnt['save'] / max(S, 1):.1f}% of stores (a function's saves and restores)")
    print(f"   in a block: a load of what was stored {100 * cnt['fw'] / max(L, 1):.1f}%, of what was loaded {100 * cnt['reload'] / max(L, 1):.1f}%; a store written over in its block {100 * cnt['dead'] / max(S, 1):.1f}%")
    print(f"   a memory op in the texel of the memory op before (one base): {100 * cnt['same_texel_prev'] / max(L + S, 1):.1f}% of memory ops")
