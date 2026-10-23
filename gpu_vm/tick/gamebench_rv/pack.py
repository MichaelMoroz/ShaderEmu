"""Why a game's executed bundles are not full (IDEAS row 21): a gvm_cpu --profile and the ELF it ran (PROG.gdb), each
executed bundle of user code weighed by its runs and by a card cost a bundle of its kind (int 103 ns, a load or store 211,
two loads 237, a store pair 300, float or path 1 251, a call or return 231, the general step 400), then sorted by what
keeps it from a neighbour:

  zero-op bundles (a control word alone): the bundle before it has a control word of its own, writes the compared
  register (a late branch would join them), is a branch target (a label), or nothing (the scheduler's or the fill's);
  one-op bundles: a control word ends it (the block is short), the next bundle is a label (a block boundary), the next op
  reads its result and cannot be op 1 (a chain), the next op cannot be op 1 by kind, both need the long immediate, or
  nothing.

    python gpu_vm/tick/gamebench_rv/pack.py PROF PROG.gdb [--core0] [--fn NAME] [--top N]
"""
import bisect, os, struct, sys
from collections import Counter, defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hotcode_lib import elf, find_base, read_prof


def kind(w):
    opc = (w >> 12) & 255; cls = opc >> 6
    if cls == 2:
        m, wd = (opc - 0x80) // 8, opc & 7
        k = 'load' if m in (0, 2) else 'store' if m in (1, 3) else 'amo' if m in (4, 5) else 'file'
        return k + ('Q' if wd == 4 else 'D' if wd == 1 else '')
    if cls == 1:
        f = opc & 31
        return 'lite' if (f <= 7 or 16 <= f <= 18) else 'mul' if f == 19 else 'mid' if f <= 19 else 'int-x%d' % f
    if cls == 3:
        f = opc & 31
        return 'flite' if f < 8 else 'float-other' if f < 24 else 'float-special'
    if opc == 0x0e: return 'lpc'
    return 'x%02x' % opc


def writes(w):
    k = kind(w); rd = (w >> 6) & 63
    if k.startswith('store') or rd == 0: return set()
    if k.endswith('Q'): return {rd, rd + 1, rd + 2, rd + 3}
    if k.endswith('D'): return {rd, rd + 1}
    return {rd}


def reads(w):
    opc = (w >> 12) & 255; cls = opc >> 6; iform = (opc >> 5) & 1
    ra = w & 63; rb = w >> 26; rc = (w >> 20) & 63; rd = (w >> 6) & 63
    if cls == 2:
        m = (opc - 0x80) // 8
        s = {ra, rb} if m in (2, 3, 4, 5) else {ra}
        if m in (1, 3): s |= {rd}
        return s - {0}
    if opc == 0x0e: return set()
    return ({ra} if iform else {ra, rb, rc}) - {0}


def ctl_kind(c):
    cc = (c >> 7) & 15; sub = (c >> 11) & 7; cra = (c >> 11) & 63
    if cc == 0:
        return {1: 'none', 2: 'jr', 3: 'callr', 4: 'eret', 5: 'j', 6: 'call', 7: 'add'}.get(sub, 'ill')
    if cc == 5 and cra == 0: return 'none'
    if cc == 4 and cra == 0 and (c >> 26) == 0: return 'j'
    if cc == 1: return 'addbnz'
    return 'br'


def ctl_reads(c):
    cc = (c >> 7) & 15; k = ctl_kind(c)
    if k == 'br':
        return ({(c >> 11) & 63, c >> 26} if cc < 8 else {(c >> 11) & 63}) - {0}
    if k == 'addbnz': return {(c >> 11) & 63}
    return set()


def cost(w, n, c):
    k = ctl_kind(c)
    ks = [kind(w[i]) for i in range(n)]
    path = c & 7
    if k in ('call', 'callr', 'jr', 'j') and (c >> 7) & 15 == 0: return 231
    if path == 0: return 400
    if path in (1, 2): return 251
    if path == 4: return 237
    if path == 6: return 300
    if path in (3, 5): return 211
    return 103


def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    core0 = '--core0' in sys.argv
    only = sys.argv[sys.argv.index('--fn') + 1] if '--fn' in sys.argv else None
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 25
    prof, gdb = a[0], a[1]
    (ta, tb), funcs = elf(gdb)
    rows = read_prof(prof)
    base = find_base(rows, ta, tb)
    F = [f[0] for f in funcs]
    nb = len(tb) // 16
    W = [struct.unpack_from('<4I', tb, 16 * i) for i in range(nb)]
    labels = set()
    for i, w in enumerate(W):
        c = w[3]; k = ctl_kind(c)
        if k in ('br', 'addbnz') or (k == 'j' and (c >> 7) & 15 == 4):
            o = ((c << 6) & 0xffffffff) >> 23
            labels.add(i + (o - 512 if o & 256 else o))
        elif k in ('j', 'call') and (c >> 7) & 15 == 0:
            o = c >> 14
            if o & 0x20000: o -= 0x40000
            labels.add(i + o)
        if k in ('call', 'callr'): labels.add(i + 1)
    for f in funcs: labels.add((f[0] - ta) // 16)

    tot = 0; tcost = 0; why = Counter(); wcost = Counter(); nops = Counter(); fn_c = Counter(); fn_why = defaultdict(Counter)
    ops_sum = 0
    for pc, (w, c0, cw, per) in rows.items():
        cnt = c0 if core0 else cw
        if not cnt: continue
        i = (pc - base - ta) // 16
        if i < 0 or i >= nb: continue
        j = bisect.bisect_right(F, ta + 16 * i) - 1
        if j < 0 or ta + 16 * i >= funcs[j][0] + funcs[j][1]: continue
        name = funcs[j][2]
        if name == 'mcw_loop' or (only and name != only): continue
        c = w[3]; n = (c >> 3) & 3; k = ctl_kind(c)
        bc = cost(w, n, c) * cnt
        tot += cnt; tcost += bc; nops[n] += cnt; ops_sum += n * cnt; fn_c[name] += bc
        r = None
        if n == 2:
            r = 'full (two ops)'
        elif n == 0:
            p = W[i - 1] if i > 0 else None
            pk = ctl_kind(p[3]) if p else None
            if k == 'add': r = '0 op: an add alone'
            elif i in labels: r = '0 op: %s at a label' % k
            elif pk not in ('none',): r = '0 op: %s after a bundle with control (%s)' % (k, pk)
            elif ctl_reads(c) & set().union(*[writes(p[x]) for x in range((p[3] >> 3) & 3)]): r = '0 op: %s on a register the bundle before writes' % k
            else: r = '0 op: %s, other' % k
        else:
            op = w[0]
            nx = W[i + 1] if i + 1 < nb else None
            if k not in ('none', 'add'): r = '1 op: a control word ends it (%s)' % k
            elif i + 1 in labels: r = '1 op: the next bundle is a label'
            elif not nx or (nx[3] >> 3) & 3 == 0: r = '1 op: the next bundle has no op (%s)' % ctl_kind(nx[3]) if nx else '1 op: end'
            else:
                o2 = nx[0]; k1 = kind(op); k2 = kind(o2)
                dep = writes(op) & reads(o2)
                if dep and (k1.startswith(('load', 'store')) and k2.startswith(('load', 'store'))): r = '1 op: chain %s -> %s' % (k1, k2)
                elif dep and k2 not in ('lite', 'flite'): r = '1 op: chain %s -> %s' % (k1, k2)
                elif k2 not in ('lite', 'flite') and not (k1.startswith(('load', 'store')) and k2.startswith(('load', 'store'))): r = '1 op: next not op 1 (%s -> %s)' % (k1, k2)
                elif k2 == 'flite' and not (k1.startswith(('load', 'store', 'flite', 'float'))): r = '1 op: next not op 1 (%s -> %s)' % (k1, k2)
                elif k1.startswith('load') and k2.startswith('store'): r = '1 op: load then store'
                elif (c >> 5) & 3 and (nx[3] >> 5) & 3: r = '1 op: both long immediates'
                elif k == 'add': r = '1 op: with an add (%s)' % ('next touches it')
                else: r = '1 op: other (%s -> %s)' % (k1, k2)
        why[r] += cnt; wcost[r] += bc; fn_why[name][r] += bc
    who = ('core 0' if core0 else 'workers') + (' ' + only if only else '')
    print(f'{who}: {tot} bundles of user code, {ops_sum / tot:.2f} ops a bundle; 0/1/2 ops {100*nops[0]/tot:.1f}/{100*nops[1]/tot:.1f}/{100*nops[2]/tot:.1f}%; card-cost weighted shares:')
    for r, v in wcost.most_common(top):
        print(f'  {100 * v / tcost:5.1f}% cost {100 * why[r] / tot:5.1f}% bundles  {r}')
    print('hottest functions (cost share; their top reasons):')
    for nme, v in fn_c.most_common(15):
        tw = fn_why[nme]
        print(f'  {100 * v / tcost:5.1f}%  {nme[:40]:40s} ' + '; '.join(f'{100*x/v:.0f}% {y}' for y, x in tw.most_common(3)))


if __name__ == '__main__':
    main()
