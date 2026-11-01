#!/usr/bin/env python3
"""RVC_PASSLOG's lines (end ran pc ra ms) summed by reason and place: passes.py LOG ELF"""
import collections, os, subprocess, sys
BASE = 0x85000040
by = collections.Counter(); ran = collections.Counter(); places = collections.Counter(); place_ran = collections.Counter()
for l in open(sys.argv[1]):
    f = l.split()
    if len(f) < 5 or not f[0].isdigit():
        continue
    end, n, pc, ra = int(f[1]), int(f[2]), int(f[3], 16), int(f[4], 16)
    by[end] += 1; ran[end] += n
    if end:
        places[(end, pc, ra)] += 1; place_ran[(end, pc, ra)] += n
tot = sum(by.values())
for e, c in by.most_common():
    print(f'end {e}: {c} passes ({100 * c / tot:.1f}%), {ran[e] // max(c, 1)} a pass')
def name(a):
    if not BASE <= a < BASE + 0x5c0000:
        return f'{a:x} (not the game)'
    p = subprocess.run([os.path.expanduser('~/shaderemu-linux/toolchain/bin/riscv32-linux-addr2line'), '-f', '-i', '-C', '-e', sys.argv[2], '%x' % (a - BASE)], capture_output=True, text=True)
    t = p.stdout.split('\n')
    return ' < '.join(t[i][:34] + ':' + t[i + 1].rsplit('/', 1)[-1].split(' ')[0] for i in range(0, min(len(t) - 1, 6), 2))
for (e, pc, ra), c in places.most_common(25):
    print(f'{c:6} end {e} {place_ran[(e, pc, ra)] // c:5} a pass  {name(pc)}  <- {name(ra)}')
