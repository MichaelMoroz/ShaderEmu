"""The study of translating code in the tick (docs/plans/riscv-dbt.md): over RVC_PROF pcs.txt, each part's mix, run
length between taken jumps (a branch taken counted as its count less the next word's: a lower bound), the pairs a fused
kind would absorb (greedy along the code, min of the two counts), and the code a run touches (instructions and texels
that cover 90, 99 and 99.9% of it).

    python tools/rv_compare/dbt_model.py DIR:CLASS [DIR:CLASS ...]     (CLASS 0 core 0, 1 the workers; writes model.json)
"""
import sys, os, collections

def sx(v, b): return v - (1 << b) if v & (1 << (b - 1)) else v

def fields(w):
    return w & 0x7f, (w >> 7) & 31, (w >> 12) & 7, (w >> 15) & 31, (w >> 20) & 31, w >> 25

def kind(w):
    op, rd, f3, rs1, rs2, f7 = fields(w)
    if op == 0x13: return 'alu'
    if op == 0x33: return 'mul' if f7 == 1 else 'alu'
    if op == 0x03: return 'load'
    if op == 0x23: return 'store'
    if op == 0x63: return 'branch'
    if op == 0x6f: return 'jal'
    if op == 0x67: return 'jalr'
    if op == 0x37: return 'lui'
    if op == 0x17: return 'auipc'
    if op in (0x07, 0x27, 0x53, 0x43, 0x47, 0x4b, 0x4f): return 'float'
    if op == 0x2f: return 'amo'
    return 'other'

def writes(w):
    op, rd, *_ = fields(w)
    if op in (0x13, 0x33, 0x03, 0x37, 0x17, 0x6f, 0x67) and rd: return rd
    return None

def reads(w):
    op, rd, f3, rs1, rs2, f7 = fields(w)
    if op in (0x33, 0x23, 0x63, 0x2f): return {rs1, rs2}
    if op in (0x13, 0x03, 0x67, 0x07, 0x27): return {rs1}
    return set()

def pair_kind(a, b):
    """name of a fusion of a then b (b reads a's result), or None"""
    ka, kb = kind(a), kind(b)
    rd = writes(a)
    if rd is None or rd not in reads(b): return None
    opa, _, f3a, rs1a, _, _ = fields(a)
    if ka == 'lui' and kb in ('alu', 'load', 'store'): return 'lui+use'
    if ka == 'auipc' and kb in ('alu', 'load', 'store', 'jalr'): return 'auipc+use'
    if kb == 'branch':
        if ka == 'alu' and opa == 0x13 and f3a == 0 and rs1a == 0: return 'li+branch'
        if ka == 'alu' and opa == 0x13 and f3a == 0 and rs1a == rd: return 'addi+branch'
        if ka == 'alu' and (f3a in (2, 3) or (opa == 0x13 and f3a in (4, 7))): return 'cmp+branch'
        if ka == 'load': return 'load+branch'
        return 'alu+branch'
    if ka == 'alu' and kb in ('load', 'store') and (opa == 0x33 and f3a == 0) and fields(b)[3] == rd: return 'add+mem'
    if ka == 'load' and kb in ('alu',): return 'load+alu'
    if ka == 'alu' and kb == 'alu': return 'alu+alu'
    return None

def main(path, cls_want):
    rows = {}
    for line in open(os.path.join(path, 'pcs.txt')):
        if line.startswith('#'): continue
        p = line.split()
        cls, pc, w, ex = int(p[0]), int(p[2], 16), int(p[3], 16), int(p[4])
        if cls != cls_want: continue
        if pc in rows: rows[pc] = (w, rows[pc][1] + ex)
        else: rows[pc] = (w, ex)
    N = sum(e for w, e in rows.values())
    if N == 0: return None
    mix = collections.Counter()
    taken = 0
    for pc, (w, e) in rows.items():
        k = kind(w); mix[k] += e
        if k in ('jal', 'jalr'): taken += e
        elif k == 'branch':
            nxt = rows.get(pc + 4, (0, 0))[1]
            taken += max(0, e - nxt)
    # fusion: greedy along the code, absorbed = min(count a, count b), each instruction used once
    fus = collections.Counter(); used = set()
    for pc in sorted(rows):
        if pc in used or pc + 4 not in rows: continue
        a, ea = rows[pc]; b, eb = rows[pc + 4]
        pk = pair_kind(a, b)
        if pk:
            fus[pk] += min(ea, eb); used.add(pc); used.add(pc + 4)
    # footprint
    cnts = sorted((e for w, e in rows.values()), reverse=True)
    def cover(f):
        s = 0
        for i, c in enumerate(cnts):
            s += c
            if s >= f * N: return i + 1
        return len(cnts)
    tex = collections.Counter()
    for pc, (w, e) in rows.items(): tex[pc >> 4] += e
    tc = sorted(tex.values(), reverse=True)
    def tcover(f):
        s = 0
        for i, c in enumerate(tc):
            s += c
            if s >= f * N: return i + 1
        return len(tc)
    return dict(N=N, mix={k: v / N for k, v in mix.items()}, run=N / max(taken, 1), taken=taken / N,
                fus={k: v / N for k, v in fus.items()}, distinct=len(rows),
                c90=cover(.9), c99=cover(.99), c999=cover(.999), t90=tcover(.9), t99=tcover(.99), t999=tcover(.999))

if __name__ == '__main__':
    import json
    out = {}
    for arg in sys.argv[1:]:
        path, cls = arg.rsplit(':', 1)
        name = os.path.basename(path).replace('.prof', '') + ('' if cls == '0' else ':w')
        r = main(path, int(cls))
        if r: out[name] = r
    json.dump(out, open('model.json', 'w'), indent=1)
    keys = ['alu', 'load', 'store', 'branch', 'jal', 'jalr', 'lui', 'auipc', 'float', 'mul', 'amo', 'other']
    print('%-18s %8s %5s %5s ' % ('part', 'M ins', 'run', 'tkn%') + ' '.join('%6s' % k for k in keys))
    for n, r in out.items():
        print('%-18s %8.1f %5.2f %5.1f ' % (n, r['N'] / 1e6, r['run'], 100 * r['taken']) + ' '.join('%6.1f' % (100 * r['mix'].get(k, 0)) for k in keys))
    fk = ['lui+use', 'auipc+use', 'li+branch', 'addi+branch', 'cmp+branch', 'load+branch', 'alu+branch', 'add+mem', 'load+alu', 'alu+alu']
    print('\nabsorbed (% of instructions)')
    print('%-18s ' % 'part' + ' '.join('%11s' % k for k in fk) + '  first5  all')
    for n, r in out.items():
        f5 = sum(r['fus'].get(k, 0) for k in fk[:5])
        print('%-18s ' % n + ' '.join('%11.2f' % (100 * r['fus'].get(k, 0)) for k in fk) + '  %6.2f %5.2f' % (100 * f5, 100 * sum(r['fus'].values())))
    print('\nfootprint: instructions (texels) covering 90 / 99 / 99.9% of the run')
    for n, r in out.items():
        print('%-18s distinct %7d  ins %6d %6d %6d   texels %6d %6d %6d' % (n, r['distinct'], r['c90'], r['c99'], r['c999'], r['t90'], r['t99'], r['t999']))
