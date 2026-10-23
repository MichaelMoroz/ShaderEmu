"""Why a game's bundles carry one op (GAMEBENCH.md, "Density"): a gvm_cpu --profile, its ELF's symbols and load base.
For each executed bundle of one op (user code: firmware and the worker loop left out): the op's kind, and what keeps
the next bundle's first op out of it: it reads this op's result (a chain), both are memory ops a path cannot pair, the
next is not a lite op, a control word (call, return, branch) ends the bundle, or nothing (the scheduler left it).
    python gpu_vm/tick/gamebench_rv/oneop.py PROF GVM.nm BASE [--core0] [--fn NAME]
"""
import bisect, sys
from collections import Counter

def opinfo(w):
    opc = (w >> 12) & 255; cls = opc >> 6; rd = (w >> 6) & 63; ra = w & 63; rb = w >> 26; rc = (w >> 20) & 63
    iform = (opc >> 5) & 1
    if cls == 2:
        m, wd = (opc - 0x80) // 8, opc & 7
        k = 'load' if m in (0, 2) else 'store' if m in (1, 3) else 'amo'
        reads = {ra, rb} if m in (2, 3) else {ra}
        if k == 'store': reads |= {rd}
        return k + ('8/16' if wd in (2, 3, 6, 7) else 'Q' if wd == 4 else ''), (None if k == 'store' else rd), reads
    if cls == 1:
        f = opc & 31
        k = 'int-lite' if (f <= 7 or 16 <= f <= 18) else 'int-mul' if f == 19 else 'int-mid' if f <= 19 else 'int-other'
        return k, rd, ({ra} if iform else {ra, rb, rc})
    if cls == 3:
        f = opc & 31
        return ('float' if f < 8 else 'float-other' if f < 24 else 'float-special'), rd, ({ra} if iform else {ra, rb, rc})
    if opc == 0x0e: return 'lpc', rd, set()
    return 'x%02x' % opc, rd, {ra, rb}

def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    core0 = '--core0' in sys.argv
    only = sys.argv[sys.argv.index('--fn') + 1] if '--fn' in sys.argv else None
    prof, nm, base = a[0], a[1], int(a[2], 0)
    syms = []
    for l in open(nm):
        p = l.split()
        if len(p) == 4 and p[2] in 'tT': syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    syms.sort(); A = [s[0] for s in syms]
    rows = {}
    for l in open(prof):
        if l.startswith('#'): continue
        p = l.split(); rows[int(p[0], 16)] = ([int(x, 16) for x in p[1:5]], int(p[5]) if core0 else int(p[6]))
    why = Counter(); kinds = Counter(); pairs = Counter(); tot = 0; one = 0; zero = 0; nz = Counter()
    for pc, (w, c) in rows.items():
        if not c: continue
        i = bisect.bisect_right(A, pc - base) - 1
        if i < 0 or pc - base >= syms[i][0] + syms[i][1]: continue
        name = syms[i][2]
        if name in ('mcw_loop',) or (only and name != only): continue
        tot += c
        ctl = w[3]; n = (ctl >> 3) & 3; cc = (ctl >> 7) & 15; sub = (ctl >> 11) & 7
        control = not (cc == 0 and sub in (1, 7)) and not (cc == 5 and (ctl >> 11) & 0x3f == 0)
        if n == 0:
            zero += c; nz['control only' if control else 'nothing']+= c; continue
        if n != 1: continue
        one += c
        k, rd, _ = opinfo(w[0]); kinds[k] += c
        nxt = rows.get(pc + 16)
        if control: why['a control word (branch, call, return) ends it'] += c; continue
        if not nxt: why['(next not run)'] += c; continue
        k2, rd2, reads2 = opinfo(nxt[0][0])
        if rd is not None and rd != 0 and rd in reads2: why['the next op reads its result'] += c
        elif w[2] and nxt[0][2] and (nxt[0][3] >> 3) & 3 >= 1: why['both need the long immediate'] += c; pairs['long: ' + k + ' then ' + k2] += c
        elif k.startswith(('load', 'store')) and k2.startswith(('load', 'store')): why['two memory ops'] += c; pairs[k + ' then ' + k2] += c
        elif k2 not in ('int-lite', 'lpc') and not k2.startswith('float'): why['the next op is not lite (' + k2 + ')'] += c
        else: why['nothing: the scheduler left it'] += c
    print(f'{"core 0" if core0 else "workers"}{" " + only if only else ""}: {tot} bundles of user code; {100*one/tot:.1f}% one op, {100*zero/tot:.1f}% no op')
    print('  no op:', ', '.join(f'{k} {100*v/tot:.1f}%' for k, v in nz.most_common()))
    print('  one op, its kind:', ', '.join(f'{k} {100*v/tot:.1f}%' for k, v in kinds.most_common(8)))
    print('  one op, why alone (share of all bundles):')
    for k, v in why.most_common(): print(f'    {100*v/tot:5.1f}%  {k}')
    print('  memory pairs left apart:', ', '.join(f'{k} {100*v/tot:.1f}%' for k, v in pairs.most_common(6)))

if __name__ == '__main__':
    main()
