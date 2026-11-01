#!/usr/bin/env python3
"""What core 0's auipc instructions are followed by, and which address they name (section of the ELF: .got or other).
    python auipc.py PROF_DIR [SECTIONS_BASE]"""
import collections, os, sys
d = {}
for l in open(os.path.join(sys.argv[1], 'pcs.txt')):
    if l[0] == '#': continue
    f = l.split()
    if f[0] != '0': continue
    pc, w, n = int(f[2], 16), int(f[3], 16), int(f[4])
    d[pc] = (w, d[pc][1] + n if pc in d else n)
tot = sum(v[1] for v in d.values())
kinds = collections.Counter(); dest = collections.Counter()
def sx12(v): return v - 4096 if v & 0x800 else v
for pc, (w, n) in d.items():
    if w & 0x7f != 0x17: continue
    rd = w >> 7 & 31; base = (pc + (w & 0xfffff000)) & 0xffffffff
    j = d.get(pc + 4, (0, 0))[0]; o = j & 0x7f; f3 = j >> 12 & 7; rs1 = j >> 15 & 31
    if o == 0x03 and rs1 == rd:
        k = 'load %d' % f3 + (' same reg' if (j >> 7 & 31) == rd else '')
        a = base + sx12(j >> 20)
    elif o == 0x13 and f3 == 0 and rs1 == rd: k = 'addi'; a = base + sx12(j >> 20)
    elif o == 0x23 and rs1 == rd: k = 'store'; a = base + sx12((j >> 25) << 5 | j >> 7 & 31)
    elif o == 0x67 and rs1 == rd: k = 'jalr'; a = 0
    else: k = 'other (next %02x)' % o; a = 0
    kinds[k] += n
    dest[(k, a)] += n
print(f'{tot:,} core 0; auipc {sum(kinds.values()):,}')
for k, n in kinds.most_common(): print(f'{100*n/tot:6.2f}% {n:>12,} {k}')
for (k, a), n in dest.most_common(30): print(f'{100*n/tot:6.2f}% {n:>12,} {k} -> {a:08x}')
