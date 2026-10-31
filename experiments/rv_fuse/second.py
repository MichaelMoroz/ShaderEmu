#!/usr/bin/env python3
"""Which second ops would give how many merges, a pair lying in one texel or not, the first a fast kind.

    python second.py DIR...      (DIR: an RVC_PROF folder, runprof.sh)
"""
import collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from study import kind, load

F3I = ['addi', 'slli', 'slti', 'sltiu', 'xori', 'srli', 'ori', 'andi']
F3R = ['add', 'sll', 'slt', 'sltu', 'xor', 'srl', 'or', 'and']


def name(w):
    k = kind(w)
    f3 = (w >> 12) & 7
    if k == 'alui': return 'srai' if f3 == 5 and (w >> 30) & 1 else F3I[f3]
    if k == 'alur': return 'sub' if f3 == 0 and (w >> 30) & 1 else 'sra' if f3 == 5 and (w >> 30) & 1 else F3R[f3]
    if k == 'branch': return 'b.' + ['eq', 'ne', '?', '?', 'lt', 'ge', 'ltu', 'geu'][f3]
    if k == 'load': return ['lb', 'lh', 'lw', '?', 'lbu', 'lhu', '?', '?'][f3]
    if k == 'store': return ['sb', 'sh', 'sw', '?', '?', '?', '?', '?'][f3]
    return k


FIRST = ('alui', 'alur', 'lui', 'auipc', 'load', 'store', 'fp', 'flw', 'fsw', 'mul')
LOGIC = {'andi', 'ori', 'xori', 'slli', 'srli', 'srai', 'and', 'or', 'xor', 'sll', 'srl', 'sra', 'slt', 'sltu', 'slti', 'sltiu'}
BR = {'b.eq', 'b.ne', 'b.lt', 'b.ge', 'b.ltu', 'b.geu'}
BASE = {'addi', 'lui', 'auipc'}
SETS = {
    'addi': {'addi'},
    'addi,lui,auipc': BASE,
    '+add,sub': BASE | {'add', 'sub'},
    '+logic,shifts': BASE | {'add', 'sub'} | LOGIC,
    '+beq,bne': BASE | {'add', 'sub'} | LOGIC | {'b.eq', 'b.ne'},
    '+all branches': BASE | {'add', 'sub'} | LOGIC | BR,
    'addi,add,sub,br': BASE | {'add', 'sub'} | BR,
    '+fp': BASE | {'add', 'sub'} | LOGIC | BR | {'fp'},
}
for d in (sys.argv[1:] if __name__ == "__main__" else []):
    rows = load(d)
    print('==', os.path.basename(d))
    for cls in (0, 1):
        pcs = {}
        for c, t, pc, w, n, l, s, nt, fl in rows:
            if c != cls: continue
            e = pcs.get(pc)
            if e: e[1] += n
            else: pcs[pc] = [w, n]
        tot = sum(v[1] for v in pcs.values())
        if not tot: continue
        m = collections.Counter()
        for w, n in pcs.values():
            m[name(w)] += n
        print(' ', 'core0' if cls == 0 else 'workers', tot, ' '.join(f'{k}:{100 * v / tot:.1f}' for k, v in m.most_common(26)))
        for sname, S in SETS.items():
            line = []
            for straddle in (False, True):
                cands = []
                for pc, (w, n) in pcs.items():
                    if kind(w) not in FIRST: continue
                    if not straddle and (pc >> 2) & 3 == 3: continue
                    nx = pcs.get(pc + 4)
                    if not nx or name(nx[0]) not in S: continue
                    if (nx[0] >> 7) & 31 == 0 and kind(nx[0]) != 'branch': continue
                    cands.append((min(n, nx[1]), pc))
                cands.sort(reverse=True)
                used = set()
                saved = 0
                for c, pc in cands:
                    if pc in used or pc + 4 in used: continue
                    used |= {pc, pc + 4}
                    saved += c
                line.append(f'{100 * saved / tot:5.1f}% merged, {tot / (tot - saved):.3f} a trip')
            print(f'    {sname:16s} in a texel {line[0]}; over texels {line[1]}')
