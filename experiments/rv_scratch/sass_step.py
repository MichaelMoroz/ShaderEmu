#!/usr/bin/env python3
"""The fast loop's first lines in each listing of build/sc_sass: from where the instruction's fields are cut to where xr and the array have been read and the array written, with each listing's zeroed ranges.
python experiments/rv_scratch/sass_step.py [s0|s2] [lines]"""
import glob, os, re, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'sc_sass'))
S = sys.argv[1] if len(sys.argv) > 1 else 's0'
N = int(sys.argv[2]) if len(sys.argv) > 2 else 60
I = re.compile(r'\s*/\*([0-9a-f]{4,})\*/\s+(.*)')
for n in ('tree', 'n16', 'n32', 'n64', 'n256'):
    L = []
    for l in open(glob.glob(os.path.join(ROOT, 'nsight_' + n, f'*.{S}.sass'))[0]):
        m = I.match(l)
        if m: L.append((m.group(1), re.sub(r'\s+', ' ', m.group(2)).strip()))
    print(f'===== {n} {S}: {len(L)} instructions')
    # where the entry's constant stores lie
    base = {}
    ur = {}
    for a, b in L[:3000]:
        m = re.match(r'UIADD3 (UR\d+), UPT, UPT, URZ, (0x[0-9a-f]+), URZ', b)
        if m: ur[m.group(1)] = int(m.group(2), 16)
        m = re.match(r'STL \[(?:(UR\d+)(?:\+(0x[0-9a-f]+))?|(RZ)|(0x[0-9a-f]+))\]', b)
        if m:
            x = ur.get(m.group(1), 0) + int(m.group(2) or '0', 16) if m.group(1) else int(m.group(4) or '0', 16)
            base[x] = 1
    k = sorted(base); print('entry stores local addresses', hex(k[0]), 'to', hex(k[-1]), 'count', len(k))
    hits = [i for i, (a, b) in enumerate(L) if re.search(r'SHF\.R\.U32\.HI R\d+, RZ, 0xf, R\d+', b)]
    for i in hits:
        if any('LDL' in x[1] for x in L[i:i + 10]) and any('0x14' in x[1] for x in L[i - 4:i + 4]):
            for x in L[i - 3:i + N]: print('  ', x[0], x[1][:92])
            break
    print('   all LDL/STL by a computed address:')
    print('   ', ' | '.join(f'{a}:{b.split(",")[0] if b.startswith("STL") else b[:18]}' for a, b in L if re.match(r'(LDL\S* \S+, |STL\S* )\[R\d+', b))[:6000])
