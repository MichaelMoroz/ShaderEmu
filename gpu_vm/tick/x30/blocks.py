"""Guest block lengths (x30, phase 1b): bundles run between branch decisions, from a gvm_cpu profile (GVM_PROF_CORES).

    python gpu_vm/tick/x30/blocks.py PROF [PROF...]

A decision is a bundle whose control word can change where the next bundle is: a conditional branch, addbnz, a near j
(beq r0, r0), and cc 0's jr, callr, call, far j and eret (not the fused add, not bne r0, r0). A run starts where a bundle's
runs exceed the runs of the bundle before it that falls into it (a target, a return point) and ends at the next decision.
Printed for core 0 and the workers: bundles, decisions, mean bundles a run (bundles over decisions), the share of bundles
in runs of 1, 2, 3-4, 5-8, 9+ bundles, and the share of bundles with a load or store."""
import sys
sys.path.insert(0, __import__('os').path.join(__import__('os').path.dirname(__file__), '..', 'gamebench_rv'))
from hotcode_lib import read_prof


def decision(ctl, uncond_j=True):
    cc = (ctl >> 7) & 15
    if cc == 0:
        sub = (ctl >> 11) & 7
        return sub in (2, 3, 4, 5, 6)
    ra, rb = (ctl >> 11) & 63, ctl >> 26
    if cc == 5 and ra == 0 and rb == 0:
        return False   # bne r0, r0: falls through
    if cc == 4 and ra == 0 and rb == 0:
        return uncond_j  # a near j
    return True


def mem(w):
    return ((w >> 12) & 255) >> 6 == 2


def analyse(rows, side, uncond_j=True):
    runs = {pc: (r[1] if side == 0 else r[2]) for pc, r in rows.items()}
    words = {pc: r[0] for pc, r in rows.items()}
    pcs = sorted(pc for pc in runs if runs[pc] > 0)
    total = sum(runs[p] for p in pcs)
    dec = {p: decision(words[p][3], uncond_j) for p in pcs}
    ndec = sum(runs[p] for p in pcs if dec[p])
    memb = sum(runs[p] for p in pcs if mem(words[p][0]) or mem(words[p][1]))
    hist = {}
    pset = set(pcs)
    nextdec = {}
    for p in reversed(pcs):   # the next decision at or after p along straight code
        if dec[p]:
            nextdec[p] = p
        elif p + 16 in pset and (p + 16) in nextdec:
            nextdec[p] = nextdec[p + 16]
        else:
            nextdec[p] = p   # the run leaves the profile's code (a trap, a pass end): ends here
    for p in pcs:
        before = runs.get(p - 16, 0) if (p - 16) in pset and not dec[p - 16] else 0
        starts = max(0, runs[p] - before)
        if starts:
            L = (nextdec[p] - p) // 16 + 1
            hist[L] = hist.get(L, 0) + starts * L
    bucket = lambda L: '1' if L == 1 else '2' if L == 2 else '3-4' if L <= 4 else '5-8' if L <= 8 else '9+'
    sh = {}
    for L, b in hist.items():
        sh[bucket(L)] = sh.get(bucket(L), 0) + b
    hb = sum(hist.values()) or 1
    return total, ndec, total / max(ndec, 1), {k: sh.get(k, 0) / hb for k in ('1', '2', '3-4', '5-8', '9+')}, memb / max(total, 1), hist


if __name__ == '__main__':
    for fn in sys.argv[1:]:
        rows = read_prof(fn)
        for side, name in ((0, 'core 0'), (1, 'workers')):
            for uj in (True, False):
                t, d, m, sh, mf, _ = analyse(rows, side, uj)
                if not t:
                    continue
                print('%s %-8s %s bundles %11d decisions %10d  %.2f a run  in runs of 1 %.0f%%, 2 %.0f%%, 3-4 %.0f%%, 5-8 %.0f%%, 9+ %.0f%%;  memory %.0f%%' % (
                    fn, name, 'j counted' if uj else 'j not   ', t, d, m, 100 * sh['1'], 100 * sh['2'], 100 * sh['3-4'], 100 * sh['5-8'], 100 * sh['9+'], 100 * mf))
