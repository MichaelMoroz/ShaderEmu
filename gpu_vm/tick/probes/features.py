"""What other ISA features would save in the programs' executed code (the ISA search, part 3), from gvm_cpu profiles
(repack.py's input). Within each straight run (repack.py's), weighted by how often each bundle ran:
  operands      where each register an op reads was written: by the op before (a belt or accumulator's slot 0), 2, 3-4,
                5-8 ops before in the run, or earlier (another run, or the run's start); r0 apart. And how the reads
                and writes spread over the registers (the hottest 4, 8 and 16: a split file's fast part)
  texel pairs   a load (store) of a word or less whose base register and 16-byte group (offsets // 16, the base taken
                as 16-aligned) another load (store) of the run shares, the base not written between: what ld4/st4 of a
                texel would merge (sp's apart: sp is 16-aligned, others may not be)
  post-inc      an addi rX, rX, imm within two ops of a memory op based on rX in the run (the op a post-increment saves;
                the control word's fused add is counted where the compiler used it)
  short skips   control words that branch forward 1-4 bundles (predication's candidates), of all decisions
    python gpu_vm/tick/probes/features.py PROF [PROF ...] [--workers]
"""
import argparse, collections

NONE_CC = 5


def leaves(ctl):
    cc = (ctl >> 7) & 15
    return not (cc == NONE_CC and ((ctl >> 11) & 63) == 0 and (ctl >> 26) == 0)


def decode(w, long_):
    """(kind, written, read regs, base, imm, width) of an op word; kind 'ld', 'st', 'ldx', 'stx', 'alu', 'float', 'x'"""
    o = (w >> 12) & 255
    c = o >> 6
    ra, rd, rb, rc = w & 63, (w >> 6) & 63, w >> 26, (w >> 20) & 63
    imm = None if long_ else ((w >> 20) ^ 0x800) - 0x800 if (w >> 31) or True else 0
    if c == 2:
        mode, wd = (o >> 3) & 7, o & 7
        if mode == 0:
            return 'ld', rd, [ra], ra, imm, wd
        if mode == 1:
            return 'st', None, [ra, rd], ra, imm, wd
        if mode == 2:
            return 'ldx', rd, [ra, rb], ra, None, wd
        if mode == 3:
            return 'stx', None, [ra, rb, rd], ra, None, wd
        return 'amo', rd, [ra, rb], ra, None, wd
    if c == 1:
        f = o & 31
        if o & 32:
            return ('addi' if f == 0 else 'alui'), rd, [ra], ra, imm, f
        if f in (24, 25, 26, 27, 28, 29):
            return 'alu', rd, [ra, rb, rc], None, None, f
        return 'alu', rd, [ra, rb], None, None, f
    if c == 3:
        return 'float', rd, [ra, rb] if not (o & 32) else [ra], None, None, None
    return 'x', rd, [ra, rb], None, None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prof', nargs='+')
    ap.add_argument('--workers', action='store_true')
    a = ap.parse_args()
    for path in a.prof:
        b = {}
        for line in open(path):
            if line[0] == '#':
                continue
            f = line.split()
            pc, ctl = int(f[0], 16), int(f[4], 16)
            cnt = int(f[6] if a.workers else f[5])
            n = (ctl >> 3) & 3
            ops = [decode(w, (ctl >> (5 + k)) & 1) for k, w in enumerate((int(f[1], 16), int(f[2], 16))[:n])]
            b[pc] = (ctl, cnt, ops)
        T = collections.Counter()
        dist = collections.Counter()
        rreads, rwrites = collections.Counter(), collections.Counter()
        prev = None
        packs = collections.Counter()
        run = []   # (is memory, long, merged away, post-inc away) of the run's ops, and its entries
        def close():
            if not run or not run_ent[0]:
                return
            for name, keep, bit in (('as is', lambda o: True, 0), ('texel merged', lambda o: not o[2], 1), ('merged with the next memory op (ld2)', lambda o: not (o[2] & 2), 2), ('and in register order (ldd, ldq)', lambda o: not (o[2] & 4), 4), ('and post-inc', lambda o: not o[2] and not o[3], 1)):
                seq = [o for o in run if keep(o)]
                for W, M in ((2, 1), (2, 2), (3, 2)):
                    steps, n, m, c = 1, 0, 0, 0
                    for mm, L, *_ in seq:
                        if n + 1 > W or m + mm > M or c + L > 1:
                            steps, n, m, c = steps + 1, 0, 0, 0
                        n, m, c = n + 1, m + mm, c + L
                    packs[(name, W, M)] += run_ent[0] * steps
                steps, cur = 1, []   # E4's rules (gvm_asm.h fits2): op 1 lite, or two loads, or a store then a load or store; op 1 reads nothing op 0 writes
                for o in seq:
                    if cur:
                        o0 = cur[0]
                        ok = len(cur) == 1 and not (o0[1] and o[1]) and not (o0[6] and o0[6] in o[7] and o[0])
                        ok = ok and (o[4] or (o0[5] == 'st' and o[5] in ('ld', 'st')) or (o0[5] == 'ld' and o[5] == 'ld'))
                        if ok:
                            cur.append(o)
                            continue
                        steps += 1
                    cur = [o]
                packs[(name, 0, 0)] += run_ent[0] * steps
                steps, cur = 1, []   # the same, a merged pair only as op 0 with a lite op 1 (the tick's paths 3 and 5)
                for o in seq:
                    if cur:
                        o0 = cur[0]
                        ok = len(cur) == 1 and not (o0[1] and o[1]) and not (o0[6] and o0[6] in o[7] and o[0]) and not (o[8] & bit)
                        ok = ok and (o[4] or (not (o0[8] & bit) and ((o0[5] == 'st' and o[5] in ('ld', 'st')) or (o0[5] == 'ld' and o[5] == 'ld'))))
                        if ok:
                            cur.append(o)
                            continue
                        steps += 1
                    cur = [o]
                packs[(name, 1, 0)] += run_ent[0] * steps
        def relax(seq, anyalu, fwd, anymem):
            steps, cur = 1, []
            for o in seq:
                if cur:
                    o0 = cur[0]
                    ok = len(cur) == 1 and not (o0[1] and o[1]) and (fwd or not (o0[6] and o0[6] in o[7]))
                    alu = o[5] in ('alu', 'alui', 'addi')
                    ok = ok and (o[4] or (anyalu and alu) or (anymem and o0[0] and o[0]) or (o0[5] == 'st' and o[5] in ('ld', 'st')) or (o0[5] == 'ld' and o[5] == 'ld'))
                    if ok:
                        cur.append(o)
                        continue
                    steps += 1
                cur = [o]
            return steps
        RELAX = [('op 1 any integer op', 1, 0, 0), ('op 1 reads op 0', 0, 1, 0), ('any two memory ops', 0, 0, 1), ('all three', 1, 1, 1)]
        def close2():
            if run and run_ent[0]:
                for nm, x, y, z in RELAX:
                    packs[('relax', nm)] += run_ent[0] * relax(run, x, y, z)
        run_ent = [0]
        for pc in sorted(b):
            ctl, cnt, ops = b[pc]
            if prev is None or pc != prev + 16 or cnt > b[prev][1]:
                hist = []   # the run's ops so far: (kind, written, read, base, imm, width)
                close(); close2(); run.clear(); run_ent[0] = cnt
            prev = pc
            for i, op in enumerate(ops):
                k, wr, rd, base, imm, wd = op
                merged = postinc = False
                T['ops'] += cnt
                # operands
                for r in rd:
                    if r == 0:
                        continue
                    T['reads'] += cnt
                    rreads[r] += cnt
                    back = None
                    for j in range(len(hist) - 1, -1, -1):
                        if hist[j][1] == r:
                            back = len(hist) - j
                            break
                    dist['1' if back == 1 else '2' if back == 2 else '3-4' if back and back <= 4 else
                         '5-8' if back and back <= 8 else 'earlier' if back is None else '>8'] += cnt
                if wr:
                    rwrites[wr] += cnt
                    T['writes'] += cnt
                # texel pairs
                if k in ('ld', 'st') and imm is not None and wd in (0, 2, 3, 6, 7):
                    for j in range(len(hist) - 1, -1, -1):
                        h = hist[j]
                        if h[1] == base:
                            break
                        if h[0] == k and h[3] == base and h[4] is not None and h[5] in (0, 2, 3, 6, 7) and h[4] // 16 == imm // 16:
                            T['pair ' + k + (' sp' if base == 2 else '')] += cnt
                            cls = 'sp' if base == 2 else 'adj' if abs(h[4] - imm) == 4 else 'other'
                            T['diff%d %s' % (abs(h[4] - imm), k)] += cnt
                            nxt = all(hh[0] not in ('ld', 'st', 'ldx', 'stx', 'amo') for hh in hist[j + 1:])
                            cons = wr is not None and h[1] is not None and abs(wr - h[1]) == abs(h[4] - imm) // 4 or (k == 'st' and abs(rd[1] - h[2][1]) == abs(h[4] - imm) // 4)
                            T['cls ' + k + ' ' + cls] += cnt
                            T['cls ' + k + ' ' + cls + (' next' if nxt else ' far')] += cnt
                            if cons:
                                T['cls ' + k + ' ' + cls + ' consec'] += cnt
                            merged = 1 | (2 if nxt else 0) | (4 if nxt and cons else 0)
                            rj = len(run) - len(hist) + j   # (the partner's place in the run)
                            run[rj] = run[rj][:8] + (run[rj][8] | merged,)
                            break
                    T[k] += cnt
                # post-increment
                if k == 'addi' and wr == base and wr:
                    near = [h for h in hist[-2:] if h[0] in ('ld', 'st', 'ldx', 'stx') and h[3] == wr]
                    if near:
                        T['postinc'] += cnt
                        postinc = True
                    T['addi self'] += cnt
                if k in ('ld', 'st') and base:
                    near = [h for h in hist[-2:] if h[0] == 'addi' and h[1] == base and h[3] == base]
                    if near:
                        T['preinc'] += cnt
                hist.append(op)
                lite = (k in ('alu', 'alui') and (wd <= 7 or 16 <= wd <= 18)) or k == 'addi'
                run.append((1 if k in ('ld', 'st', 'ldx', 'stx') else 0, (ctl >> (5 + i)) & 1, merged, postinc, lite,
                            'ld' if k in ('ld', 'ldx') else 'st' if k in ('st', 'stx') else k, wr, set(rd), 0))
            cc = (ctl >> 7) & 15
            if leaves(ctl):
                T['decisions'] += cnt
                if cc != 0 and cc != 4 and cc != 1:
                    off = ((ctl << 6) & 0xffffffff) >> 23
                    off = off - 512 if off & 256 else off
                    T['cond'] += cnt
                    if 1 <= off <= 5:
                        T['skip<=4'] += cnt
                        T['skip%d' % (off - 1)] += cnt
                    if off < 0:
                        T['back'] += cnt
                prev = None
                close(); close2(); run.clear(); run_ent[0] = 0
        close(); close2()
        ops = T['ops']
        print(f'{path} ({"workers" if a.workers else "core 0"}): {ops:,} ops, {T["reads"]:,} register reads (r0 apart), {T["writes"]:,} writes')
        print('| a read\'s value written | share of reads |\n|---|---|')
        for k in ('1', '2', '3-4', '5-8', '>8', 'earlier'):
            print(f'| {k} ops before{" (or before the run)" if k == "earlier" else ""} | {100 * dist[k] / T["reads"]:.1f}% |')
        tr = sum(rreads.values()); tw = sum(rwrites.values())
        top = [r for r, _ in rreads.most_common()]
        for K in (4, 8, 16):
            s = set(top[:K])
            print(f'| the {K} most read registers ({",".join("r%d" % r for r in top[:K])}) | reads {100 * sum(rreads[r] for r in s) / tr:.1f}%, writes {100 * sum(rwrites[r] for r in s) / tw:.1f}% |')
        print('| feature | ops it saves, share of all ops |\n|---|---|')
        for k in ('ld', 'st'):
            print(f'| a {k} merged with another of its texel (sp\'s) | {100 * T["pair " + k] / ops:.1f}% ({100 * T["pair " + k + " sp"] / ops:.1f}%) of {100 * T[k] / ops:.1f}% |')
        print(f'| post-increment (addi rX after a memory op on rX) | {100 * T["postinc"] / ops:.1f}% (pre-: {100 * T["preinc"] / ops:.1f}%), of addi rX, rX {100 * T["addi self"] / ops:.1f}% |')
        print(f'| conditional branches 1-4 bundles forward | {100 * T["skip<=4"] / max(1, T["cond"]):.1f}% of conditional, {100 * T["skip<=4"] / T["decisions"]:.1f}% of decisions '
              f'(over 1: {100 * T["skip0"] / T["decisions"]:.1f}, 2: {100 * T["skip1"] / T["decisions"]:.1f}); backward {100 * T["back"] / T["decisions"]:.1f}% |')
        for k in ('ld', 'st'):
            print(f'| {k} merges: ' + ', '.join(f'{c} {100 * T["cls " + k + " " + c] / ops:.1f}% (the next memory op {100 * T["cls " + k + " " + c + " next"] / ops:.1f}, registers in order {100 * T["cls " + k + " " + c + " consec"] / ops:.1f})' for c in ('sp', 'adj', 'other')) + ' |')
        print('| merges by distance | ' + ', '.join(f'{k} {100 * T[k] / ops:.1f}%' for k in sorted(T) if k.startswith('diff')) + ' |')
        print('| steps (greedy, a constant a step) | W2 M1 | W2 M2 | W3 M2 |\n|---|---|---|---|')
        print(f'| (E4 rules as is: {packs[("as is", 0, 0)]:,} steps; the bundles that ran: {sum(v[1] for v in b.values()):,}) |')
        for name in ('as is', 'texel merged', 'merged with the next memory op (ld2)', 'and in register order (ldd, ldq)', 'and post-inc'):
            print(f'| {name} | ' + ' | '.join(f'{packs[(name, W, M)] / packs[("as is", W, M)]:.3f}' for W, M in ((0, 0), (1, 0), (2, 1), (2, 2), (3, 2))) + ' |')
        print('| E4 with | steps |\n|---|---|')
        for nm, *_ in RELAX:
            print(f'| {nm} | {packs[("relax", nm)] / packs[("as is", 0, 0)]:.3f} |')
        print()


if __name__ == '__main__':
    main()
