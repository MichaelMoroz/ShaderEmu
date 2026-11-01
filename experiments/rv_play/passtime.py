#!/usr/bin/env python3
"""What a pass's time goes by, on h2h's card log: passtime.py FILE. Each pass: core 0's instructions, workers' instructions and cores (FILE.workers), and its time.
Time as a line of core 0's instructions, workers' instructions, workers running; then by how many workers ran and by its instructions."""
import struct, sys
import numpy as np
def load(p, n):
    raw = open(p, 'rb').read()
    return np.array(list(struct.iter_unpack('<%dI' % n, raw[:len(raw) // (4 * n) * (4 * n)])), dtype=np.int64)
f = load(sys.argv[1], 4)
w = load(sys.argv[1] + '.workers', 4)
n = min(len(f), len(w))
f, w = f[:n], w[:n]
t = np.diff(f[:, 3], prepend=f[0, 3]).astype(float)
ok = t < 20000
ok[0] = False
first = int(sys.argv[2]) if len(sys.argv) > 2 else 0
ok[:first] = False
A = np.c_[np.ones(ok.sum()), f[ok, 1], w[ok, 0], w[ok, 1]]
c = np.linalg.lstsq(A, t[ok], rcond=None)[0]
print('passes', ok.sum(), 'wall ms', t[ok].sum() / 1000)
print('us = %.1f + %.4f core0 + %.5f worker instr + %.2f a worker' % tuple(c))
print('sums ms: base %.0f core0 %.0f workers instr %.0f workers %.0f' % (c[0] * ok.sum() / 1000, c[1] * f[ok, 1].sum() / 1000, c[2] * w[ok, 0].sum() / 1000, c[3] * w[ok, 1].sum() / 1000))
print('cores  passes  mean us  core0 instr')
for lo, hi in ((0, 0), (1, 1), (2, 2), (3, 4), (5, 8), (9, 16), (17, 32), (33, 64), (65, 300)):
    m = ok & (w[:, 1] >= lo) & (w[:, 1] <= hi)
    if m.sum():
        print('%3d-%3d %6d %8.0f %8.0f  %5.1f%%' % (lo, hi, m.sum(), t[m].mean(), f[m, 1].mean(), 100 * t[m].sum() / t[ok].sum()))
print('core0 budget passes (8192) by time:')
m = ok & (f[:, 1] == 8192)
print(np.percentile(t[m], [5, 25, 50, 75, 95, 99]).round(0))
print('slowest 1% of passes, share of time', round(100 * np.sort(t[ok])[-ok.sum() // 100:].sum() / t[ok].sum(), 1))
