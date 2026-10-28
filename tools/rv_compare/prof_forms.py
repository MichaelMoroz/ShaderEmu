"""The plan's slow forms in a part's profile (docs/plans/riscv-profile.md): the program's copies and fills by size of call
(musl's memcpy, memmove, memset: what a copy by the commit from 2 KB up would take), the kernel's memcpy (its byte
paths, its 64-byte word loop), the kernel's byte stores by function (get_user/put_user's byte paths), the soft floats'
callers, and large clears. One JSON line a part.

    python tools/rv_compare/prof_forms.py PART [PART ...]"""
import collections, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SE = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(SE, 'tools'))
sys.path.insert(0, HERE)
import prof  # noqa: E402
from prof_summary import ELFS, KERNEL, PROGRAMS, ALWAYS, SOFT  # noqa: E402

COPIES = ('memcpy', 'memmove', 'memset', '__memcpy_fwd', 'bzero')
BIG = 1200   # instructions a call: about 2 KB of an aligned musl memcpy (some 10 instructions a texel)


def forms(part, d):
    out = os.path.join(d, f'{part}.prof')
    P = prof.Profile(out)
    elfs = [prof.Elf(os.path.join(ELFS, n), name=n) for n in PROGRAMS.get(part, []) + ALWAYS if os.path.exists(os.path.join(ELFS, n))]
    elfs.append(prof.Elf(KERNEL, name='vmlinux'))
    P.place(elfs, quiet=True)
    sel = P.cls == 0
    tot_i, tot_t = int(P.exec[sel].sum()), int(P.newtex[sel].sum())
    by_i, by_t = collections.Counter(), collections.Counter()
    ksb = collections.Counter()
    kbyte = kword = 0
    mc = P.name(0x806b28c4, False)
    for pc, ex, nt, w in zip(P.pc[sel], P.exec[sel], P.newtex[sel], P.word[sel]):
        f = P.name(int(pc), False)
        by_i[f] += int(ex)
        by_t[f] += int(nt)
        if 0x80400000 <= pc < 0x80800000 and (int(w) & 0x707f) == 0x23:   # sb in the kernel
            ksb[f] += int(ex)
        if 0x806b2a40 <= pc < 0x806b2a54 or 0x806b2928 <= pc < 0x806b293c:
            kbyte += int(ex)
        if 0x806b2980 <= pc < 0x806b2a0c:
            kword += int(ex)
    r = {'part': part, 'core0': tot_i, 'newtex': tot_t}
    # the program's copies and fills, by the size of their calls (callers' mean)
    calls = collections.defaultdict(lambda: [0, 0])
    for k, a, b, n, incl in P.edges:
        if k:
            continue
        nb = P.name(b, False)
        if nb.split(':')[-1] in COPIES and not nb.startswith('vmlinux'):
            big = incl / max(n, 1) >= BIG
            calls[(nb, big)][0] += n
            calls[(nb, big)][1] += incl
    copy_i = sum(v for f, v in by_i.items() if f.split(':')[-1] in COPIES and not f.startswith('vmlinux'))
    copy_t = sum(v for f, v in by_t.items() if f.split(':')[-1] in COPIES and not f.startswith('vmlinux'))
    big_i = sum(v[1] for (f, big), v in calls.items() if big)
    r['prog_copies'] = {'instr_pct': round(100 * copy_i / max(tot_i, 1), 2), 'tex_pct': round(100 * copy_t / max(tot_t, 1), 2),
                        'big_calls_instr_pct': round(100 * big_i / max(tot_i, 1), 2),
                        'by': {f'{f}{" >=2KB" if big else ""}': (v[0], round(v[1] / max(v[0], 1))) for (f, big), v in calls.items()}}
    # the kernel's memcpy
    r['kernel_memcpy'] = {'instr_pct': round(100 * by_i.get(mc, 0) / max(tot_i, 1), 2), 'tex_pct': round(100 * by_t.get(mc, 0) / max(tot_t, 1), 2),
                          'byte_paths_pct': round(100 * kbyte / max(tot_i, 1), 2), 'word_loop_pct': round(100 * kword / max(tot_i, 1), 2)}
    r['kernel_sb'] = [(f, v) for f, v in ksb.most_common(6)]
    # soft floats: who calls them (inclusive, core 0)
    soft = collections.Counter()
    for k, a, b, n, incl in P.edges:
        if k == 0 and SOFT.match(P.name(b, False).split(':')[-1]):
            soft[P.name(a, False) if a else '(top)'] += incl
    r['soft_callers'] = [(f, round(100 * v / max(tot_i, 1), 2)) for f, v in soft.most_common(6)]
    # large fills: memset calls of 2,000 instructions or more a call, by caller
    big = collections.Counter()
    for k, a, b, n, incl in P.edges:
        if k == 0 and P.name(b, False).split(':')[-1] in ('memset', 'bzero') and incl / max(n, 1) >= 2000:
            big[(P.name(a, False) if a else '(top)', n)] += incl
    r['big_fills'] = [(f, n, round(100 * v / max(tot_i, 1), 2)) for (f, n), v in big.most_common(5)]
    return r


if __name__ == '__main__':
    d = os.path.join(SE, 'build', 'prof_parts')
    for p in sys.argv[1:]:
        try:
            print(json.dumps(forms(p, d)), flush=True)
        except Exception as e:
            print(json.dumps({'part': p, 'error': repr(e)}), flush=True)
