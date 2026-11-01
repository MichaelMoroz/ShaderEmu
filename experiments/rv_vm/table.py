#!/usr/bin/env python3
"""Each load: lean, gpu_vm's variants: mean wall, x lean, passes, core 0's a pass, share ended full. python table.py [TAG ...]"""
import json, os, sys
from collections import defaultdict
R = defaultdict(list)
for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'results.jsonl')):
    r = json.loads(l)
    if r.get('done'): R[(r['load'], r['machine'], r.get('tag', ''))].append(r)
tags = sys.argv[1:]
for load in ['boot', 'gzip', 'jpeg', 'png', 'mp3_1', 'mp3', 'flac', 'mpeg', 'avi', 'tdawn']:
    rv = R.get((load, 'rv', ''))
    if not rv: continue
    part = 'boot' if load == 'boot' else 'work'
    base = sum(x[part]['wall_ms'] for x in rv) / len(rv)
    w = rv[-1][part]['why']
    print(f"{load:6} lean {base:8.0f}          full {w.get('full', {}).get('share', 0):4.1f}%  {rv[-1][part]['core0_a_pass']:.0f} a pass")
    for t in [''] + tags:
        g = R.get((load, 'gvm', t))
        if not g: continue
        a = sum(x[part]['wall_ms'] for x in g) / len(g)
        y = g[-1][part]
        print(f"       {t or 'tree':8} {a:8.0f} {base / a:5.2f}x full {y['why'].get('full', {}).get('share', 0):4.1f}%  {y['core0_a_pass']:.0f} a pass, {y['passes']} passes, {a / y['passes']:.2f} ms a pass  n={len(g)}")
