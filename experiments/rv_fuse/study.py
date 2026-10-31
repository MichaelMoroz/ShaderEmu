#!/usr/bin/env python3
"""What fusing RISC-V instructions could save, from pcs.txt of an RVC_PROF run (runprof.sh).

    python study.py DIR [--table N]

Every pc run, its word, its count. A pair (pc, pc + 4) runs together min(count) times unless pc is a
control instruction. Packs a block's instructions greedily with several rule sets and prints: the mix by
kind, loads and stores, and the slots (trips of the tick's loop) each rule set leaves.
"""
import collections, os, sys


def kind(w):
    o = w & 0x7f
    f3 = (w >> 12) & 7
    if o == 0x13: return 'alui'
    if o == 0x33: return 'mul' if (w >> 25) == 1 else 'alur'
    if o == 0x37: return 'lui'
    if o == 0x17: return 'auipc'
    if o == 0x03: return 'load'
    if o == 0x23: return 'store'
    if o == 0x63: return 'branch'
    if o == 0x6f: return 'jal'
    if o == 0x67: return 'jalr'
    if o == 0x07: return 'flw'
    if o == 0x27: return 'fsw'
    if o == 0x53: return 'fp'
    if o in (0x43, 0x47, 0x4b, 0x4f): return 'fmadd'
    if o == 0x2f: return 'amo'
    if o == 0x73: return 'sys'
    if o == 0x0f: return 'fence'
    return 'other'


def rd(w):
    k = kind(w)
    if k in ('store', 'branch', 'fsw', 'fence', 'other'): return 0
    if k in ('flw', 'fp', 'fmadd'):
        # to a float register, except comparisons and moves to x
        if k == 'fp' and (w >> 27) in (0x14, 0x18, 0x1c):
            return (w >> 7) & 31
        return 32 + ((w >> 7) & 31)
    return (w >> 7) & 31


def srcs(w):
    k = kind(w)
    a = (w >> 15) & 31
    b = (w >> 20) & 31
    if k in ('lui', 'auipc', 'jal'): return ()
    if k in ('alui', 'load', 'jalr', 'flw'): return (a,)
    if k == 'store': return (a, b)
    if k == 'fsw': return (a, 32 + b)
    if k in ('alur', 'mul', 'branch', 'amo'): return (a, b)
    if k == 'fp':
        top = w >> 27
        if top in (0x1a, 0x1e): return (a,)   # from an integer register
        return (32 + a, 32 + b)
    if k == 'fmadd': return (32 + a, 32 + b, 32 + (w >> 27))
    return (a, b)


CTL = ('branch', 'jal', 'jalr', 'sys', 'fence', 'amo')
ALU = ('alui', 'alur', 'lui', 'auipc')


def is_general(w):
    """Takes the general path on the tick (not the fast loop)."""
    return kind(w) in ('sys', 'fence', 'fmadd', 'other')


def load(d):
    rows = []
    with open(os.path.join(d, 'pcs.txt')) as f:
        for line in f:
            if line[0] == '#': continue
            p = line.split()
            rows.append((int(p[0]), int(p[1]), int(p[2], 16), int(p[3], 16), int(p[4]), int(p[5]), int(p[6]), int(p[7]), int(p[8])))
    return rows


# Each rule set: may the first and the second instruction share a trip?
def r_alu_alu(a, b, ka, kb):
    return ka in ALU and kb in ALU


def indep(a, b):
    d = rd(a)
    return d == 0 or d not in srcs(b)


def r_alu_any(a, b, ka, kb):
    """arithmetic, then anything but a float: the arithmetic's result passed by a select"""
    return ka in ALU and kb in ALU + ('load', 'store', 'branch')


def r_any_alu(a, b, ka, kb):
    """the fast loop's kinds, then arithmetic on the same trip"""
    return ka in ALU + ('load', 'store', 'fp', 'flw', 'fsw') and kb in ALU


def r_both(a, b, ka, kb):
    return r_alu_any(a, b, ka, kb) or r_any_alu(a, b, ka, kb)


def r_both_mem(a, b, ka, kb):
    """also two memory ops in one texel of the same base: two words at one search"""
    if r_both(a, b, ka, kb): return True
    if ka in ('load', 'store') and ka == kb and ((a >> 15) & 31) == ((b >> 15) & 31):
        return True
    return False


def pack(rows, rule, maxlen=2, need_indep=False):
    """slots after greedy pairing, the hottest pair first; returns slots by first kind"""
    by = {r[2]: r for r in rows if r[0] == 0 or True}
    return by


def study(d, table):
    rows = load(d)
    for cls, name in ((0, 'core 0'), (1, 'workers')):
        pcs = {}
        for c, t, pc, w, n, l, s, nt, fl in rows:
            if c != cls: continue
            e = pcs.get(pc)
            if e: e[1] += n
            else: pcs[pc] = [w, n]
        total = sum(v[1] for v in pcs.values())
        if not total: continue
        mix = collections.Counter()
        for w, n in pcs.values():
            mix[kind(w)] += n
        print(f'--- {name}: {total:,} instructions')
        print('  mix: ' + ', '.join(f'{k} {100 * v / total:.1f}' for k, v in mix.most_common()))

        # pairs: first kind -> second kind
        pairs = collections.Counter()
        for pc, (w, n) in pcs.items():
            k = kind(w)
            if k in ('branch', 'jal', 'jalr', 'sys'): continue
            nx = pcs.get(pc + 4)
            if not nx: continue
            pairs[(k, kind(nx[0]))] += min(n, nx[1])
        print('  pairs (% of instructions): ' + ', '.join(f'{a}+{b} {100 * v / total:.1f}' for (a, b), v in pairs.most_common(table)))

        for rname, rule in (('alu+alu', r_alu_alu), ('alu+any', r_alu_any), ('any+alu', r_any_alu), ('both', r_both), ('both+mem pair', r_both_mem)):
            for dep in (False, True):
                # greedy along the code: hottest pairs take their two instructions first
                cands = []
                for pc, (w, n) in pcs.items():
                    k = kind(w)
                    if k in CTL: continue
                    nx = pcs.get(pc + 4)
                    if not nx: continue
                    k2 = kind(nx[0])
                    if not rule(w, nx[0], k, k2): continue
                    if dep and not indep(w, nx[0]): continue
                    cands.append((min(n, nx[1]), pc))
                cands.sort(reverse=True)
                used = set()
                saved = 0
                for c, pc in cands:
                    if pc in used or pc + 4 in used: continue
                    used.add(pc); used.add(pc + 4)
                    saved += c
                print(f'  {rname:14s} {"independent" if dep else "any order  "}: {100 * saved / total:5.1f}% of instructions merged: {total / (total - saved):.3f} an instruction a trip')
        # same base memory pairs and stack traffic
        lo = sum(n for w, n in pcs.values() if kind(w) in ('load', 'flw'))
        st = sum(n for w, n in pcs.values() if kind(w) in ('store', 'fsw'))
        sp_l = sum(n for w, n in pcs.values() if kind(w) in ('load', 'flw') and ((w >> 15) & 31) == 2)
        sp_s = sum(n for w, n in pcs.values() if kind(w) in ('store', 'fsw') and ((w >> 15) & 31) == 2)
        print(f'  loads {100 * lo / total:.1f}% ({100 * sp_l / max(lo, 1):.0f}% from sp), stores {100 * st / total:.1f}% ({100 * sp_s / max(st, 1):.0f}% to sp)')
        # the budget of the taken branches
        br = sum(n for w, n in pcs.values() if kind(w) == 'branch')
        taken = 0
        for pc, (w, n) in pcs.items():
            if kind(w) == 'branch':
                nx = pcs.get(pc + 4)
                # upper bound on not taken: what ran after it
                taken += max(0, n - (nx[1] if nx else 0))
        print(f'  branches {100 * br / total:.1f}%, at least {100 * taken / max(br, 1):.0f}% of them taken')
    # stores' texels
    ns = sum(r[6] for r in rows); nt = sum(r[7] for r in rows); fl = sum(r[8] for r in rows)
    print(f'  stores {ns:,}, new texels {nt:,} ({nt / max(ns, 1):.2f} a store), full {fl:,}')


if __name__ == '__main__':
    table = 14
    args = sys.argv[1:]
    if '--table' in args:
        i = args.index('--table'); table = int(args[i + 1]); del args[i:i + 2]
    study(args[0], table)
