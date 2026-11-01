#!/usr/bin/env python3
"""RV32 words a profile saw run to wide3.h's bundles of three ops, each block list-scheduled.
    python xlate3.py SNAP PCSDIR OUT.code [--check] [--ops 3] [--mems 2]
A word moves up only over what it does not depend on. A store moves over nothing (everything before it is done in or before its bundle), memory ops keep their
order but for loads among loads, and a word that moves over an unfinished one must be one that can run again (its rd none of its sources). So at any bundle's start the RV pc in the
table, the first word not done, can be resumed: what is done later than it runs again to the same value."""
import argparse, collections, os, random, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
_argv = sys.argv
sys.argv = sys.argv[:1] + ['x', 'x', 'x']
from study import kind, load  # noqa: E402
sys.argv = _argv
HDR, W, ROW0 = 32, 2048, 64
B0, PCT, ROWS = 2304 * 2048, 5000 * 2048, 5700
K_LO, K_HI, P_LO, P_HI = 0x82000000, 0x82800000, 0x84000000, 0x88000000
SINK = 32
ap = argparse.ArgumentParser()
ap.add_argument('snap'); ap.add_argument('pcs'); ap.add_argument('out')
ap.add_argument('--floats', action='store_true', help='float words as ops (W3_SHAPE 1024)'); ap.add_argument('--hot', action='store_true', help='blocks laid out by how hot they are'); ap.add_argument('--tile', action='store_true', help='for W3_SHAPE 32'); ap.add_argument('--check', action='store_true'); ap.add_argument('--ops', type=int, default=3); ap.add_argument('--mems', type=int, default=2)
a = ap.parse_args()
raw = np.fromfile(a.snap, dtype=np.uint8)
base = HDR + ROW0 * W * 16
pcs = {}
for c, t, pc, w, n, l, s, nt, fl in load(a.pcs):
    e = pcs.get(pc)
    if e and e[0] == w: e[1] += n
    elif not e or n > e[1]: pcs[pc] = [w, n]
code, stale = {}, 0
for pc, (w, n) in pcs.items():
    if not (K_LO <= pc < K_HI or P_LO <= pc < P_HI) or pc & 3: continue
    o = base + (pc & 0x7fffffff)
    if int.from_bytes(raw[o:o + 4].tobytes(), 'little') != w:
        stale += 1
        continue
    code[pc] = (w, n)
del raw
def sx(v, b): return (v ^ (1 << (b - 1))) - (1 << (b - 1))
def imm_b(w): return sx(((w >> 31) << 12) | ((w & 0x80) << 4) | ((w >> 20) & 0x7e0) | ((w >> 7) & 0x1e), 13)
def imm_j(w): return sx(((w >> 31) << 20) | (w & 0xff000) | ((w >> 9) & 0x800) | ((w >> 20) & 0x7fe), 21)
def imm_s(w): return sx(((w >> 25) << 5) | ((w >> 7) & 31), 12)
rd_of = lambda w: (w >> 7) & 31
ra_of = lambda w: (w >> 15) & 31
rb_of = lambda w: (w >> 20) & 31
f3_of = lambda w: (w >> 12) & 7
dest = lambda r: r if r else SINK
def op_word(rd, ra, rb, fn, imm): return rd | ra << 6 | rb << 11 | fn << 16 | (imm & 0xfff) << 20
NOP = op_word(SINK, 0, 0, 0, 0)

def decode(w, pc):
    """(class, word, reads, writes, constant): class a, m (load), s (store), c (constant in Z), None for the old step"""
    k, f3 = kind(w), f3_of(w)
    rd, ra, rb = rd_of(w), ra_of(w), rb_of(w)
    if k == 'alui':
        imm = sx(w >> 20, 12)
        fn = {0: 0, 7: 2, 6: 3, 4: 4, 2: 8, 3: 9}.get(f3)
        if fn is not None: return 'a', op_word(dest(rd), ra, 0, fn, imm), {ra}, {rd}, None
        sh = (w >> 20) & 31
        if f3 == 1: return 'a', op_word(dest(rd), ra, 0, 5, sh), {ra}, {rd}, None
        return 'a', op_word(dest(rd), ra, 0, 7 if (w >> 30) & 1 else 6, sh), {ra}, {rd}, None
    if k == 'alur':
        alt = (w >> 30) & 1
        if f3 == 0: return 'a', op_word(dest(rd), ra, rb, 1 if alt else 0, 0), {ra, rb}, {rd}, None
        fn = {7: 2, 6: 3, 4: 4, 2: 8, 3: 9}.get(f3)
        if fn is not None: return 'a', op_word(dest(rd), ra, rb, fn, 0), {ra, rb}, {rd}, None
        if f3 == 1: return 'a', op_word(dest(rd), ra, rb, 5, 0), {ra, rb}, {rd}, None
        return 'a', op_word(dest(rd), ra, rb, 7 if alt else 6, 0), {ra, rb}, {rd}, None
    if k == 'mul' and f3 == 0: return 'a', op_word(dest(rd), ra, rb, 10, 0), {ra, rb}, {rd}, None
    if k == 'lui': return 'c', op_word(dest(rd), 0, 0, 15, 0), set(), {rd}, w & 0xfffff000
    if k == 'auipc': return 'c', op_word(dest(rd), 0, 0, 15, 0), set(), {rd}, ((w & 0xfffff000) + pc) & 0xffffffff
    if k == 'load' and f3 in (0, 1, 2, 4, 5): return 'm', op_word(dest(rd), ra, 0, f3, sx(w >> 20, 12)), {ra}, {rd}, None
    if k == 'store' and f3 in (0, 1, 2): return 's', op_word(SINK, ra, rb, 8 | f3, imm_s(w)), {ra, rb}, set(), None
    if a.floats:
        F = 33
        if k == 'flw' and f3 == 2 and rd < 31: return 'm', op_word(F + rd, ra, 0, 2, sx(w >> 20, 12)), {ra}, {F + rd}, None
        if k == 'fsw' and f3 == 2: return 's', op_word(SINK, ra, rb, 8 | 3, imm_s(w)), {ra, F + rb}, set(), None
        if k == 'fp' and (w >> 25) & 3 == 0:
            f5 = w >> 27
            if f5 in (0, 1, 2) and rd < 31: return 'a', op_word(F + rd, ra, rb, 11 + f5, 0), {F + ra, F + rb}, {F + rd}, None
            if f5 == 4 and f3 == 0 and ra == rb and rd < 31: return 'a', op_word(F + rd, ra, 0, 14, 0), {F + ra}, {F + rd}, None
            if f5 == 28 and f3 == 0 and rb == 0: return 'a', op_word(dest(rd), ra, 0, 14, 0), {F + ra}, {rd}, None
            if f5 == 30 and f3 == 0 and rb == 0 and rd < 31: return 'a', op_word(F + rd, ra, 0, 0, 0), {ra}, {F + rd}, None
    return None, None, None, None, None

starts = set()
for pc, (w, n) in code.items():
    k = kind(w)
    if k == 'branch': starts |= {pc + imm_b(w), pc + 4}
    elif k == 'jal': starts |= {pc + imm_j(w), pc + 4}
    elif k in ('jalr', 'sys', 'fence', 'other', 'fmadd', 'amo'): starts.add(pc + 4)
    p = code.get(pc - 4)
    if not p or p[1] != n: starts.add(pc)
    if k == 'auipc' and pc + 4 in code and kind(code[pc + 4][0]) == 'jalr' and ra_of(code[pc + 4][0]) == rd_of(w):
        starts.add((pc + (w & 0xfffff000) + sx(code[pc + 4][0] >> 20, 12)) & 0xffffffff)

bundles, start_of = [], {}
def empty(rv): return dict(ops=[], mems=0, st=False, const=None, n=0, ctl=0, f3=0, cra=0, crb=0, target=None, off=0, slow=0, X=None, Z=None, rv=rv, w=0)
stats = collections.Counter()
def block(at):
    """the block from at: bundles appended, returns the next pc to go on from in order, or None"""
    first = at
    placed = []
    words = []       # (pc, placed index)
    ready, read_at, read_bar = {}, collections.Counter(), collections.Counter()
    mem_at = -1
    maxb = -1
    n0 = code[at][1]
    stop = False
    nx = None
    while at in code and not stop:
        if at != first and at in starts:
            nx = at
            break
        w, _ = code[at]
        k = kind(w)
        cl, op, u, df, cst = decode(w, at)
        # a jalr by what lui or auipc just made goes by a known place
        known = None
        if k == 'jalr' and at != first and at - 4 in code and kind(code[at - 4][0]) in ('lui', 'auipc') and rd_of(code[at - 4][0]) == ra_of(w) and ra_of(w):
            pw = code[at - 4][0]
            known = (((pw & 0xfffff000) + (at - 4 if kind(pw) == 'auipc' else 0)) + sx(w >> 20, 12)) & 0xfffffffe
        here = at
        at += 4
        if k in ('lui', 'auipc') and at in code and at not in starts:
            w2 = code[at][0]
            if kind(w2) == 'alui' and f3_of(w2) == 0 and rd_of(w2) == rd_of(w) and ra_of(w2) == rd_of(w) and rd_of(w):
                cst = (cst + sx(w2 >> 20, 12)) & 0xffffffff
                words.append((at, None))
                at += 4
        if cl is None:
            if k in ('branch', 'jal') or (k == 'jalr' and (rd_of(w) == 0 or known is not None)):
                pass
            else:                 # the old step, a bundle of its own
                if placed and not placed[-1]['ops'] and not placed[-1]['ctl']: placed.pop()
                B = empty(here)
                if k in ('sys', 'fence', 'other', 'fmadd'):   # (the old step does not take these: left at it)
                    B.update(slow=5)
                else:
                    B.update(slow=4, X=w, Z=here, n=1, ctl=3 if k == 'jalr' else 0)
                placed.append(B)
                words.append((here, len(placed) - 1))
                stats['old'] += n0
                maxb = mem_at = len(placed) - 1
                for r in range(65): ready[r] = maxb + 1
                if k == 'jalr':
                    nx = None
                    stop = True
                elif k in ('sys', 'fence', 'other', 'fmadd', 'amo'):
                    nx = at if at in code else None
                    stop = True
                placed.append(empty(at))
                continue
        if k in ('branch', 'jal', 'jalr'):
            lk = k != 'branch' and rd_of(w) != 0
            if lk: # jal rd: the link as a constant
                if not placed or placed[-1]['slow'] or placed[-1]['ops']:
                    placed.append(empty(here))
                B = placed[-1]
                B['ops'].append(op_word(dest(rd_of(w)), 0, 0, 15, 0))
                B['const'] = (here + 4) & 0xffffffff
            if not placed or placed[-1]['slow']: placed.append(empty(here))
            B = placed[-1]
            B['n'] += 1
            if k == 'branch': B.update(ctl=1, f3=f3_of(w), cra=ra_of(w), crb=rb_of(w), target=(here + imm_b(w)) & 0xffffffff)
            elif k == 'jal': B.update(ctl=2, target=(here + imm_j(w)) & 0xffffffff)
            elif known is not None: B.update(ctl=2, target=known)
            else: B.update(ctl=3, cra=ra_of(w), off=sx(w >> 20, 12))
            nx = at if at in code else None
            if k != 'branch': nx = None
            stop = True
            words.append((here, len(placed) - 1))
            break
        u = u - {0}; df = df - {0}
        mem = cl in ('m', 's')
        e = 0
        if mem:
            for r in u: e = max(e, ready.get(r, -1) + 1)
            e = max(e, mem_at if cl == 's' else (mem_at if mem_at >= 0 else 0))
        else:
            for r in u: e = max(e, ready.get(r, 0))
        # a word done early is done again from a restart: nothing done before the last unfinished word of those before its reader may overwrite what that reader reads
        for r in df: e = max(e, ready.get(r, 0), read_at[r], read_bar[r])
        again = not (df & u)
        if cl == 's' or not again: e = max(e, maxb)
        if cl == 's':
            e = max(e, max((i for i, B in enumerate(placed) if B['mems']), default=0))
        b = e
        while True:
            if b >= len(placed): placed.append(empty(here))
            B = placed[b]
            ok = not B['slow'] and not B['ctl']
            if ok:
                if cl == 'c': ok = not B['ops']        # it is X, before all others
                elif mem: ok = B['mems'] < a.mems and B['mems'] == len(B['ops']) and not (cl == 's' and B['st']) and B['const'] is None
                else: ok = len(B['ops']) < (2 if B['const'] is not None else a.ops)
            if ok: break
            b += 1
        B = placed[b]
        if cl == 'c': B['const'] = cst
        B['ops'].append(op)
        if mem: B['mems'] += 1
        B['st'] = B['st'] or cl == 's'
        B['n'] += 1 + (1 if words and words[-1][1] is None else 0)
        for r in df: ready[r] = b
        for r in u: read_at[r] = max(read_at[r], b)
        if cl == 's': mem_at = b
        maxb = max(maxb, b)
        for r in u: read_bar[r] = max(read_bar[r], maxb)
        words.append((here, b))
    kept = [i for i, B in enumerate(placed) if B['ops'] or B['ctl'] or B['slow']]
    now = {o: n for n, o in enumerate(kept)}
    words = [(p, None if q is None else now[q]) for p, q in words]
    placed = [placed[i] for i in kept]
    return placed, sorted(words, key=lambda t: t[0]), nx, n0, first

order = sorted(code)
if a.hot: order = sorted(code, key=lambda p: (-code[p][1], p))
for pc in order:
    if pc in start_of or (pc not in starts and pc - 4 in code): continue
    at = pc
    while at in code and at not in start_of:
        placed, words, nx, n0, first = block(at)
        start_of[at] = len(bundles)
        idx0 = len(bundles)
        # the restart of each bundle: the first word (in order) not placed in an earlier bundle
        ind = {id(B): i for i, B in enumerate(placed)}
        seq = [p for p, q in words]
        for i, B in enumerate(placed):
            B['w'] = n0
            if not B['slow']:
                B['rv'] = next((p for p, q in words if q is not None and q >= i), B['rv'])
            bundles.append(B)
        L = placed[-1] if placed else None
        if nx is None and L is not None and L['ctl'] in (0, 1) and L['slow'] != 5:
            # falls off what is known: a leaving bundle, at the pc after the last word
            lp = (words[-1][0] + 4) if words else at
            bundles.append(dict(empty(lp), slow=5))
        if L is None:
            bundles.append(dict(empty(at), slow=5)); break
        if nx is None: break
        if nx in start_of or (a.hot and code[nx][1] * 8 < n0):   # (runs next to another: a leaving-free way on is a far jump)
            bundles.append(dict(empty(nx), ctl=2, target=nx))
            break
        at = nx
leave = {}
def leave_bundle(t):
    if t not in leave:
        leave[t] = len(bundles)
        bundles.append(dict(empty(t), slow=5))
    return leave[t]
for B in list(bundles):
    if B['target'] is not None and 'tb' not in B:
        B['tb'] = start_of[B['target']] if B['target'] in start_of else leave_bundle(B['target'])
# a branch too far goes by an island: a far jump among bundles nothing falls into
order_b = list(bundles)
at_of = {id(B): i for i, B in enumerate(order_b)}
gaps = [i for i, B in enumerate(order_b) if B['slow'] == 5 or (not B['slow'] and B['ctl'] in (2, 3))]
for B in order_b:
    if B['target'] is not None: B['tbo'] = order_b[B['tb']]
import bisect
isl = collections.defaultdict(dict)
for i, B in enumerate(order_b):
    if B['ctl'] == 1 and not -900 <= B['tb'] - i < 900:
        g = bisect.bisect_left(gaps, i)
        near = [gaps[j] for j in (g - 1, g) if 0 <= j < len(gaps) and abs(gaps[j] - i) < 800]
        if near:
            j = min(near, key=lambda q: abs(q - i))
            I = isl[j].get(B['target'])
            if I is None:
                I = dict(empty(B['target']), ctl=2, target=B['target'], tb=0, tbo=B['tbo'], island=True)
                isl[j][B['target']] = I
            B['tbo'] = I
if isl:
    nb = []
    for i, B in enumerate(order_b):
        nb.append(B)
        nb.extend(isl[i].values() if i in isl else ())
    start_obj = {pc: order_b[b] for pc, b in start_of.items()}
    bundles = nb
    at_of = {id(B): i for i, B in enumerate(bundles)}
    start_of = {pc: at_of[id(B)] for pc, B in start_obj.items()}
    for B in bundles:
        if B['target'] is not None: B['tb'] = at_of[id(B['tbo'])]
print(f"{sum(len(v) for v in isl.values())} islands")
area = np.zeros((ROWS * 2048, 4), dtype=np.uint32)
far = far_w = 0
for idx, B in enumerate(bundles):
    c = B0 + idx
    if B['slow'] == 5:
        area[c] = (NOP, NOP, 0, 5)
    elif B['slow'] == 4:
        area[c] = (B['X'], NOP, B['Z'], 4 | 1 << 3 | B['ctl'] << 6)
    else:
        ops = B['ops'] + [NOP] * 3
        ctl = (1 if B['mems'] >= 1 else 0) | (2 if B['mems'] >= 2 else 0) | min(B['n'], 7) << 3
        if B['const'] is not None:
            # the constant's op first, then the others in order, reading what it made
            cop = [o for o in B['ops'] if (o >> 16) & 15 == 15][0]
            rest = [o for o in B['ops'] if o != cop] + [NOP]
            X, Y, Z = cop, rest[0], B['const']
        else:
            X, Y, Z = ops[0], ops[1], ops[2]
        if B['ctl'] == 1:
            off = B['tb'] - idx
            if -1024 <= off < 1024:
                ctl |= 1 << 6 | B['f3'] << 8 | B['cra'] << 11 | B['crb'] << 16 | (off & 0x7ff) << 21
            else:
                far += 1; far_w += B['w']
                area[c] = (NOP, NOP, 0, 5)
                area[PCT + (idx >> 2), idx & 3] = B['rv'] & 0xffffffff
                continue
        elif B['ctl'] == 2:
            off = B['tb'] - idx
            if -1024 <= off < 1024:
                ctl |= 1 << 6 | (off & 0x7ff) << 21        # beq x0, x0
            else:
                ctl |= 2 << 6 | (off & 0xffffff) << 8
        elif B['ctl'] == 3:
            if not -1024 <= B['off'] < 1024:
                area[c] = (NOP, NOP, 0, 5)
                area[PCT + (idx >> 2), idx & 3] = B['rv'] & 0xffffffff
                continue
            ctl |= 3 << 6 | B['cra'] << 11 | (B['off'] & 0x7ff) << 21
        area[c] = (X, Y, Z, ctl)
    area[PCT + (idx >> 2), idx & 3] = B['rv'] & 0xffffffff
plain = area[B0:B0 + len(bundles)].copy()
if a.tile:
    # the bundles' place in the texture by tiles
    n = len(bundles)
    src = area[B0:B0 + n].copy()
    area[B0:B0 + n] = 0
    i_ = np.arange(n)
    x_ = (i_ & 15) | ((i_ >> 4) & 0x7f0)
    y_ = 2304 + (((i_ >> 4) & 15) | ((i_ >> 11) & 0xf0))
    area[y_ * 2048 + x_] = src
for pc, b in start_of.items():
    t = (pc - K_LO) >> 4 if K_LO <= pc < K_HI else 256 * 2048 + ((pc - P_LO) >> 4)
    area[t, (pc >> 2) & 3] = B0 + b
area.reshape(ROWS, 2048, 4)[::-1].tofile(a.out)
chk_area = area
tot = sum(B['w'] for B in bundles)
rvs = sum(B['w'] * B['n'] for B in bundles)
print(f"{len(bundles):,} bundles, {len(code):,} words ({stale:,} stale); RV a bundle {rvs / tot:.3f}; old step {100 * sum(B['w'] for B in bundles if B['slow'] == 4) / tot:.1f}% of bundles; two mems {100 * sum(B['w'] for B in bundles if B['mems'] == 2) / tot:.1f}%; one mem {100 * sum(B['w'] for B in bundles if B['mems'] == 1) / tot:.1f}%; far {far} ({100 * far_w / max(tot, 1):.2f}%)")
assert B0 + len(bundles) < PCT

if a.check:
    # every block run from random registers on both forms: registers, stores and where it goes the same, and from each bundle's restart too
    M = 0xffffffff
    def sg(v): return v - (1 << 32) if v & 0x80000000 else v
    def mem(ad):
        x = (ad & ~3) * 2654435761 & M
        return x ^ (x >> 13)
    def lv(va, f3):
        m = mem(va) >> ((va & 3) * 8)
        return {0: ((m & 0xff) ^ 0x80) - 0x80 & M, 1: ((m & 0xffff) ^ 0x8000) - 0x8000 & M, 2: m & M, 4: m & 0xff, 5: m & 0xffff}[f3]
    import struct
    def fl(x): return struct.unpack('<f', struct.pack('<I', x))[0]
    def uf(x): return struct.unpack('<I', struct.pack('<f', x))[0]
    def fop(fn, x, y):
        v = np.float32(fl(x)); u = np.float32(fl(y))
        return uf({11: v + u, 12: v - u, 13: v * u}[fn])
    def ok_word(w):
        k = kind(w)
        if k in ('alui', 'alur', 'lui', 'auipc', 'load', 'store', 'branch', 'jal', 'jalr'): return True
        if k == 'mul': return f3_of(w) == 0
        if a.floats and k in ('flw', 'fsw', 'fp'): return decode(w, 0)[0] is not None
        return False
    def rv_one(w, pc, r, st):
        k, f3 = kind(w), f3_of(w)
        A, Bv, d = r[ra_of(w)], r[rb_of(w)], rd_of(w)
        nx, v = pc + 4, None
        if k == 'flw': r[33 + d] = lv((A + sx(w >> 20, 12)) & M, 2); return nx & M
        if k == 'fsw': st.append(((A + imm_s(w)) & M, r[33 + rb_of(w)], 2)); return nx & M
        if k == 'fp':
            f5, fa, fb = w >> 27, r[33 + ra_of(w)], r[33 + rb_of(w)]
            if f5 in (0, 1, 2): r[33 + d] = fop(11 + f5, fa, fb)
            elif f5 == 4: r[33 + d] = fa
            elif f5 == 28:
                if d: r[d] = fa
            else: r[33 + d] = A
            return nx & M
        if k in ('alui', 'alur', 'mul'):
            y = sx(w >> 20, 12) & M if k == 'alui' else Bv
            sh = (w >> 20) & 31 if k == 'alui' else Bv & 31
            alt = (w >> 30) & 1
            if k == 'mul': v = A * Bv & M
            elif f3 == 0: v = (A - y if k == 'alur' and alt else A + y) & M
            elif f3 == 1: v = A << sh & M
            elif f3 == 2: v = int(sg(A) < sg(y))
            elif f3 == 3: v = int(A < y)
            elif f3 == 4: v = A ^ y
            elif f3 == 5: v = (sg(A) >> sh) & M if alt else A >> sh
            elif f3 == 6: v = A | y
            else: v = A & y
        elif k == 'lui': v = w & 0xfffff000
        elif k == 'auipc': v = ((w & 0xfffff000) + pc) & M
        elif k == 'load': v = lv((A + sx(w >> 20, 12)) & M, f3)
        elif k == 'store': st.append(((A + imm_s(w)) & M, Bv, f3))
        elif k == 'branch':
            lt = A < Bv if f3 & 2 else sg(A) < sg(Bv)
            if (lt if f3 & 4 else A == Bv) != bool(f3 & 1): nx = pc + imm_b(w)
        elif k == 'jal': v, nx = pc + 4, pc + imm_j(w)
        elif k == 'jalr': v, nx = pc + 4, (A + sx(w >> 20, 12)) & 0xfffffffe
        else: raise KeyError(k)
        if v is not None and d: r[d] = v
        return nx & M
    def alu(op, r):
        f, A = (op >> 16) & 15, r[(op >> 6) & 31]
        if 11 <= f <= 13: return fop(f, r[33 + ((op >> 6) & 31)], r[33 + ((op >> 11) & 31)])
        if f == 14: return r[33 + ((op >> 6) & 31)]
        y = (r[(op >> 11) & 31] + sx(op >> 20, 12)) & M
        sh = y & 31
        return {0: A + y, 1: A - y, 2: A & y, 3: A | y, 4: A ^ y, 5: A << sh, 6: A >> sh, 7: sg(A) >> sh, 8: int(sg(A) < sg(y)), 9: int(A < y), 10: A * y}[f] & M
    def wr(r, op, v):
        if op & 63 != SINK and op & 63: r[op & 63] = v & M
    def bundle_one(idx, r, st):
        X, Y, Z, Wd = (int(q) for q in plain[idx])
        for slot, op in ((0, X), (1, Y)):
            if Wd & (1 << slot):
                va = (r[(op >> 6) & 31] + sx(op >> 20, 12)) & M
                if op & 0x80000: pass
        # memory ops read the registers as the bundle found them
        memr = list(r)
        for slot, op in ((0, X), (1, Y)):
            if Wd & (1 << slot):
                va = (memr[(op >> 6) & 31] + sx(op >> 20, 12)) & M
                f3 = (op >> 16) & 7
                if op & 0x80000: st.append((va, memr[((op >> 11) & 31) + (33 if f3 == 3 else 0)], 2 if f3 == 3 else f3))
                else: wr(r, op, lv(va, f3))
            elif slot == 0:
                wr(r, op, Z if (op >> 16) & 15 == 15 else alu(op, r))
            else:
                wr(r, op, alu(op, r))
        if (X >> 16) & 15 != 15 or Wd & 1: wr(r, Z, alu(Z, r))
        kc = Wd & 0xc0
        if kc == 0x40:
            ca, cb, f = r[(Wd >> 11) & 31], r[(Wd >> 16) & 31], (Wd >> 8) & 7
            lt = ca < cb if f & 2 else sg(ca) < sg(cb)
            if (lt if f & 4 else ca == cb) != bool(f & 1): return ('b', idx + sx(Wd >> 21, 11))
        elif kc == 0x80: return ('b', idx + sx(Wd >> 8, 24))
        elif kc == 0xc0: return ('p', (r[(Wd >> 11) & 31] + sx(Wd >> 21, 11)) & 0xfffffffe)
        return ('b', idx + 1)
    random.seed(1)
    bad = collections.Counter()
    shown = 0
    entries = [(pc, b) for pc, b in start_of.items()]
    random.shuffle(entries)
    for pc, b0 in entries[:40000]:
        for trial in range(3):
            r0 = [0] + [random.choice((random.getrandbits(32), random.getrandbits(5), 0x82000000 + random.getrandbits(12) * 4, M, 0, 1)) for _ in range(31)] + [0] + [uf(np.float32(random.choice((1.5, -2.25, 1000.125, 0.0078125, 3.0)))) for _ in range(32)]
            r2, st2 = list(r0), []
            idx = b0
            steps = 0
            snapshots = []
            how = None
            while steps < 60:
                Wd = int(plain[idx][3])
                if Wd & 4: how = ('l', idx); break
                snapshots.append((idx, list(r2), len(st2)))
                how = bundle_one(idx, r2, st2)
                steps += 1
                if how[0] == 'p' or (how[0] == 'b' and (bundles[how[1]]['rv'] in start_of and start_of[bundles[how[1]]['rv']] == how[1])): break
                idx = how[1]
            else:
                continue
            if how[0] == 'l':
                endpc = bundles[how[1]]['rv']
            else:
                endpc = how[1] if how[0] == 'p' else bundles[how[1]]['rv']
            def on(p, r, st):
                for _ in range(16):
                    w = code[p][0] if p in code else None
                    if w is None or not ok_word(w): break
                    p = rv_one(w, p, r, st)
            def run(from_pc, r, st):
                p, guard = from_pc, 0
                while guard < 200:
                    w = code[p][0] if p in code else None
                    if w is None: return None
                    if not ok_word(w): return None
                    p = rv_one(w, p, r, st); guard += 1
                    if p == endpc:
                        on(p, r, st)      # on over the same words: what was done ahead of the place left comes level
                        return p
                return None
            r1, st1 = list(r0), []
            if endpc == pc and steps == 0: on(pc, r1, st1)
            elif run(pc, r1, st1) is None: continue
            on(endpc, r2, st2)
            why = 'registers' if r1 != r2 else 'stores' if st1 != st2 else None
            # from a bundle's restart: the RV words from there on the state then must come to the same end
            if not why:
                for (i2, rs, ns) in snapshots[1:]:
                    rr, sr = list(rs), []
                    if bundles[i2]['rv'] == endpc: on(endpc, rr, sr)
                    elif run(bundles[i2]['rv'], rr, sr) is None: continue
                    if rr != r1 or st1[ns:] != sr:
                        if shown < 6: print('  restart at bundle', i2 - b0, 'of', len(snapshots), hex(bundles[i2]['rv']), [(i, hex(x), hex(y)) for i, (x, y) in enumerate(zip(rr, r1)) if x != y], st1[ns:][:4], sr[:4], [[hex(int(q)) for q in plain[j]] for j in range(b0, i2 + 1)])
                        why = 'restart'; break
            if why:
                bad[why] += 1
                if shown < 6:
                    shown += 1
                    print(why, hex(pc), 'words', [hex(code[pc + 4 * q][0]) for q in range(9) if pc + 4 * q in code], 'to', hex(endpc), [(i, hex(x), hex(y)) for i, (x, y) in enumerate(zip(r1, r2)) if x != y], st1[:4], st2[:4], [[hex(int(q)) for q in plain[j]] for j in range(b0, b0 + 4)])
                break
    print('checked: ' + (', '.join(f'{v} {k}' for k, v in bad.items()) or 'none differ'))
