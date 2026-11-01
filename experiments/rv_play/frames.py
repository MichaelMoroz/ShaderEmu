#!/usr/bin/env python3
"""h2h's .frames (pc, instructions, stall, us since start) between its marks' passes: what ended core 0's passes, by reason and pc.
    frames.py FILE [FIRST [COUNT]] [--elf ELF]"""
import collections, struct, sys
WHY = {0: 'budget', 1: 'exit call', 2: 'csr full', 3: 'full', 6: 'uart', 7: 'fence.i', 8: 'copy', 9: 'pause', 10: 'fill'}
a = [x for x in sys.argv[1:]]
raw = open(a[0], 'rb').read()
rec = list(struct.iter_unpack('<4I', raw[:len(raw) // 16 * 16]))
first = int(a[1]) if len(a) > 1 else 0
count = int(a[2]) if len(a) > 2 else len(rec) - first
rec = rec[first:first + count]
us = rec[-1][3] - rec[0][3]
print(f'{len(rec)} passes, {us / 1000:.0f} ms, {sum(r[1] for r in rec) / 1e6:.2f} M, {us / 1000 / len(rec):.3f} ms a pass')
by = collections.defaultdict(lambda: [0, 0, 0])
pl = collections.defaultdict(lambda: [0, 0, 0])
for i, r in enumerate(rec[1:], 1):
    w = r[2] & 0xff
    t = r[3] - rec[i - 1][3]
    by[w][0] += 1; by[w][1] += r[1]; by[w][2] += t
    if w:
        k = (w, r[0] & ~0xf)
        pl[k][0] += 1; pl[k][1] += r[1]; pl[k][2] += t
for w, v in sorted(by.items(), key=lambda e: -e[1][0]):
    print(f'{WHY.get(w, w):10} {v[0]:6} passes {v[0] / len(rec) * 100:5.1f}%  {v[1] // v[0]:5} a pass  {v[2] / v[0] / 1000:.3f} ms a pass  lost to 8192: {(8192 * v[0] - v[1]) / 1e6:.2f} M')
for k, v in sorted(pl.items(), key=lambda e: -e[1][0])[:25]:
    print(f'{v[0]:6} {WHY.get(k[0], k[0]):8} pc {k[1]:08x} {v[1] // v[0]:5} a pass, lost {(8192 * v[0] - v[1]) / 1e6:.2f} M')
