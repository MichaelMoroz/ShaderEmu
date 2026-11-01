#!/usr/bin/env python3
"""whole.jsonl as a table, medians: wall between the marks (boot: to the first mark), passes, instructions a pass and full share of core 0, x against lean."""
import json, os, statistics as st
rows = {}
for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'whole.jsonl')):
    r = json.loads(l)
    d = r.get('boot') if r['load'] == 'boot' else r.get('work')
    if not d or not r.get('done'): continue
    rows.setdefault((r['load'], r['tag']), []).append((d['wall_ms'], d['passes'], d['core0_a_pass'], d['why'].get('full', {}).get('share', 0), r.get('proof', [])[-1:], r.get('oops', 0)))
base = {}
for (l, t), v in sorted(rows.items(), key=lambda e: (e[0][0], e[0][1] != 'lean')):
    w = st.median(x[0] for x in v)
    if t == 'lean': base[l] = w
    print(f"{l:9} {t:6} n={len(v)} wall {w:9.1f} ({min(x[0] for x in v):.0f}-{max(x[0] for x in v):.0f}) x {base.get(l, w) / w:.3f} passes {st.median(x[1] for x in v):7.0f} a pass {st.median(x[2] for x in v):7.0f} full {st.median(x[3] for x in v):5.1f}% oops {sum(x[5] for x in v)} {v[-1][4]}")
