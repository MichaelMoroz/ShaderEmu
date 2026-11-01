#!/usr/bin/env python3
"""Core 0's instructions outside the game by the kernel's functions: kernel.py PROF_DIR VMLINUX [TOP]"""
import collections, subprocess, sys
cnt = collections.Counter(); total = 0; game = 0; other = 0
for l in open(sys.argv[1] + '/pcs.txt'):
    if l[0] == '#':
        continue
    f = l.split()
    if f[0] != '0':
        continue
    n = int(f[4]); pc = int(f[2], 16); total += n
    if 0x85000000 <= pc < 0x85800000:
        game += n
    elif 0x82000000 <= pc < 0x82800000:
        cnt[pc] += n
    else:
        other += n
addrs = sorted(cnt)
import os
A2L = os.path.expanduser('~/shaderemu-linux/toolchain/bin/riscv32-linux-addr2line')
p = subprocess.run([A2L, '-f', '-e', sys.argv[2]], input='\n'.join('%x' % a for a in addrs), capture_output=True, text=True)
names = p.stdout.split('\n')[0::2]
by = collections.Counter()
for a, nm in zip(addrs, names):
    by[nm] += cnt[a]
print(f'core 0 {total:,}: the game\'s place {game:,}, the kernel {sum(cnt.values()):,}, elsewhere (other programs, libraries) {other:,}')
for k, v in by.most_common(int(sys.argv[3]) if len(sys.argv) > 3 else 40):
    print(f'{v:>11,} {k}')
