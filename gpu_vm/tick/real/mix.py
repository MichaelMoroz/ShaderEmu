#!/usr/bin/env python3
"""The bundle mix of a gvm_cpu --trace on stdin (gpu_vm/tick/real): paths, cc 0's subs, branches taken, the fast loop's
runs (bundles of paths 3-7 between two of another path), the hottest bundles with a path that leaves the fast loop.
   gvm_cpu --trace PROG.elf 2>&1 >/dev/null | python3 mix.py [--skip N]"""
import sys, re, collections
skip = int(sys.argv[sys.argv.index('--skip') + 1]) if '--skip' in sys.argv else 0
line_re = re.compile(r'^\[(\d+)\] ([0-9a-f]{8}) ([0-9a-f]{8})  (.*)$')
paths = collections.Counter(); subs = collections.Counter(); cc = collections.Counter()
taken = collections.Counter(); slow_pcs = collections.Counter(); text = {}
n = 0; prev = None; runs = 0; inrun = False; nops = collections.Counter()
for line in sys.stdin:
    m = line_re.match(line)
    if not m: continue
    pc = int(m.group(2), 16); ctl = int(m.group(3), 16)
    if prev is not None and n > skip:
        ppc, pctl = prev
        c = (pctl >> 7) & 15
        if c != 0 and c != 5:
            taken[('taken' if pc != (ppc & ~15) + 16 else 'not')] += 1
    prev = (pc, ctl)
    n += 1
    if n <= skip: continue
    p = ctl & 7; c = (ctl >> 7) & 15
    paths[p] += 1; cc[c] += 1; nops[(ctl >> 3) & 3] += 1
    if c == 0: subs[(ctl >> 11) & 7] += 1
    fast = p >= 3
    if fast and not inrun: runs += 1
    inrun = fast
    if not fast: slow_pcs[pc] += 1; text[pc] = m.group(4)
tot = sum(paths.values())
print(f'bundles {tot}')
print('paths: ' + ', '.join(f'{p} {100*paths[p]/tot:.1f}%' for p in range(8)))
print('ops: ' + ', '.join(f'{k} {100*nops[k]/tot:.1f}%' for k in range(3)))
subn = {0:'ill',1:'none',2:'jr',3:'callr',4:'eret',5:'j',6:'call',7:'add'}
print('cc 0: ' + ', '.join(f'{subn[s]} {100*subs[s]/tot:.2f}%' for s in sorted(subs)))
print('cc: ' + ', '.join(f'{k} {100*cc[k]/tot:.1f}%' for k in sorted(cc)))
tb = taken['taken'] + taken['not']
if tb: print(f'conditional branches {100*tb/tot:.1f}% of bundles, taken {100*taken["taken"]/tb:.1f}%')
print(f'fast loop runs {runs}: {tot/runs:.1f} bundles a run' if runs else '')
for pc, k in slow_pcs.most_common(15): print(f'{k:9d} {pc:08x} {text[pc]}')
