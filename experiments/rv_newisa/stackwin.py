#!/usr/bin/env python3
"""How many memory ops fall in a window of memory fixed at a pass's start around sp (features' traces): python stackwin.py
Window: [sp0 + lo, sp0 + hi) on texels, the pass being 8,192 instructions. Hits are shares of loads and of stores. Measured on the trace; no time here."""
import os, numpy as np
D = r'C:\Users\micha\agents_isa\ni\trace'
def sx(v, b): return ((v.astype(np.int64) & ((1 << b) - 1)) ^ (1 << (b - 1))) - (1 << (b - 1))
for name in ['boot', 'tdawn', 'gzip', 'mp3', 'jpeg']:
    r = np.fromfile(os.path.join(D, name + '.bin'), dtype=np.uint32).reshape(-1, 4)
    pc, w, a = r[:, 0], r[:, 1], r[:, 2]
    op = w & 0x7f; rs1 = (w >> 15) & 31
    ld = (op == 3) | (op == 7); st = (op == 0x23) | (op == 0x27)
    va = (a.astype(np.int64) + np.where(st, sx(((w >> 25) << 5) | ((w >> 7) & 31), 12), sx(w >> 20, 12))) & 0xffffffff
    # sp at each instruction: rs1's value where rs1 is sp, carried forward
    isp = rs1 == 2
    sp = np.where(isp, a.astype(np.int64), 0); idx = np.where(isp, np.arange(len(a)), 0); np.maximum.accumulate(idx, out=idx); sp = sp[idx]
    p = np.arange(len(a)) // 8192
    first = np.searchsorted(p, np.arange(p.max() + 1)); sp0 = sp[first][p]
    line = f'{name:7} loads {ld.mean()*100:5.1f}% stores {st.mean()*100:5.1f}%  by sp {(isp&ld).sum()/ld.sum()*100:4.0f}/{(isp&st).sum()/st.sum()*100:4.0f} | '
    for lo, hi in [(-64, 192), (-128, 384), (-256, 768), (-1024, 1024), (-2048, 2048)]:
        d = va - sp0; hit = (d >= lo) & (d < hi)
        line += f'[{lo},{hi}) {hit[ld].mean()*100:3.0f}/{hit[st].mean()*100:3.0f} '
    print(line)
