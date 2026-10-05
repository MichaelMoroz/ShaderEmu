"""Averages an Nsight GPU Trace export (GPUTRACE_REGIMES.xls) per marker over the traced frames.

usage: nsight_summary.py GPUTRACE_REGIMES.xls [substring ...]   (substrings filter metric names)
"""
import collections, sys
path = sys.argv[1]
filters = sys.argv[2:]
rows = [l.split('\t') for l in open(path).read().replace('\r', '\n').split('\n') if l.strip()]
hdr = rows[0]
acc = collections.defaultdict(lambda: collections.defaultdict(list))
for r in rows[1:]:
    for name, v in zip(hdr[1:], r[1:]):
        try:
            acc[r[0]][name].append(float(v))
        except ValueError:
            pass
names = list(collections.OrderedDict.fromkeys(r[0] for r in rows[1:]))
metrics = list(collections.OrderedDict.fromkeys(hdr[1:]))
print('%d event rows, %d distinct metrics' % (len(rows) - 1, len(metrics)))
print('%-80s' % 'metric' + ''.join('%14s' % n[:13] for n in names))
for m in metrics:
    vals = [(sum(acc[n][m]) / len(acc[n][m])) if acc[n][m] else None for n in names]
    if all(v is None or v == 0 for v in vals) or (filters and not any(f in m for f in filters)):
        continue
    print('%-80s' % m[:79] + ''.join('%14s' % ('-' if v is None else ('%.4g' % v)) for v in vals))
