#!/usr/bin/env python3
"""A budget pass's time by the kind of code it ended in, on the card: bypc.py FILE FIRST COUNT ELF  (run in WSL; names by the pass's last pc)."""
import collections, os, struct, subprocess, sys
f, first, count, elf = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
raw = open(f, 'rb').read()
rec = list(struct.iter_unpack('<4I', raw[:len(raw) // 16 * 16]))[first:first + count]
BASE = 0x85000040
t = collections.defaultdict(lambda: [0, 0])
for i in range(1, len(rec)):
    if rec[i][2] & 0xff == 0 and rec[i - 1][2] & 0xff == 0:
        k = rec[i][0]
        t[k & ~0x3ff][0] += 1
        t[k & ~0x3ff][1] += rec[i][3] - rec[i - 1][3]
n = sum(v[0] for v in t.values()); s = sum(v[1] for v in t.values())
print(n, 'budget passes after a budget pass', s / n / 1000, 'ms')
items = sorted(t.items(), key=lambda e: -e[1][0])[:30]
addrs = '\n'.join('%x' % (k + 0x200 - BASE) for k, v in items)
p = subprocess.run([os.path.expanduser('~/shaderemu-linux/toolchain/bin/riscv32-linux-addr2line'), '-f', '-C', '-e', elf], input=addrs, capture_output=True, text=True).stdout.split('\n')
for i, (k, v) in enumerate(items):
    print(f'{k:08x} {v[0]:5} passes {v[1] / v[0] / 1000:.3f} ms  {p[2 * i][:50] if k >= BASE else "kernel/lib"}')
h = collections.Counter(min((rec[i][3] - rec[i - 1][3]) // 100, 40) for i in range(1, len(rec)) if rec[i][2] & 0xff == 0)
print('histogram of a budget pass, 0.1 ms:', sorted(h.items()))
