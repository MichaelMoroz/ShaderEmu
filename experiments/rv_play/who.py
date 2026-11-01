#!/usr/bin/env python3
"""RVC_PASSLOG with its stack words: who ended core 0's passes in the game. who.py LOG ELF [END...]
Each line: pass end ran pc ra ms word...; the game's words on the stack, the first few, named."""
import collections, os, subprocess, sys
BASE = 0x85000040
TOP = 0x85600000
log, elf = sys.argv[1], sys.argv[2]
ends = {int(x) for x in sys.argv[3:]} or {1, 2, 4, 5}
by = collections.Counter(); ran = collections.Counter(); n = 0; total = collections.Counter()
for l in open(log):
    f = l.split()
    if len(f) < 6 or not f[0].isdigit():
        continue
    n += 1
    e = int(f[1]); total[e] += 1
    if e not in ends:
        continue
    pc = int(f[3], 16)
    words = [int(w, 16) for w in f[4:5] + f[6:]]
    game = [w for w in words if BASE <= w < TOP][:3]
    by[(e, pc >> 4, tuple(game))] += 1; ran[(e, pc >> 4, tuple(game))] += int(f[2])
print(n, 'passes', dict(total))
def names(a):
    p = subprocess.run([os.path.expanduser('~/shaderemu-linux/toolchain/bin/riscv32-linux-addr2line'), '-f', '-C', '-e', elf, '%x' % (a - BASE)], capture_output=True, text=True)
    t = p.stdout.split('\n')
    return t[0][:32] + ':' + t[1].rsplit('/', 1)[-1].split(' ')[0]
for k, c in by.most_common(30):
    e, pc, game = k
    print(f'{c:6} end {e} {ran[k] // c:5} a pass pc {pc << 4:x}  ' + ' < '.join(names(a) for a in game))
