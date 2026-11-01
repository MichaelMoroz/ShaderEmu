#!/usr/bin/env python3
"""The predecoded form of real code: python xlate.py SNAP N OUT.bin -> prints CODE_LO and how many texels. A texel for each RV word, at (pc - CODE_LO) >> 2 of `_Code` (2048 wide).
w0: 0-5 rd (x0 is the sink, 32), 6-11 rs1, 12-17 rs2, 18-20 f3, 21 register operand, 22 alt (sub, sra), 23 load, 24 branch, 25 way out (w3's RV word is done by the old decode), 26 jal, 27 unsigned compare.
w1: the immediate (lui and auipc and jal's link as add x0 + imm). w2: a taken branch's or jal's texel. w3: the RV word."""
import argparse, sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import snapsim as S
SINK = 32
def one(w, pc, lo):
    op = w & 0x7f; f3 = w >> 12 & 7; rd = w >> 7 & 31; r1 = w >> 15 & 31; r2 = w >> 20 & 31; dst = rd or SINK; f7 = w >> 25
    F = lambda d, a, b, f, fl: d | a << 6 | b << 12 | f << 18 | fl
    if op == 0x13:
        im = S.sx(w >> 20, 12) & S.M
        return F(dst, r1, 0, f3, (1 << 22 if w >> 30 & 1 else 0) | (1 << 27 if f3 == 3 else 0)), im if f3 not in (1, 5) else im & 31, 0, w
    if op == 0x33 and f7 != 1:
        return F(dst, r1, r2, f3, 1 << 21 | (1 << 22 if w >> 30 & 1 else 0) | (1 << 27 if f3 == 3 else 0)), 0, 0, w
    if op == 0x37: return F(dst, 0, 0, 0, 0), w & 0xfffff000, 0, w
    if op == 0x17: return F(dst, 0, 0, 0, 0), (pc + (w & 0xfffff000)) & S.M, 0, w
    if op == 0x6f:
        tg = (pc + S.sx((w >> 31 << 20) | (w & 0xff000) | ((w >> 20 & 1) << 11) | (w >> 20 & 0x7fe), 21)) & S.M
        return F(dst, 0, 0, 0, 1 << 26), (pc + 4) & S.M, ((tg - lo) >> 2) & S.M, w
    if op == 0x63 and f3 not in (2, 3):
        tg = (pc + S.sx((w >> 31 << 12) | ((w >> 7 & 1) << 11) | (w >> 20 & 0x7e0) | (w >> 7 & 0x1e), 13)) & S.M
        return F(SINK, r1, r2, f3, 1 << 21 | 1 << 24 | (1 << 27 if f3 >= 6 else 0)), 0, ((tg - lo) >> 2) & S.M, w
    if op == 0x03 and f3 in (0, 1, 2, 4, 5):
        return F(dst, r1, 0, f3, 1 << 23), S.sx(w >> 20, 12) & S.M, 0, w
    return 1 << 25, 0, 0, w
def trace(sn, x, pc, n):
    """the pcs a run reaches, by the model, one step at a time"""
    return S.run(sn, x, pc, n, pcs=set())[0]['pcs']
def main():
    ap = argparse.ArgumentParser(); ap.add_argument('snap'); ap.add_argument('n', type=int); ap.add_argument('out'); a = ap.parse_args()
    sn = S.Snap(a.snap); x, pc = S.regs(sn); pcs = trace(sn, x, pc, a.n)
    lo = min(pcs) & ~0xfff; hi = (max(pcs) | 0xfff) + 1
    n = (hi - lo) >> 2
    if n > 2048 * 9216: sys.exit('too wide')
    arr = np.zeros((n, 4), np.uint32)
    sn2 = S.Snap(a.snap)
    for page in sorted({p & ~0xfff for p in pcs}):
        for at in range(page, page + 4096, 4):
            arr[(at - lo) >> 2] = one(sn2.r32(at), at, lo)
    arr.tofile(a.out)
    print('CODE_LO=0x%08xu texels=%d' % (lo, n))
if __name__ == '__main__': main()
