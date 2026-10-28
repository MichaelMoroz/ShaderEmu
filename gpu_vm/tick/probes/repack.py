"""The steps a program's executed code takes under other step shapes (the ISA search, GPU-VM's RESULTS.md), from a
gvm_cpu profile (--profile: a line a bundle, `pc w0 w1 w2 ctl core0 workers ...`, the counts of core 0 and the workers).

    python gpu_vm/tick/probes/repack.py PROF [PROF ...] [--workers]

A straight run is the bundles from one that is entered (a branch's target, or after a bundle whose control may leave)
to the next control that may leave: its ops are what a shape packs. For each shape the steps over all runs, weighted by
how often each run is entered (its first bundle's count; a run left early by a trap or a pass's end is counted whole):
  E4 as is        the bundles themselves
  W1              one op a step, the control word in the last op's step (thin)
  W2 ideal        ceil(ops / 2) a run: E4's packing with no dependence or pairing rule (how far E4 is from ideal)
  W3, W4 ideal    ceil(ops / 3), ceil(ops / 4)
  Wk real         ideal times E4's own ratio of bundles to its ideal (the same losses to dependences and pairing)
  decisions       runs entered: a block ISA's one decision a run (blocks of up to K bundles: ceil(bundles / K) each)
"""
import argparse, collections, math, sys

NONE_CC = 5   # isa.h: bne r0, r0 (GVM_CTRL_NONE): falls through


def leaves(ctl):
    """a control word that may leave the straight line (anything but bne r0, r0)"""
    cc = (ctl >> 7) & 15
    return not (cc == NONE_CC and ((ctl >> 11) & 63) == 0 and (ctl >> 26) == 0)


def is_mem(w):
    o = (w >> 12) & 255
    return (o >> 6) == 2 and ((o >> 3) & 7) <= 3


SHAPES = [(2, 1, 1), (2, 2, 1), (3, 1, 1), (3, 2, 1), (3, 3, 2), (4, 1, 1), (4, 2, 1), (4, 2, 2), (4, 4, 2), (6, 2, 2), (8, 2, 2), (8, 4, 4)]


def pack(seq, W, M, C):
    """steps of a run's ops in order: at most W ops, M memory ops and C long constants a step (at least one step: the control)"""
    steps, n, m, c = 1, 0, 0, 0
    for w, L in seq:
        mm = 1 if is_mem(w) else 0
        if n + 1 > W or m + mm > M or c + L > C:
            steps, n, m, c = steps + 1, 0, 0, 0
        n, m, c = n + 1, m + mm, c + L
    return steps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prof', nargs='+')
    ap.add_argument('--workers', action='store_true', help="the workers' counts, not core 0's")
    a = ap.parse_args()
    for path in a.prof:
        b = {}
        for line in open(path):
            if line[0] == '#':
                continue
            f = line.split()
            pc, ctl = int(f[0], 16), int(f[4], 16)
            cnt = int(f[6] if a.workers else f[5])
            w0, w1 = int(f[1], 16), int(f[2], 16)
            n = (ctl >> 3) & 3
            ops = [(w, (ctl >> (5 + k)) & 1) for k, w in enumerate((w0, w1)[:n])]
            b[pc] = (n, ctl, cnt, ops)
        pcs = sorted(b)
        bundles = sum(v[2] for v in b.values())
        # a run starts at a bundle whose count exceeds what flowed into it from the bundle before (entered by a jump),
        # or after a bundle that may leave; it ends at a bundle that may leave
        tot = collections.Counter()
        prev = None
        run = []
        def close(run):
            if not run:
                return
            n_ops = sum(b[p][0] for p in run)
            k = len(run)
            ent = b[run[0]][2]
            tot['E4'] += sum(b[p][2] for p in run)
            tot['W1'] += sum(b[p][2] * max(b[p][0], 1) for p in run)
            for w in (2, 3, 4):
                tot['W%d ideal' % w] += ent * max(1, math.ceil(n_ops / w))
            tot['decisions'] += ent
            for K in (2, 4, 8, 16):
                tot['blocks K%d' % K] += ent * math.ceil(k / K)
            tot['ops'] += sum(b[p][2] * b[p][0] for p in run)
            seq = [o for p in run for o in b[p][3]]
            for W, M, C in SHAPES:
                tot[(W, M, C)] += ent * pack(seq, W, M, C)
        for p in pcs:
            n, ctl, c, _ = b[p]
            if run and (prev is None or p != prev + 16 or c > b[prev][2]):
                close(run); run = []
            run.append(p)
            prev = p
            if leaves(ctl):
                close(run); run = []
                prev = None
        close(run)
        r = tot['E4'] / tot['W2 ideal']
        print(f'{path}: {bundles:,} bundles ({"workers" if a.workers else "core 0"}), {tot["ops"]:,} ops, {tot["decisions"]:,} runs entered '
              f'({tot["E4"] / max(1, tot["decisions"]):.2f} bundles a run)')
        print('| shape | steps | against E4 |\n|---|---|---|')
        rows = [('E4 as is', tot['E4']), ('W1 (one op a step)', tot['W1']), ('W2 ideal', tot['W2 ideal']),
                ('W3 ideal', tot['W3 ideal']), ('W4 ideal', tot['W4 ideal']),
                ('W3 real (E4\'s losses)', tot['W3 ideal'] * r), ('W4 real (E4\'s losses)', tot['W4 ideal'] * r),
                ('decisions (runs entered)', tot['decisions'])] + [('blocks of up to %d bundles' % K, tot['blocks K%d' % K]) for K in (2, 4, 8, 16)]
        rows += [('W%d, %s memory, %d constant%s (greedy)' % (W, M if M < 9 else 'any', C, 's' if C > 1 else ''), tot[(W, M, C)]) for W, M, C in SHAPES]
        for name, v in rows:
            print(f'| {name} | {v:,.0f} | {v / tot["E4"]:.3f} |')


if __name__ == '__main__':
    main()
