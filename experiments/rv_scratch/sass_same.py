#!/usr/bin/env python3
"""What differs between the listings of build/sc_sass past constant-address local stores, by opcode, and the field-to-store path's lines and waits at each size.
python experiments/rv_scratch/sass_same.py"""
import collections, difflib, glob, os, re
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'sc_sass'))
def load(n, s):
    L = []
    for l in open(glob.glob(os.path.join(ROOT, 'nsight_' + n, f'*.{s}.sass'))[0]):
        m = re.match(r'\s*/\*([0-9a-f]{4,})\*/\s+(.*)', l)
        if m:
            b = re.sub(r'\s+', ' ', m.group(2)).strip(); t = b.split()
            L.append((m.group(1), (t[1] if t[0].startswith('@') else t[0]).split('.')[0], b))
    return L
const = re.compile(r'STL \[(UR\d+(\+0x[0-9a-f]+)?|RZ|0x[0-9a-f]+)\]')
for s in ('s0', 's2'):
    for a, b in (('n16', 'n32'), ('n32', 'n64'), ('n64', 'n256')):
        A, B = load(a, s), load(b, s)
        print(f'{s} {a} {len(A)} -> {b} {len(B)}: constant-address STL {sum(1 for x in A if const.match(x[2]))} -> {sum(1 for x in B if const.match(x[2]))}, opcode counts changed:', end=' ')
        ca, cb = collections.Counter(x[1] for x in A), collections.Counter(x[1] for x in B)
        print({k: cb[k] - ca[k] for k in set(ca) | set(cb) if cb[k] != ca[k]})
    for n in ('tree', 'n16', 'n32', 'n64', 'n256'):
        L = load(n, s); i = next(j for j, x in enumerate(L) if re.search(r'SHF\.R\.U32\.HI R\d+, RZ, 0xf, R\d+', x[2]) and any(y[1] == 'LDL' for y in L[j:j + 12]) and any('0x14' in y[2] for y in L[j - 4:j + 4]))
        j = i - 2
        while L[j][1] != 'SHF': j += 1
        e = next(k for k in range(i, i + 80) if L[k][1] == 'BSSY' or L[k][1] == 'BRA')
        path = L[j:e]
        print(f'  {n}: {len(path)} lines to the first branch; LDL {sum(x[1]=="LDL" for x in path)}, STL {sum(x[1]=="STL" for x in path)}; opcodes and waits', ' '.join(x[1][0:3] + (re.search(r'\?(\w+)', x[2]).group(1).replace('WAIT', 'W').replace('_END_GROUP', '').replace('trans', 't')) for x in path))
