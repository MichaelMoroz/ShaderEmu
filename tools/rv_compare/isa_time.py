"""A run's instructions weighed by what each kind costs the tick (docs/machine/tick-costs.md, "What a pass is made of":
115 ns an instruction that is no load or store, 450 a load, 370 a store, 2,700 one on the general path), from an
RVC_PROF run's pcs.txt: the modelled card time of core 0 and of the workers, for comparing builds of one program
(docs/plans/riscv-isa.md). --fast-fma: fmadd and its kin on the fast step (a float operation's cost).

    python tools/rv_compare/isa_time.py DIR [DIR ...] [--fast-fma]"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from isa_model import fields, is_fast   # noqa: E402

COST = {'alu': 115, 'load': 450, 'store': 370, 'general': 2700}


def kind(w, fast_fma, zb):
    op, rd, f3, rs1, rs2, f7 = fields(w)
    if fast_fma and op in (0x43, 0x47, 0x4b, 0x4f):
        return 'alu'
    if zb and op in (0x13, 0x33) and not is_fast(w):
        return 'alu'   # (Zba, Zbb, Zbs on the fast step's arithmetic: the tick's ZB)
    if not is_fast(w):
        return 'general'
    if op in (0x03, 0x07) or (op == 0x2f and (w >> 27) == 2):
        return 'load'
    if op in (0x23, 0x27) or op == 0x2f:
        return 'store'
    return 'alu'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--fast-fma', action='store_true')
    ap.add_argument('--zb', action='store_true')
    a = ap.parse_args()
    for d in a.dirs:
        t = [dict.fromkeys(COST, 0), dict.fromkeys(COST, 0)]
        for line in open(os.path.join(d, 'pcs.txt')):
            if line[0] == '#':
                continue
            p = line.split()
            t[int(p[0])][kind(int(p[3], 16), a.fast_fma, a.zb)] += int(p[4])
        out = []
        for c in (0, 1):
            ms = sum(t[c][k] * COST[k] for k in COST) / 1e6
            out.append(f"class {c}: {sum(t[c].values())} instructions ({', '.join(f'{k} {t[c][k]}' for k in COST)}), {ms:.1f} ms modelled")
        print(os.path.basename(d.rstrip('/\\')) + ': ' + '; '.join(out))


if __name__ == '__main__':
    sys.exit(main())
