"""The share of a gvm_cpu --profile's bundles by path (gpu_vm/isa.h, GVM_PATH_*), core 0's and the workers' apart:
paths 3-7 stay in the tick's fast loop, 1 and 2 leave it for step12, 0 for the general step.

    python gpu_vm/bench_games/paths.py FILE.prof [...]
"""
import sys
from collections import Counter

NAMES = {0: 'general', 1: 'alu (step12)', 2: 'memf (step12)', 3: 'load+lite', 4: 'load2', 5: 'store+lite', 6: 'mem2', 7: 'int'}

for path in sys.argv[1:]:
    by = [Counter(), Counter()]
    top = [Counter(), Counter()]
    for line in open(path):
        if line.startswith('#'):
            continue
        pc, w0, w1, w2, ctl, n0, nw = line.split()
        p = int(ctl, 16) & 7
        for k, n in enumerate((int(n0), int(nw))):
            by[k][p] += n
            if p < 3:
                top[k][(pc, p)] += n
    print(path)
    for k, who in enumerate(('core 0', 'workers')):
        total = sum(by[k].values())
        if not total:
            continue
        left = sum(by[k][p] for p in (0, 1, 2))
        print(f'  {who}: {total:,} bundles; leave the fast loop {left / total * 100:.1f}% '
              f'(general {by[k][0] / total * 100:.1f}%, step12 {(by[k][1] + by[k][2]) / total * 100:.1f}%)')
        print('    ' + ', '.join(f'{NAMES[p]} {by[k][p] / total * 100:.1f}%' for p in range(8)))
        print('    hottest leaving: ' + ', '.join(f'{pc} p{p} {n / total * 100:.2f}%' for (pc, p), n in top[k].most_common(6)))
