#!/usr/bin/env python3
"""A run's window in buckets of passes: core 0's instructions and commonest pc, workers' instructions, cores running, the most one ran.  timeline.py FILE FIRST COUNT [BUCKET]"""
import collections, struct, sys
f, first, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
b = int(sys.argv[4]) if len(sys.argv) > 4 else 100
def load(p, n):
    raw = open(p, 'rb').read()
    return list(struct.iter_unpack('<%dI' % n, raw[:len(raw) // (4 * n) * (4 * n)]))
rec = load(f, 4)[first:first + count]; wk = load(f + '.workers', 4)[first:first + count]; most = load(f + '.most', 1)[first:first + count]
for i in range(0, len(rec), b):
    r = rec[i:i + b]; w = wk[i:i + b]; m = most[i:i + b]
    pc = collections.Counter(x[0] & ~0xff for x in r).most_common(1)[0][0]
    print(f'{i:6} {(r[-1][3] - r[0][3]) / 1000:7.1f} ms  core0 {sum(x[1] for x in r) / len(r):6.0f} a pass pc~{pc:08x}  workers {sum(x[0] for x in w) / len(r):8.0f} a pass cores {sum(x[1] for x in w) / len(r):5.1f} most {sum(x[0] for x in m) / len(r):6.0f}  full {sum(x[2] for x in w)} wait {sum(x[3] for x in w)}')
