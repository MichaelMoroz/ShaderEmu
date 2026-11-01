#!/usr/bin/env python3
"""The account's table from results.jsonl: the last run of each load (and tag)."""
import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
last = {}
for l in open(os.path.join(HERE, 'results.jsonl')):
    r = json.loads(l); last[(r['load'], r.get('tag', ''))] = r
print('%-14s %8s %6s %7s %7s | %5s %5s %5s %5s %5s | %5s %5s %5s | %s' % ('load', 'wall ms', 'pass', 'c0 M', 'wk M', 'idle', 'fix', 'c0', 'wk', 'rest', 'instr/p', 'full%', 'wait+wk', 'fit a us, ns c0, ns wk'))
for (load, tag), r in last.items():
    w = r.get('boot') if load == 'boot' else r.get('work')
    if not w: print(load, tag, 'NOT DONE'); continue
    s = w['shares']; k = w['kinds']
    print('%-14s %8.0f %6d %7.1f %7.1f | %5.1f %5.1f %5.1f %5.1f %5.1f | %5.0f %5.1f %5.1f | %s %s %s' % (load + ('/' + tag if tag else ''), w['wall_ms'], w['passes'], w['core0'] / 1e6, w['workers'] / 1e6, s['idle'], s['fixed'], s['core0'], s['workers'], s['rest'],
          w['core0_a_pass'], k.get('write full', {}).get('share', 0), w['core0_waits_with_workers'], w['fit_us']['a'], w['fit_us']['b_ns'], w['fit_us']['c_ns']))
if len(sys.argv) > 1:
    for (load, tag), r in last.items():
        if sys.argv[1] != 'all' and sys.argv[1] != load: continue
        w = r.get('boot') if load == 'boot' else r.get('work')
        if not w: continue
        print('\n==', load, tag, 'workers', w['worker_passes'], 'proof', r.get('proof'))
        for k, v in sorted(w['kinds'].items(), key=lambda e: -e[1]['wall']):
            print('  %-34s %6d passes %5.1f%% %5.1f%% wall %8.1f ms  %5d instr %6d us  with workers %d' % (k, v['passes'], v['share'], v['wall'], v['ms'], v['a_pass'], v['us_a_pass'], v['worker_passes']))
        for k, v in w['places'].items(): print('    %-50s %6d %8.1f ms' % (k, v[0], v[1]))
