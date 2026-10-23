"""The calls a gvm_cpu profile ran (cc 0 bundles: call, callr, jr, the far j): their share of the bundles, and the
direct calls by callee and by call site with the callee's bundles a call (small, hot callees: inlining's candidates).

    python gpu_vm/tick/gamebench_rv/calls.py PROF ELF [--core0|--workers|--core N] [--top N]
"""
import bisect, os, sys
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hotcode_lib import elf, find_base, read_prof

args = list(sys.argv[1:])
def opt(k, d=None):
    if k in args:
        i = args.index(k); v = args[i + 1]; del args[i:i + 2]; return v
    return d
top = int(opt('--top', 30)); core = opt('--core'); which = 'all'
for k in ('--core0', '--workers'):
    if k in args: which = k[2:]; args.remove(k)
prof, elff = args[:2]
(ta, tb), funcs = elf(elff)
rows = read_prof(prof)
def cnt(r):
    if core is not None: return r[3].get(int(core), 0) if int(core) else r[1]
    return r[1] if which == 'core0' else r[2] if which == 'workers' else r[1] + r[2]
base = find_base(rows, ta, tb)
starts = [f[0] for f in funcs]
def fn(a):
    i = bisect.bisect_right(starts, a - base) - 1
    return funcs[i][2] if i >= 0 and a - base < funcs[i][0] + funcs[i][1] else '?'
tot = sum(cnt(r) for r in rows.values())
kinds, callee, site, fbund = Counter(), Counter(), Counter(), Counter()
for pc, r in rows.items():
    c = cnt(r)
    if not c: continue
    fbund[fn(pc)] += c
    w = r[0][3]
    if (w >> 7) & 15: continue
    sub = (w >> 11) & 7
    kinds[{2: 'jr', 3: 'callr', 5: 'j far', 6: 'call', 7: 'add'}.get(sub, f'sub{sub}')] += c
    if sub == 6:
        t = (pc + ((w if w < 0x80000000 else w - (1 << 32)) >> 14) * 16) & 0xffffffff
        callee[fn(t)] += c; site[(fn(pc), fn(t))] += c
print(f'{tot:,} bundles; ' + ', '.join(f'{k} {100 * v / tot:.2f}%' for k, v in kinds.most_common()))
print(f'{"callee":36} {"calls":>11} {"%bund":>6} {"bund/call":>9}')
for n, c in callee.most_common(top):
    print(f'{n[:36]:36} {c:11,} {100 * c / tot:6.2f} {fbund[n] / c:9.1f}')
print(f'{"caller -> callee":60} {"calls":>11}')
for (a, b), c in site.most_common(top):
    print(f'{(a + " -> " + b)[:60]:60} {c:11,}')
