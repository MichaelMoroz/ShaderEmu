"""The kernel's part of every part's profile (prof_parts.py): core 0's instructions and the write cache's entries by
kernel function, a part's top ones and the sum over all parts (each part's share of its own core 0, averaged).

    python tools/rv_compare/prof_kernel.py [PART ...] [--top 12]"""
import argparse, collections, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SE = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(SE, 'tools'))
sys.path.insert(0, HERE)
import prof  # noqa: E402
from parts import PARTS  # noqa: E402
from prof_summary import KERNEL  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('parts', nargs='*')
    ap.add_argument('--dir', default=os.path.join(SE, 'build', 'prof_parts'))
    ap.add_argument('--top', type=int, default=12)
    a = ap.parse_args()
    K = prof.Elf(KERNEL, name='vmlinux')
    avg_i, avg_t = collections.Counter(), collections.Counter()
    n = 0
    for part in a.parts or list(PARTS):
        out = os.path.join(a.dir, f'{part}.prof')
        if not os.path.exists(os.path.join(out, 'pcs.txt')):
            continue
        P = prof.Profile(out)
        P.place([K], quiet=True)
        sel = P.cls == 0
        tot_i, tot_t = int(P.exec[sel].sum()), int(P.newtex[sel].sum())
        by_i, by_t = collections.Counter(), collections.Counter()
        for pc, ex, nt in zip(P.pc[sel], P.exec[sel], P.newtex[sel]):
            if not (0x80400000 <= pc < 0x80800000):
                continue
            f = P.name(int(pc), False)
            by_i[f] += int(ex)
            by_t[f] += int(nt)
        ki, kt = sum(by_i.values()), sum(by_t.values())
        row = {'part': part, 'kernel_pct': round(100 * ki / max(tot_i, 1), 1), 'kernel_tex_pct': round(100 * kt / max(tot_t, 1), 1),
               'top': [(f, round(100 * v / max(tot_i, 1), 2)) for f, v in by_i.most_common(a.top)],
               'top_tex': [(f, round(100 * v / max(tot_t, 1), 2)) for f, v in by_t.most_common(a.top)]}
        print(json.dumps(row), flush=True)
        for f, v in by_i.items():
            avg_i[f] += v / max(tot_i, 1)
        for f, v in by_t.items():
            avg_t[f] += v / max(tot_t, 1)
        n += 1
    print(json.dumps({'part': 'ALL (mean share of core 0)', 'top': [(f, round(100 * v / n, 2)) for f, v in avg_i.most_common(30)],
                      'top_tex': [(f, round(100 * v / n, 2)) for f, v in avg_t.most_common(30)]}))


if __name__ == '__main__':
    main()
