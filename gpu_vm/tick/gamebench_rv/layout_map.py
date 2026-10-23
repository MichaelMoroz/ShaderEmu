"""A layout tried in gvm_cpu's code-cache model without building it (GVM_ICACHE_MAP): the profiled bundles moved.

    python gpu_vm/tick/gamebench_rv/layout_map.py PROF ELF MODE OUT

MODE: 'funcs' (functions whole, hottest first by bundles a byte: what a linker order file gives), 'split' (only the
bundles that ran, each function's in address order, functions hottest first: a perfect hot/cold split with that order).
Bundles of other programs (the kernel, a library not in ELF) keep their addresses. Writes "pc newpc" lines.
"""
import bisect, struct, sys
from collections import Counter, defaultdict
sys.path.insert(0, __import__('os').path.dirname(__file__))
from hotcode_lib import elf, find_base, read_prof

prof, elff, mode, out = sys.argv[1:5]
(ta, tb), funcs = elf(elff)
rows = read_prof(prof)
base = find_base(rows, ta, tb)
lo, hi = ta + base, ta + base + len(tb)
starts = [f[0] for f in funcs]
fb, fpcs = Counter(), defaultdict(list)
for pc, r in rows.items():
    if not (lo <= pc < hi): continue
    a = pc - base
    i = bisect.bisect_right(starts, a) - 1
    if i < 0 or a >= funcs[i][0] + funcs[i][1]: continue
    fb[i] += r[1] + r[2]; fpcs[i].append(pc)
order = sorted(fb, key=lambda i: -fb[i] / max(funcs[i][1], 16))
NEW = 0x70000000
with open(out, 'w') as f:
    at = NEW
    for i in order:
        a0, sz, n = funcs[i]
        if mode == 'funcs':
            for o in range(0, sz, 16): f.write(f'{a0 + base + o:x} {at + o:x}\n')
            at += (sz + 15) & ~15
        else:
            for pc in sorted(fpcs[i]): f.write(f'{pc:x} {at:x}\n'); at += 16
print(len(order), 'functions,', (at - NEW) // 1024, 'KB')
