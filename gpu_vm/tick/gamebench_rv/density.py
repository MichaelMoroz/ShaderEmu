"""Where a game's gpu_vm bundles go by pattern (GAMEBENCH.md, "Density"): a gvm_cpu --profile of a game, its ELF's symbols
and its load base, and optionally the RISC-V build's symbols for the same functions' sizes. Prints the workers' (or core
0's) bundles by pattern (no op, one op, control only, calls and returns, globals by lpc, byte and halfword access, soft
double and 64-bit helpers, time conversions, memcpy) and the hottest functions with their static RISC-V instructions a
bundle.

    python gpu_vm/tick/gamebench_rv/density.py PROF GVM.nm BASE [RV.nm]   (GVM.nm: llvm-nm -n -S, or fnmap.py's map of the profiled build, BASE 0) [--core0] [--top N]
"""
import bisect, sys
from collections import Counter, defaultdict

SOFT_DOUBLE = {'__extendsfdf2', '__truncdfsf2', '__lshrdi3', '__ashldi3', '__adddf3', '__subdf3', '__muldf3', '__divdf3', '__gtdf2', '__ltdf2', '__gedf2', '__ledf2', '__eqdf2',
               '__nedf2', '__unorddf2', '__extendsfdf2', '__truncdfsf2', '__floatsidf', '__fixdfsi', '__floatunsidf',
               '__fixunsdfsi', '__floatdidf'}
INT64 = {'__divdi3', '__udivdi3', '__moddi3', '__umoddi3', '__muldi3', '__ashrdi3', '__udivmoddi4'}
TIME = {'__secs_to_tm', '__year_to_secs', '__map_file', '__tm_to_secs', '__month_to_secs', 'localtime', 'localtime_r',
        '__localtime_r', 'mktime', '__tz_set', 'do_tzset', 'getname', 'strtoul', 'strtol', '__secs_to_zone', 'time',
        '__clock_gettime', 'clock_gettime', 'gettimeofday'}


def load_nm(fn):
    d = {}
    for l in open(fn):
        p = l.split()
        if len(p) >= 3 and p[-2] in 'tT':
            d[p[-1]] = (int(p[0], 16), int(p[1], 16) if len(p) == 4 else 0)
    return d


def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    core0 = '--core0' in sys.argv
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 30
    prof, gnm, base = a[0], a[1], int(a[2], 0)
    rv = load_nm(a[3]) if len(a) > 3 else {}
    g = load_nm(gnm)
    syms = sorted((v[0], n) for n, v in g.items())
    addrs = [s[0] for s in syms]
    sizes = {n: v[1] for n, v in g.items()}
    pat = Counter()
    fn_b = Counter()
    fn_pat = defaultdict(Counter)
    tot = 0
    for l in open(prof):
        if l.startswith('#'):
            continue
        p = l.split()
        pc = int(p[0], 16)
        w = [int(x, 16) for x in p[1:5]]
        cnt = int(p[5]) if core0 else int(p[6])
        if not cnt:
            continue
        tot += cnt
        i = bisect.bisect_right(addrs, pc - base) - 1
        if pc < 0x100000:
            name = '(firmware, kernel)'
        elif i >= 0 and (not sizes[syms[i][1]] or pc - base < syms[i][0] + sizes[syms[i][1]]):
            name = syms[i][1]
        else:
            name = '(not mapped)'
        fn_b[name] += cnt
        ctl = w[3]
        n = (ctl >> 3) & 3
        cc = (ctl >> 7) & 15
        sub = (ctl >> 11) & 7   # (GVM_SUB: the special form's kind)
        ks = []
        for k in range(min(n, 2)):
            opc = (w[k] >> 12) & 255
            cls = opc >> 6
            if cls == 2:
                m, wd = (opc - 0x80) // 8, opc & 7
                ks.append(('load' if m in (0, 2) else 'store' if m in (1, 3) else 'amo') + ('8' if wd in (3, 7) else '16' if wd in (2, 6) else ''))
            elif cls == 3:
                ks.append('float')
            elif cls == 1:
                ks.append('int')
            elif opc == 0x0e:
                ks.append('lpc')
            else:
                ks.append('x')
        tags = []
        if n == 0:
            tags.append('bundle of no op')
        elif n == 1:
            tags.append('bundle of one op')
        if cc == 0 and sub in (6, 3):
            tags.append('call')
        if cc == 0 and sub == 2:
            tags.append('return or jr')
        for k in ks:
            if k == 'lpc':
                tags.append('lpc (a global or a constant\'s address)')
            if k.endswith('8') or k.endswith('16'):
                tags.append('byte or halfword access')
        if name in SOFT_DOUBLE:
            tags.append('soft double (libgcc)')
        if name in INT64:
            tags.append('64-bit integer helpers')
        if name in TIME:
            tags.append('time conversion, strtoul, clock')
        if name in ('memcpy', 'memset', 'memmove'):
            tags.append('memcpy, memset')
        if name.startswith('(firmware'):
            tags.append('firmware and kernel')
        if name == '(not mapped)':
            tags.append('(not mapped to a function)')
        for t in tags:
            pat[t] += cnt
            fn_pat[name][t] += cnt
    who = 'core 0' if core0 else 'workers'
    print(f'{who}: {tot} bundles')
    for t, v in pat.most_common():
        print(f'  {100 * v / tot:5.1f}%  {t}')
    print(f'\n{"function":34s} {"share":>6s} {"no-op":>6s} {"1 op":>6s} {"calls":>6s} {"gvm b":>6s} {"rv i":>6s} {"rv/b":>5s}')
    for nme, v in fn_b.most_common(top):
        fp = fn_pat[nme]
        gb = g.get(nme, (0, 0))[1] // 16
        ri = rv.get(nme, (0, 0))[1] // 4
        print(f'{nme[:34]:34s} {100 * v / tot:5.1f}% {100 * fp["bundle of no op"] / v:5.1f}% {100 * fp["bundle of one op"] / v:5.1f}% '
              f'{100 * fp["call"] / v:5.1f}% {gb:6d} {ri:6d} {ri / gb if gb and ri else 0:5.2f}')


if __name__ == '__main__':
    main()
