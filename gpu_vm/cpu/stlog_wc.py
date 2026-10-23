#!/usr/bin/env python3
"""Every core's stores from gvm_cpu's GVM_STLOG (GVM_STLOG_CORE=-1, GVM_STLOG_VALUES=1) replayed through a built tick's
write cache (nb=16 hash=1: two tables of 16 buckets of four ways, the tick's multiplicative hashes): how many bundles a
worker runs before its cache is full, by job (a job is told by its stack's 64 KB region), and the same with silent
stores (the bytes the core sees already, to a texel with no entry: the builder's sil) making no entry.

    python gpu_vm/cpu/stlog_wc.py FILE [--cores 2-8] [--nb 16]

A record is seven words: the address, sp, the core's bundles, bytes | base register << 8 | supervisor << 16 | mode << 20 |
load << 24 | core << 25, the pc, the store's bytes, the bytes it overwrites; address 0xffffffff is a pass's end.
"""
import argparse, collections
import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--cores', default='1-255')
    ap.add_argument('--nb', type=int, default=16)
    a = ap.parse_args()
    lo, hi = (int(x) for x in a.cores.split('-'))
    nb = a.nb
    sh = 32 - (nb.bit_length() - 1)
    b1 = lambda t: ((t * 0x9e3779b1) & 0xffffffff) >> sh
    b2 = lambda t: nb + ((((t * 0x85ebca6b) + 0x165667b1) & 0xffffffff) >> sh)
    r = np.fromfile(a.file, dtype=np.uint32).reshape(-1, 7)
    for skip in (0, 1):
        segs, silent, stores = collections.defaultdict(list), collections.Counter(), collections.Counter()
        for c in range(lo, hi + 1):
            rc = r[(r[:, 3] >> 25) == c]
            if not len(rc): continue
            q = [[] for _ in range(2 * nb)]; seen = set(); start = None; jobs = collections.Counter()
            for ad, sp, b, info, pc, nv, ov in rc.tolist():
                if start is None: start = b
                if ad == 0xffffffff:
                    q = [[] for _ in range(2 * nb)]; seen = set(); start = b; jobs = collections.Counter(); continue
                job = 'kernel' if (info >> 16) & 1 else '%04x' % (sp >> 16)
                jobs[job] += 1; stores[job] += 1
                n = info & 255
                ts = [ad >> 4] if (ad & 15) + n <= 16 else [ad >> 4, (ad >> 4) + 1]
                if n <= 4 and nv == ov and not any(t in seen for t in ts):
                    silent[job] += 1
                    if skip: continue
                for t in ts:
                    if t in seen: continue
                    k = next((k for k in (b1(t), b2(t)) if len(q[k]) < 4), None)
                    if k is None:
                        segs[jobs.most_common(1)[0][0]].append(b - start)
                        q = [[] for _ in range(2 * nb)]; seen = set(); start = b; jobs = collections.Counter()
                        k = b1(t)
                    q[k].append(t); seen.add(t)
        print('silent stores make no entry' if skip else 'as the tick is')
        for j in sorted(segs, key=lambda j: -stores[j]):
            if j == 'kernel' or len(segs[j]) < 5: continue
            print(f'  stack {j}xxxx: {len(segs[j])} full caches, {np.median(segs[j]):.0f} bundles before full (median); '
                  f'silent {silent[j] / max(1, stores[j]) * 100:.0f}% of {stores[j]} stores')


if __name__ == '__main__':
    main()
