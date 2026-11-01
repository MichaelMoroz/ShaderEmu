#!/usr/bin/env python3
"""Runs of passes that ended waiting, on the card's log: waits.py FILE FIRST COUNT. Workers' words per pass come from FILE.workers (ran, cores, full, wait)."""
import collections, struct, sys
f, first, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
def load(p, n):
    raw = open(p, 'rb').read()
    return list(struct.iter_unpack('<%dI' % n, raw[:len(raw) // (4 * n) * (4 * n)]))
rec = load(f, 4)[first:first + count]
wk = load(f + '.workers', 4)
print('workers log', len(wk), 'frames log', len(load(f, 4)))
wk = wk[first:first + count]
BASE = 'kernel/game'
runs = collections.Counter(); lens = collections.defaultdict(collections.Counter)
i = 0; prev = None
time_by = collections.Counter(); n_by = collections.Counter(); ins_by = collections.Counter()
while i < len(rec):
    r = rec[i]
    if r[2] & 0xff == 9:
        kind = 'kernel' if r[0] < 0x85000000 else 'game'
        j = i
        while j + 1 < len(rec) and rec[j + 1][2] & 0xff == 9:
            j += 1
        n = j - i + 1
        lens[kind][min(n, 12)] += 1
        # the run's first pass did work before it paused; the rest are waiting
        for k in range(i, j + 1):
            n_by[kind + (' first' if k == i else ' more')] += 1
            ins_by[kind + (' first' if k == i else ' more')] += rec[k][1]
            if k: time_by[kind + (' first' if k == i else ' more')] += rec[k][3] - rec[k - 1][3]
        i = j + 1
    else:
        i += 1
for k in n_by:
    print(f'{k:14} {n_by[k]:6} passes, {ins_by[k] // n_by[k]:5} a pass, {time_by[k] / 1000:7.0f} ms')
for k in lens:
    print(k, 'runs by length', sorted(lens[k].items()))
ran = sum(w[0] for w in wk); busy = sum(1 for w in wk if w[0])
print(f'workers: {ran / 1e6:.1f} M, passes with any {busy}, cores a pass {sum(w[1] for w in wk) / len(wk):.1f}')
# time of a budget pass by how much the workers ran
b = collections.defaultdict(lambda: [0, 0])
for i in range(1, len(rec)):
    if rec[i][2] & 0xff == 0:
        k = min(wk[i][0] // 8192, 12)
        b[k][0] += 1; b[k][1] += rec[i][3] - rec[i - 1][3]
for k in sorted(b):
    print(f'budget pass, workers ran {k * 8192:6}+: {b[k][0]:5} passes, {b[k][1] / b[k][0] / 1000:.3f} ms')
