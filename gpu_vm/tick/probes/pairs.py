"""Which ops feed which in the programs' executed code (the ISA search: candidates for fused ops), from a gvm_cpu
profile (repack.py's input): within each straight run, an op that reads a register the op before it in the run wrote,
counted by both ops' kinds, weighted by how often the bundle ran. Also each kind's share of all ops.

    python gpu_vm/tick/probes/pairs.py PROF [--top 30] [--workers]
"""
import argparse, collections

I = ['add', 'and', 'or', 'xor', 'sub', 'andn', 'orn', 'xnor', 'slt', 'sltu', 'min', 'minu', 'sge', 'sgeu', 'max', 'maxu',
     'sll', 'srl', 'sra', 'mul', 'ror', 'rol', 'seq', 'sne', 'shadd', 'sel', 'addcy', 'carry', 'subby', 'borrow', 'bfe', 'i31']
M = ['ld', 'st', 'ldx', 'stx', 'amo', 'amo2', 'fread', 'fwrite']
W = ['w', 'd', 'h', 'b', 'q', 'nc', 'hu', 'bu']


def kind(w):
    o = (w >> 12) & 255
    c = o >> 6
    if c == 1:
        return I[o & 31] + ('i' if o & 32 else '')
    if c == 2:
        return M[(o >> 3) & 7] + W[o & 7]
    if c == 3:
        return 'float'
    return 'x%02x' % o


def regs(w):
    """(written, read): a store's rd is read"""
    o = (w >> 12) & 255
    rd, ra, rb = (w >> 6) & 63, w & 63, w >> 26
    c, imm = o >> 6, (o & 32) != 0 if (o >> 6) == 1 else False
    if c == 2 and ((o >> 3) & 7) in (1, 3):
        return None, {ra, rd} | ({rb} if ((o >> 3) & 7) == 3 else set())
    rd_ = rd if rd else None
    if c == 2:
        return rd_, {ra} | ({rb} if ((o >> 3) & 7) == 2 else set())
    if c == 1 and o & 32:
        return rd_, {ra}
    return rd_, {ra, rb}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prof')
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--workers', action='store_true')
    a = ap.parse_args()
    rows = []
    for line in open(a.prof):
        if line[0] == '#':
            continue
        f = line.split()
        ctl = int(f[4], 16)
        rows.append((int(f[0], 16), [int(f[1], 16), int(f[2], 16)][:(ctl >> 3) & 3], ctl, int(f[6] if a.workers else f[5])))
    rows.sort()
    kinds, pairs, ctlfeed = collections.Counter(), collections.Counter(), collections.Counter()
    total = 0
    prev_pc, last = None, None   # last: (kind, register written) of the run's last op
    for pc, ops, ctl, cnt in rows:
        if prev_pc is None or pc != prev_pc + 16:
            last = None
        for w in ops:
            k = kind(w)
            kinds[k] += cnt; total += cnt
            wr, rd = regs(w)
            if last and last[1] is not None and last[1] in rd and last[1] != 0:
                pairs[(last[0], k)] += cnt
            last = (k, wr)
        cc = (ctl >> 7) & 15
        if cc != 5 and last and last[1] is not None and last[1] in ((ctl >> 11) & 63, ctl >> 26) and cc != 0:
            ctlfeed[last[0]] += cnt
        if not (cc == 5 and ((ctl >> 11) & 63) == 0 and (ctl >> 26) == 0):
            last = None
        prev_pc = pc
    print(f'{a.prof}: {total:,} ops')
    print('| op kind | share of ops |\n|---|---|')
    for k, v in kinds.most_common(a.top):
        print(f'| {k} | {100 * v / total:.1f}% |')
    print('\n| producer -> consumer (the next op reads what the op before wrote) | share of ops |\n|---|---|')
    for (p, c), v in pairs.most_common(a.top):
        print(f'| {p} -> {c} | {100 * v / total:.1f}% |')
    print('\n| the op before a branch that reads its result | share of ops |\n|---|---|')
    for k, v in ctlfeed.most_common(10):
        print(f'| {k} -> branch | {100 * v / total:.1f}% |')


if __name__ == '__main__':
    main()
