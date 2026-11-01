#!/usr/bin/env python3
"""How many instructions merged forms would take away, from a long profile (pcs.txt: class table pc word executed ...): python pairs.py PCS [PCS...]
Adjacent words only, a pair weighted by min(count of each), each word in at most one pair (greedy by address). Inferred from counts: nothing here is run. The second must use the first's result where that is said; a pair's rd rule is not checked for liveness."""
import collections, sys
def fields(w): return w & 0x7f, w >> 7 & 31, w >> 12 & 7, w >> 15 & 31, w >> 20 & 31
def kind(a, b):
    o1, d1, f1, s1, t1 = fields(a); o2, d2, f2, s2, t2 = fields(b)
    if o1 == 0x37 and o2 == 0x13 and s2 == d1 and f2 == 0: return 'lui+addi'
    if o1 in (0x37, 0x17) and o2 in (0x03, 0x23, 0x67, 0x13) and s2 == d1: return 'lui/auipc+use'
    if o1 == 0x13 and f1 == 1 and o2 == 0x33 and f2 == 0 and b >> 25 == 0 and (s2 == d1 or t2 == d1): return 'slli+add'
    if o1 in (0x33, 0x13) and d1 and o2 == 0x03 and s2 == d1: return 'alu+load'
    if o1 in (0x33, 0x13) and d1 and o2 == 0x23 and s2 == d1: return 'alu+store'
    if o1 in (0x33, 0x13) and d1 and o2 == 0x63 and d1 in (s2, t2): return 'alu+branch'
    if o1 == 0x03 and d1 and o2 == 0x63 and d1 in (s2, t2): return 'load+branch'
    if o1 == 0x03 and d1 and o2 in (0x13, 0x33) and d1 in (s2, t2 if o2 == 0x33 else -1): return 'load+alu'
    if o1 == 0x03 and o2 == 0x03 and s1 == s2 and abs((a >> 20) - (b >> 20)) == 4 and d1 != s1: return 'load pair'
    if o1 == 0x23 and o2 == 0x23 and s1 == s2 and f1 == 2 == f2: return 'store pair'
    if o1 in (0x33, 0x13) and d1 and o2 in (0x33, 0x13) and d1 == s2: return 'alu+alu chained'
    if o1 in (0x33, 0x13) and o2 == 0x67: return 'alu+jalr'
    if o1 == 0x03 and o2 == 0x67: return 'load+jalr'
    return None
for path in sys.argv[1:]:
    P = {};
    for l in open(path):
        if l[0] == '#': continue
        p = l.split()
        P[int(p[2], 16)] = (int(p[3], 16), int(p[4]))
    tot = sum(c for w, c in P.values()); got = collections.Counter(); used = 0
    prev = None
    for pc in sorted(P):
        if prev == pc: continue   # the second of the last pair
        n = P.get(pc + 4)
        if n:
            k = kind(P[pc][0], n[0])
            if k: got[k] += min(P[pc][1], n[1]); prev = pc + 4
    print(path, 'instructions', tot, 'taken away %.1f%%' % (100 * sum(got.values()) / tot), ' '.join('%s %.1f' % (k, 100 * v / tot) for k, v in got.most_common()))
