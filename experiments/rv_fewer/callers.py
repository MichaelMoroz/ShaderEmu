#!/usr/bin/env python3
"""Who calls a function on core 0, from a profile's edges.txt. In WSL: python3 callers.py PROF ELF BASE REGEX [REGEX...]"""
import bisect, collections, os, re, subprocess, sys
prof, elf, base = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
import os
NM = os.path.expanduser('~/shaderemu-linux/toolchain/bin/riscv32-linux-nm')
syms = []
for l in subprocess.run([NM, '-n', '-C', elf], capture_output=True, text=True).stdout.split('\n'):
    p = l.split(' ', 2)
    if len(p) == 3 and p[1] in 'tTwW': syms.append((int(p[0], 16), p[2]))
ad = [s[0] for s in syms]
def nm(pc):
    a = pc - base
    if a < ad[0] or a > 0x5c0000 + 0xfe90: return '?%08x' % pc
    return syms[bisect.bisect_right(ad, a) - 1][1][:60]
E = []
for l in open(os.path.join(prof, 'edges.txt')):
    if l[0] == '#': continue
    f = l.split()
    if f[0] != '0': continue
    E.append((nm(int(f[1], 16)), nm(int(f[2], 16)), int(f[3]), int(f[4])))
for rx in sys.argv[4:]:
    r = re.compile(rx); c = collections.Counter(); k = collections.Counter()
    for a, b, n, i in E:
        if r.search(b): c[(a, b)] += i; k[(a, b)] += n
    print('==', rx)
    for key, i in c.most_common(14): print(f'  {i:>11,} incl {k[key]:>8,} calls  {key[0]} -> {key[1]}')
