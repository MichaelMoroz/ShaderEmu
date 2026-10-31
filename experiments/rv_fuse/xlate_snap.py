#!/usr/bin/env python3
"""RV32 words a snapshot's profile saw run (kernel, programs) to wide.h's bundles: the code area the harness puts under the ROM.

    python xlate_snap.py SNAP PCSDIR OUT.code [--no-fold]

Registers, loads and stores stay RV32's. What is not translated (a word never seen, a system instruction) is left to the old loop.
"""
import argparse, collections, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from study import kind, load  # noqa: E402

HDR, W, ROW0 = 32, 2048, 64
B0 = 2304 * 2048
PCT = 5000 * 2048
ROWS = 5700
SINK = 32
K_LO, K_HI, P_LO, P_HI = 0x82000000, 0x82800000, 0x84000000, 0x88000000
P_SUM, P_ALU, P_LOAD, P_STORE, P_RV, P_LEAVE = range(6)
ap = argparse.ArgumentParser()
ap.add_argument('snap')
ap.add_argument('pcs')
ap.add_argument('out')
ap.add_argument('--no-fold', action='store_true')
a = ap.parse_args()

raw = np.fromfile(a.snap, dtype=np.uint8)
base = HDR + ROW0 * W * 16
pcs = {}
for c, t, pc, w, n, l, s, nt, fl in load(a.pcs):
    e = pcs.get(pc)
    if e and e[0] == w: e[1] += n
    elif not e or n > e[1]: pcs[pc] = [w, n]
code = {}
stale = 0
for pc, (w, n) in pcs.items():
    if not (K_LO <= pc < K_HI or P_LO <= pc < P_HI) or pc & 3: continue
    o = base + (pc & 0x7fffffff)
    if int.from_bytes(raw[o:o + 4].tobytes(), 'little') != w:
        stale += 1
        continue
    code[pc] = (w, n)
del raw


def sx(v, bits):
    return (v ^ (1 << (bits - 1))) - (1 << (bits - 1))


def imm_b(w):
    return sx(((w >> 31) << 12) | ((w & 0x80) << 4) | ((w >> 20) & 0x7e0) | ((w >> 7) & 0x1e), 13)


def imm_j(w):
    return sx(((w >> 31) << 20) | (w & 0xff000) | ((w >> 9) & 0x800) | ((w >> 20) & 0x7fe), 21)


def imm_s(w):
    return sx(((w >> 25) << 5) | ((w >> 7) & 31), 12)


rd_of = lambda w: (w >> 7) & 31
ra_of = lambda w: (w >> 15) & 31
rb_of = lambda w: (w >> 20) & 31
f3_of = lambda w: (w >> 12) & 7
dest = lambda r: r if r else SINK


# Where a bundle must start: a target, after any control, or where the count says something else came in.
starts = set()
for pc, (w, n) in code.items():
    k = kind(w)
    if k == 'branch': starts |= {pc + imm_b(w), pc + 4}
    elif k == 'jal': starts |= {pc + imm_j(w), pc + 4}
    elif k in ('jalr', 'sys', 'fence', 'other', 'fmadd', 'amo'): starts.add(pc + 4)
    p = code.get(pc - 4)
    if not p or p[1] != n: starts.add(pc)
    # a far call or jump made of auipc and jalr is a target too
    if k == 'auipc' and pc + 4 in code and kind(code[pc + 4][0]) == 'jalr' and ra_of(code[pc + 4][0]) == rd_of(w):
        starts.add(pc + (w & 0xfffff000) + sx(code[pc + 4][0] >> 20, 12))



def alu_op(w):
    """(path, rd, ra, rb, fn, imm) or None: the sum or the unit (wide.h's numbers: and 0+2, or 1+2 ...)"""
    k, f3 = kind(w), f3_of(w)
    rd, ra = dest(rd_of(w)), ra_of(w)
    if k == 'alui':
        imm = sx(w >> 20, 12)
        if f3 == 0: return P_SUM, rd, ra, 0, 0, imm
        fn = {7: 2, 6: 3, 4: 4, 2: 8, 3: 9}.get(f3)
        if fn is not None: return P_ALU, rd, ra, 0, fn, imm
        sh = (w >> 20) & 31
        if f3 == 1: return P_ALU, rd, ra, 0, 5, sh
        return P_ALU, rd, ra, 0, 7 if (w >> 30) & 1 else 6, sh
    if k == 'alur':
        rb = rb_of(w)
        alt = (w >> 30) & 1
        if f3 == 0: return (P_ALU, rd, ra, rb, 1, 0) if alt else (P_SUM, rd, ra, rb, 0, 0)
        fn = {7: 2, 6: 3, 4: 4, 2: 8, 3: 9}.get(f3)
        if fn is not None: return P_ALU, rd, ra, rb, fn, 0
        if f3 == 1: return P_ALU, rd, ra, rb, 5, 0
        return P_ALU, rd, ra, rb, 7 if alt else 6, 0
    return None


def lite(w):
    """Op 1: a sum whose immediate fits twelve bits"""
    k = kind(w)
    if not rd_of(w): return None
    if k == 'alui' and f3_of(w) == 0: return dest(rd_of(w)) | ra_of(w) << 6 | (w >> 20) << 20
    if k == 'alur' and f3_of(w) == 0 and not (w >> 30) & 1: return dest(rd_of(w)) | ra_of(w) << 6 | rb_of(w) << 11
    return None


Y_NONE = SINK
bundles = []    # [X, Y, Z, W-without-offset, rvpc, target rv pc or None]
start_of = {}
exits = {}


def x_word(rd, ra, rb, fn=0):
    return rd | ra << 6 | rb << 11 | fn << 16


stats = collections.Counter()
weight = collections.Counter()
for pc in sorted(code):
    if pc in start_of or (pc not in starts and pc - 4 in code): continue
    # a run from here, while the words are known
    at = pc
    while at in code:
        w, n0 = code[at]
        k = kind(w)
        start_of[at] = len(bundles)
        B = dict(X=x_word(SINK, 0, 0), Y=Y_NONE, Z=0, path=P_SUM, n=0, ctl=0, f3=0, cra=0, crb=0, target=None, rv=at, off=0)
        i = at
        have_op0 = False
        # op 0
        op = alu_op(w)
        nxt = code.get(i + 4)
        if k in ('lui', 'auipc'):
            val = (w & 0xfffff000) + (i if k == 'auipc' else 0) & 0xffffffff
            rd = rd_of(w)
            B.update(X=x_word(dest(rd), 0, 0), Z=val, path=P_SUM, n=1)
            have_op0 = True
            i += 4
            if not a.no_fold and nxt and i not in starts and rd:
                w2 = nxt[0]
                if kind(w2) == 'alui' and f3_of(w2) == 0 and rd_of(w2) == rd and ra_of(w2) == rd:
                    B.update(Z=(val + sx(w2 >> 20, 12)) & 0xffffffff, n=2)
                    i += 4
                elif kind(w2) == 'load' and rd_of(w2) == rd and ra_of(w2) == rd:
                    B.update(X=x_word(dest(rd), 0, 0, f3_of(w2)), Z=(val + sx(w2 >> 20, 12)) & 0xffffffff, path=P_LOAD, n=2)
                    i += 4
                elif kind(w2) == 'jalr' and ra_of(w2) == rd and rd_of(w2) in (0, 1, rd):
                    # a far call or jump: the link as the op, the jump as control (when it is translated)
                    tgt = (val + sx(w2 >> 20, 12)) & 0xffffffff
                    if rd_of(w2) == rd:
                        B.update(Z=i + 4, n=2, ctl=2, target=tgt)
                        i += 4
        elif op:
            path, rd, ra, rb, fn, imm = op
            B.update(X=x_word(rd, ra, rb, fn), Z=imm & 0xffffffff, path=path, n=1)
            have_op0 = True
            i += 4
        elif k == 'load' and f3_of(w) in (0, 1, 2, 4, 5):
            B.update(X=x_word(dest(rd_of(w)), ra_of(w), 0, f3_of(w)), Z=sx(w >> 20, 12) & 0xffffffff, path=P_LOAD, n=1)
            have_op0 = True
            i += 4
        elif k == 'store' and f3_of(w) in (0, 1, 2):
            B.update(X=x_word(SINK, ra_of(w), rb_of(w), f3_of(w)), Z=imm_s(w) & 0xffffffff, path=P_STORE, n=1)
            have_op0 = True
            i += 4
        elif k in ('mul', 'fp', 'flw', 'fsw', 'amo'):
            # by the old step (X the word: its low bits are not read as a register here, Z its pc; v goes to X & 63: see below)
            B.update(X=w, Z=i, path=P_RV, n=1)
            i += 4
            have_op0 = True
        elif k == 'jalr':
            if rd_of(w) == 0:
                B.update(n=1, ctl=3, cra=ra_of(w), off=sx(w >> 20, 12))
            else:
                B.update(X=w, Z=i, path=P_RV, n=1, ctl=3)
            i += 4
        elif k == 'jal':
            if rd_of(w):
                B.update(X=x_word(dest(rd_of(w)), 0, 0), Z=i + 4, n=1, ctl=2, target=i + imm_j(w))
            else:
                B.update(n=1, ctl=2, target=i + imm_j(w))
            i += 4
        elif k == 'branch':
            B.update(n=1, ctl=1, f3=f3_of(w), cra=ra_of(w), crb=rb_of(w), target=i + imm_b(w))
            i += 4
        else:
            B.update(path=P_LEAVE, n=0)
            i += 4
        # op 1, then control, each if it is next, is not a start and known
        if B['path'] != P_LEAVE and not B['ctl']:
            nx = code.get(i)
            if nx and i not in starts:
                y = lite(nx[0])
                if y is not None and B['path'] != P_RV:
                    B['Y'] = y
                    B['n'] += 1
                    i += 4
            nx = code.get(i)
            if nx and i not in starts and B['path'] != P_RV:
                w2 = nx[0]
                k2 = kind(w2)
                if k2 == 'branch':
                    B.update(n=B['n'] + 1, ctl=1, f3=f3_of(w2), cra=ra_of(w2), crb=rb_of(w2), target=i + imm_b(w2))
                    i += 4
                elif k2 == 'jal' and rd_of(w2) == 0:
                    B.update(n=B['n'] + 1, ctl=2, target=i + imm_j(w2))
                    i += 4
                elif k2 == 'jalr' and rd_of(w2) == 0:
                    B.update(n=B['n'] + 1, ctl=3, cra=ra_of(w2), off=sx(w2 >> 20, 12))
                    i += 4
        bundles.append(B)
        weight[B['n']] += n0
        if B['path'] == P_LEAVE:
            at = i
            continue
        at = i
        if at not in code and not B['ctl'] in (2, 3):
            # falls off what is known: a leaving bundle
            bundles.append(dict(X=x_word(SINK, 0, 0), Y=Y_NONE, Z=0, path=P_LEAVE, n=0, ctl=0, f3=0, cra=0, crb=0, target=None, rv=at, off=0))
    # a run that ended in a jump goes on from the next start the loop finds

# targets
leave_at = {}


def leave_bundle(t):
    if t not in leave_at:
        leave_at[t] = len(bundles)
        bundles.append(dict(X=x_word(SINK, 0, 0), Y=Y_NONE, Z=0, path=P_LEAVE, n=0, ctl=0, f3=0, cra=0, crb=0, target=None, rv=t, off=0))
    return leave_at[t]


far = 0
for idx in range(len(bundles)):
    B = bundles[idx]
    if B['target'] is not None:
        t = B['target'] & 0xffffffff
        B['tb'] = start_of[t] if t in start_of else leave_bundle(t)
    B['idx'] = idx
n = len(bundles)
print(f'{n:,} bundles; {len(code):,} RV words ({stale:,} no longer in RAM); by the profile {sum(k * v for k, v in weight.items()) / max(sum(weight.values()), 1):.3f} RV a bundle (first word counts)')
assert B0 + n < PCT

area = np.zeros((ROWS * 2048, 4), dtype=np.uint32)
for idx, B in enumerate(bundles):
    c = B0 + idx
    ctl = B['path'] | B['n'] << 3 | B['ctl'] << 6
    if B['ctl'] == 1:
        off = B['tb'] - idx
        if not -1024 <= off < 1024:
            # out of reach: the old loop takes it, from this bundle's start
            ctl = P_LEAVE
            far += 1
        else:
            ctl |= B['f3'] << 8 | B['cra'] << 11 | B['crb'] << 16 | (off & 0x7ff) << 21
    elif B['ctl'] == 2:
        ctl |= ((B['tb'] - idx) & 0xffffff) << 8
    elif B['ctl'] == 3:
        if B['path'] == P_RV:
            ctl = B['path'] | B['n'] << 3 | 3 << 6
        else:
            ctl |= B['cra'] << 11 | (B['off'] & 0x7ff) << 21
    X = B['X']
    area[c] = (X, B['Y'], B['Z'] & 0xffffffff, ctl)
    area[PCT + (idx >> 2), idx & 3] = B['rv'] & 0xffffffff
for pc, b in start_of.items():
    if K_LO <= pc < K_HI:
        t = (pc - K_LO) >> 4
    else:
        t = 256 * 2048 + ((pc - P_LO) >> 4)
    area[t, (pc >> 2) & 3] = B0 + b
# the code area's rows go in the texture's order: the file's last row is y = 0 (the harness flips it)
area.reshape(ROWS, 2048, 4)[::-1].tofile(a.out)
print(f'{far} branches out of reach left to the old loop; wrote {a.out}')
