#!/usr/bin/env python3
"""The table of results.jsonl: python show.py [--tag TAG] [FILE]. Means over runs; wall ms between the marks (boot: to the first)."""
import json, os, sys
from collections import defaultdict
a = sys.argv[1:]
tag = None
if '--tag' in a:
    i = a.index('--tag'); tag = a[i + 1]; del a[i:i + 2]
f = a[0] if a else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'results.jsonl')
R = defaultdict(list)
for l in open(f):
    r = json.loads(l)
    if not r.get('done'): continue
    R[(r['load'], r['machine'], r.get('tag', ''))].append(r)
print('load machine tag runs wall_ms(each) passes core0/pass why...')
for k in sorted(R, key=lambda k: (k[0], k[1] != 'rv', k[2])):
    if tag is not None and k[2] not in ('', tag): continue
    rs = R[k]
    part = 'boot' if k[0] == 'boot' else 'work'
    w = [r[part]['wall_ms'] for r in rs]
    x = rs[-1][part]
    why = ' '.join(f"{n} {v['share']:.0f}%@{v['a_pass']}" for n, v in x['why'].items())
    print(f"{k[0]:6} {k[1]:3} {k[2]:8} {len(rs)} {sum(w) / len(w):9.0f} {w} p={x['passes']} c0/p={x['core0_a_pass']:.0f} {why} | {(rs[-1]['proof'] or [''])[-1][:70]} | oops {rs[-1]['oops']}")
