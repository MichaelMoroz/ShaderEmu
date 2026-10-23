"""A program's hot code from a gvm_cpu --profile: where its bundles run, by function, and how much code covers a share
of them (the code-size cliff: straight code over ~16 KB costs every bundle more, gpu_vm/IDEAS.md).

    python gpu_vm/tick/gamebench_rv/hotcode.py PROF ELF [--core0|--workers|--core N] [--top N] [--lines 128]

The profile's pcs are matched to the ELF's text by the bundles' words (the load base is found, not given). Prints the
program's share of the profile, the bytes of code (distinct bundles and distinct lines of --lines bytes) that cover
50/75/90/95/99% of its bundles, and the hottest functions with their size and the bytes of them that ran.
--core N needs a profile made with GVM_PROF_CORES=1.
"""
import bisect, os, sys
from collections import Counter, defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hotcode_lib import elf, find_base, read_prof


def main():
    args = list(sys.argv[1:])

    def opt(k, d=None):
        if k in args:
            i = args.index(k); v = args[i + 1]; del args[i:i + 2]; return v
        return d
    top = int(opt('--top', 30)); line = int(opt('--lines', 128)); core = opt('--core')
    which = 'all' if core is None else f'core {core}'
    for k in ('--core0', '--workers'):
        if k in args: which = k[2:]; args.remove(k)
    prof, elff = args[:2]
    (ta, tb), funcs = elf(elff)
    rows = read_prof(prof)

    def cnt(r):
        if core is not None: return r[3].get(int(core), 0) if int(core) else r[1]
        return r[1] if which == 'core0' else r[2] if which == 'workers' else r[1] + r[2]
    total = sum(cnt(r) for r in rows.values())
    base = find_base(rows, ta, tb)
    lo, hi = ta + base, ta + base + len(tb)
    mine = {pc - base: cnt(r) for pc, r in rows.items() if lo <= pc < hi and cnt(r)}
    own = sum(mine.values())
    print(f'base {base:#x}; this program {own:,} of {total:,} bundles ({100 * own / max(total, 1):.1f}%), {which}')

    def cover(keyf, unit):
        acc = Counter()
        for a, c in mine.items(): acc[keyf(a)] += c
        vs = sorted(acc.values(), reverse=True)
        out, s, i = [], 0, 0
        for q in (0.5, 0.75, 0.9, 0.95, 0.99, 1.0):
            while s < q * own - 1e-9 and i < len(vs): s += vs[i]; i += 1
            out.append(f'{int(q * 100)}% {i * unit / 1024:.1f}')
        return ', '.join(out)
    print('KB of bundles covering:', cover(lambda a: a, 16))
    print(f'KB of {line}-byte lines covering:', cover(lambda a: a // line, line))
    starts = [f[0] for f in funcs]
    fb, fran = Counter(), defaultdict(set)
    for a, c in mine.items():
        i = bisect.bisect_right(starts, a) - 1
        n = funcs[i][2] if i >= 0 and a < funcs[i][0] + funcs[i][1] else '?'
        fb[n] += c; fran[n].add(a)
    size = {f[2]: f[1] for f in funcs}
    print(f'{"function":40} {"bundles":>12} {"%":>6} {"cum%":>6} {"size":>7} {"ran":>7}')
    cum = 0
    for n, c in fb.most_common(top):
        cum += c
        print(f'{n[:40]:40} {c:12,} {100 * c / own:6.2f} {100 * cum / own:6.1f} {size.get(n, 0):7} {16 * len(fran[n]):7}')


main()
