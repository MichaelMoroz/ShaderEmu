#!/usr/bin/env python3
"""packing.py's ceilings without its cap of no reordering: python pack2.py PCSDIR [...]
Each basic block's words (a run with no way in, up to control) are list-scheduled into bundles, by a register's write and reads and by memory's
order (a store keeps its place among loads and stores; loads may pass one another only within --loads-free; a float or a slow word keeps its place in a bundle
of its own). A shape is (ops, of them memory, of them may be a shift/logic unit op, long immediates). One control a bundle, last, reading results of the bundle's ops.
RV a bundle by the profile's counts, and the same with the block's fall-through branches inside a bundle run (superblocks: a not-taken branch ends no bundle's fetch but is one control)."""
import os, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
_a = sys.argv[1:]
sys.argv = sys.argv[:1] + ['x', 'x', 'x']
from study import kind, load  # noqa: E402
def sx(v, b): return (v ^ (1 << (b - 1))) - (1 << (b - 1))
def imm_b(w): return sx(((w >> 31) << 12) | ((w & 0x80) << 4) | ((w >> 20) & 0x7e0) | ((w >> 7) & 0x1e), 13)
def imm_j(w): return sx(((w >> 31) << 20) | (w & 0xff000) | ((w >> 9) & 0x800) | ((w >> 20) & 0x7fe), 21)
rd = lambda w: (w >> 7) & 31
ra = lambda w: (w >> 15) & 31
rb = lambda w: (w >> 20) & 31
def use_def(w):
    k = kind(w)
    if k in ('alui', 'load', 'jalr', 'flw'): return {ra(w)} - {0}, {rd(w)} - {0}
    if k in ('alur', 'mul'): return {ra(w), rb(w)} - {0}, {rd(w)} - {0}
    if k in ('store',): return {ra(w), rb(w)} - {0}, set()
    if k == 'fsw': return {ra(w)} - {0}, set()
    if k in ('lui', 'auipc', 'jal'): return set(), {rd(w)} - {0}
    if k == 'branch': return {ra(w), rb(w)} - {0}, set()
    return None, None
SHAPES = [('2 ops, 1 mem', 2, 1, 1, 0), ('2 ops, 2 mem', 2, 2, 1, 0), ('3 ops, 1 mem', 3, 1, 1, 0), ('3 ops, 2 mem', 3, 2, 1, 0), ('4 ops, 2 mem', 4, 2, 2, 0), ('4 ops, 4 mem', 4, 4, 2, 0), ('6 ops, 2 mem', 6, 2, 3, 0), ('8 ops, 4 mem', 8, 4, 4, 0), ('3,1 mem first', 3, 1, 1, 1), ('3,2 mem first', 3, 2, 1, 1), ('4,2 mem first', 4, 2, 2, 1)]
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
        if p and p[1] != n and kind(p[0]) != 'branch': targets.add(pc)
    # blocks: [(weight, [words])]
    blocks = []
    for pc in sorted(pcs):
        if pc - 4 in pcs and pc not in targets and kind(pcs[pc - 4][0]) not in ('branch', 'jal', 'jalr'): continue
        at, ws = pc, []
        while at in pcs:
            if at != pc and at in targets: break
            ws.append(pcs[at][0])
            at += 4
            if kind(ws[-1]) in ('branch', 'jal', 'jalr'): break
        blocks.append((pcs[pc][1], ws))
    tot = sum(a * len(b) for a, b in blocks)
    print(f"{os.path.basename(d.rstrip('/'))}: {len(blocks):,} blocks, {tot / sum(a for a, b in blocks):.2f} RV a block by the counts")
    for name, W, M, L, MF in SHAPES:
        bundles = 0
        rv = 0
        for wt, ws in blocks:
            placed = []              # bundles: [ops, mems, longs]
            ready_w = {}             # register -> the bundle that last wrote it
            read_at = collections.defaultdict(int)
            mem_at = -1
            for w in ws:
                k = kind(w)
                u, df = use_def(w)
                ctrl = k in ('branch', 'jal', 'jalr')
                if u is None:        # slow or float: a bundle of its own, an order point
                    b = len(placed)
                    placed.append([W, M, L, 1])
                    mem_at = b
                    for r in range(32): ready_w[r] = b
                    continue
                e = 0
                for r in u: e = max(e, ready_w.get(r, 0))       # may read the result in its bundle (ops in order)
                for r in df: e = max(e, ready_w.get(r, 0), read_at[r])
                mem = k in ('load', 'store')
                if k == 'store': e = max(e, mem_at)
                if k == 'load': e = max(e, mem_at)
                if ctrl:             # control goes in the last bundle, and an op that is control has no slot
                    b = max(e, len(placed) - 1, 0)
                    if not placed: placed.append([0, 0, 0, 0])
                    if b >= len(placed): placed.append([0, 0, 0, 0])
                    if df:           # a link: an op too
                        if placed[b][0] >= W: b += 1; placed.append([0, 0, 0, 0])
                        placed[b][0] += 1
                    continue
                lng = 1 if k in ('lui', 'auipc') else 0
                if MF and mem:       # mem-first: a memory op reads nothing made in its bundle, and comes before the bundle's other ops
                    e = 0
                    for r in u: e = max(e, ready_w.get(r, -1) + 1)
                    for r in df: e = max(e, ready_w.get(r, 0), read_at[r])
                    e = max(e, mem_at)
                b = e
                while True:
                    if b >= len(placed): placed.append([0, 0, 0, 0])
                    p = placed[b]
                    if p[0] < W and (not mem or p[1] < M) and (not lng or p[2] < L) and (not (MF and mem) or (p[0] == p[1] and (k != 'store' or p[3] == 0))): break
                    b += 1
                placed[b][0] += 1
                placed[b][1] += 1 if mem else 0
                placed[b][2] += lng
                placed[b][3] += 1 if k == 'store' else 0
                for r in df: ready_w[r] = b
                for r in u: read_at[r] = max(read_at[r], b)
                if k == 'store': mem_at = b
            bundles += wt * max(len(placed), 1)
            rv += wt * len(ws)
        print(f'   {name:18s} {rv / bundles:.2f} RV a bundle')
