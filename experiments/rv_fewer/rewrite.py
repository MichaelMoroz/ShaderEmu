#!/usr/bin/env python3
"""A load's dynamic instructions from a profile's pcs.txt, rewritten: how many a new instruction would take away.
    python rewrite.py PROF_DIR [--cls 0] [--range LO,HI]
pcs.txt has every pc's word and count. A pair's count is the lesser of its two (an upper bound where a branch comes in
between them, exact in a straight run). A register's being dead after a pair is found by liveness over the executed code."""
import argparse, collections, os
ap = argparse.ArgumentParser(); ap.add_argument('prof'); ap.add_argument('--cls', type=int, default=0); ap.add_argument('--range', default='')
A = ap.parse_args()
lo, hi = (int(x, 16) for x in A.range.split(',')) if A.range else (0, 1 << 32)
pcs = {}
for l in open(os.path.join(A.prof, 'pcs.txt')):
    if l[0] == '#': continue
    f = l.split(); c, pc, w, n = int(f[0]), int(f[2], 16), int(f[3], 16), int(f[4])
    if c != A.cls or not lo <= pc < hi: continue
    if pc in pcs: pcs[pc] = (w, pcs[pc][1] + n)
    else: pcs[pc] = (w, n)
def sx(v, b): return v - (1 << b) if v >> (b - 1) & 1 else v
class I: pass
def dec(pc, w):
    i = I(); i.pc = pc; i.w = w; o = w & 0x7f; i.op = o; i.f3 = w >> 12 & 7; i.f7 = w >> 25; i.rd = w >> 7 & 31; i.rs1 = w >> 15 & 31; i.rs2 = w >> 20 & 31
    i.imm = 0; i.k = '?'; i.reads = set(); i.wr = None; i.term = False; i.targ = None
    r1 = {i.rs1}; r2 = {i.rs1, i.rs2}
    if o == 0x13:
        i.imm = sx(w >> 20, 12); i.reads = r1; i.wr = i.rd
        i.k = {0:'addi',1:'slli',2:'slti',3:'sltiu',4:'xori',5:'srai' if w>>30&1 else 'srli',6:'ori',7:'andi'}[i.f3]
        if i.f3 == 0 and i.rs1 == 0: i.k = 'li'
        elif i.f3 == 0 and i.imm == 0: i.k = 'mv'
    elif o == 0x33:
        i.reads = r2; i.wr = i.rd
        if i.f7 == 1: i.k = ['mul','mulh','mulhsu','mulhu','div','divu','rem','remu'][i.f3]
        else: i.k = {0:'sub' if w>>30&1 else 'add',1:'sll',2:'slt',3:'sltu',4:'xor',5:'sra' if w>>30&1 else 'srl',6:'or',7:'and'}[i.f3]
    elif o == 0x03: i.imm = sx(w >> 20, 12); i.reads = r1; i.wr = i.rd; i.k = {0:'lb',1:'lh',2:'lw',4:'lbu',5:'lhu'}.get(i.f3,'l?')
    elif o == 0x23: i.imm = sx((w >> 25) << 5 | i.rd, 12); i.reads = r2; i.k = {0:'sb',1:'sh',2:'sw'}.get(i.f3,'s?')
    elif o == 0x07: i.imm = sx(w >> 20, 12); i.reads = r1; i.k = 'flw'
    elif o == 0x27: i.imm = sx((w >> 25) << 5 | i.rd, 12); i.reads = r1; i.k = 'fsw'
    elif o == 0x63:
        i.reads = r2; i.term = True; i.k = {0:'beq',1:'bne',4:'blt',5:'bge',6:'bltu',7:'bgeu'}.get(i.f3,'b?')
        i.imm = sx((w>>31)<<12 | (w>>7&1)<<11 | (w>>25&63)<<5 | (w>>8&15)<<1, 13); i.targ = (pc + i.imm) & 0xffffffff
    elif o == 0x37: i.k = 'lui'; i.wr = i.rd; i.imm = w & 0xfffff000
    elif o == 0x17: i.k = 'auipc'; i.wr = i.rd; i.imm = w & 0xfffff000
    elif o == 0x6f:
        i.wr = i.rd; i.term = True; i.imm = sx((w>>31)<<20 | (w>>12&255)<<12 | (w>>20&1)<<11 | (w>>21&1023)<<1, 21); i.targ = (pc + i.imm) & 0xffffffff
        i.k = 'call' if i.rd == 1 else 'j'
    elif o == 0x67:
        i.reads = r1; i.wr = i.rd; i.term = True; i.imm = sx(w >> 20, 12)
        i.k = 'callr' if i.rd == 1 else ('ret' if i.rs1 == 1 and i.rd == 0 else 'jr')
    elif o == 0x53: i.k = 'fp'; i.reads = r1 if (i.f7 & 0x68) == 0x68 else set(); i.wr = i.rd if i.f7 in (0x60,0x50,0x70) else None
    elif o == 0x2f: i.k = 'amo'; i.reads = r2; i.wr = i.rd
    elif o == 0x73: i.k = 'sys'; i.reads = r1; i.wr = i.rd
    elif o == 0x0f: i.k = 'fence'
    else: i.k = 'other'; i.reads = r2; i.wr = i.rd
    i.reads = i.reads - {0}
    if i.wr == 0: i.wr = None
    return i
ins = {pc: dec(pc, w) for pc, (w, n) in pcs.items()}
cnt = {pc: n for pc, (w, n) in pcs.items()}
total = sum(cnt.values())
print(f'class {A.cls}: {total:,} instructions, {len(ins):,} pcs')
mix = collections.Counter()
for pc, i in ins.items(): mix[i.k] += cnt[pc]
print('mix:', ', '.join(f'{k} {100*v/total:.1f}' for k, v in mix.most_common(40)))
# liveness over the executed code: a return keeps a0, a1, sp, gp, tp, s*; a call reads a0-a7 and leaves s*
ALL = (1 << 32) - 1
SAVED = sum(1 << r for r in [2,3,4,8,9] + list(range(18,28)))
RET = SAVED | 1 << 10 | 1 << 11 | 1 << 1
ARGS = sum(1 << r for r in range(10,18))
TEMP = sum(1 << r for r in [1,5,6,7,28,29,30,31])
live = {pc: 0 for pc in ins}
order = sorted(ins, reverse=True)
def out_of(pc):
    i = ins[pc]
    if i.k in ('call','callr'):
        nx = live.get(pc + 4, RET)
        return (nx & ~(ARGS | TEMP)) | ARGS | SAVED
    if i.k == 'ret': return RET
    if i.k == 'jr': return ALL
    if i.k == 'j': return live.get(i.targ, ALL)
    if i.term: return live.get(pc + 4, ALL) | live.get(i.targ, ALL)
    return live.get(pc + 4, ALL)
for it in range(30):
    ch = 0
    for pc in order:
        i = ins[pc]; o = out_of(pc)
        if i.wr is not None: o &= ~(1 << i.wr)
        for r in i.reads: o |= 1 << r
        if o != live[pc]: live[pc] = o; ch += 1
    if not ch: break
def dead_after(pc, r): return not (out_of(pc) >> r) & 1
saved = collections.Counter()
BR = ('beq','bne','blt','bge','bltu','bgeu')
LD = ('lb','lh','lw','lbu','lhu'); ST = ('sb','sh','sw'); MEM = LD + ST
ALU = ('add','addi','sub','and','andi','or','ori','xor','xori','slli','srli','srai','sll','srl','sra','mv','li','lui','mul','slt','sltu','slti','sltiu')
for pc, i in ins.items():
    j = ins.get(pc + 4)
    if j is None or i.term: continue
    n = min(cnt[pc], cnt[pc + 4])
    if i.wr is not None:
        r = i.wr
        used = r in j.reads
        gone = j.wr == r or dead_after(pc + 4, r)
        if used and gone:
            if j.k in MEM and j.rs1 == r and not (j.k in ST and j.rs2 == r):
                nm = {'add':'A indexed mem: add;mem','lui':'B absolute mem: lui;mem','addi':'A2 addi;mem','sub':'A3 sub;mem','mv':'A4 mv;mem','slli':'A5 slli;mem'}.get(i.k)
                if nm: saved[nm] += n
            if j.k in BR and i.k == 'li': saved['C compare-branch imm: li;b'] += n
            if j.k in BR and i.k == 'andi': saved['D test-branch: andi;b'] += n
            if j.k in BR and i.k in LD: saved['E load-branch: load;b'] += n
            if j.k in BR and i.k in ALU and i.k not in ('li','andi'): saved['F alu;b ' + i.k] += n
            if i.k == 'slli' and j.k in ('srai','srli') and j.rs1 == r: saved['G extract: slli;sr[la]i'] += n
            if i.k == 'slli' and j.k == 'add': saved['H Zba: slli;add'] += n
            if i.k == 'lui' and j.k == 'addi': saved['I address: lui;addi'] += n
            if i.k in LD and j.k in ALU: saved['J load;alu ' + j.k] += n
            if i.k in ALU and j.k in ALU: saved['K alu;alu'] += n
    if i.k in ('lw','sw','flw','fsw') and j.k == i.k and j.rs1 == i.rs1 and (i.k != 'lw' or i.rd != i.rs1):
        d = abs(j.imm - i.imm)
        if 0 < d <= 12:
            saved[{'lw':'L load pair','sw':'L store pair'}.get(i.k, 'L f pair') + (' sp' if i.rs1 == 2 else ' other') + (' +4' if d == 4 else ' +8/12')] += n
    if i.k in LD and j.k in LD and j.rs1 == i.rs1 and i.rd != i.rs1 and abs(j.imm - i.imm) <= 12 and (i.k != 'lw' or j.k != 'lw'):
        saved['L mixed-size load pair'] += n
    if i.k == 'mv' and j.k in ('call','callr'): saved['M mv;call'] += n
    if j.k == 'ret' and i.k == 'addi' and i.rd == 2: saved['N addi sp;ret'] += n
print('--- one instruction taken per pair, % of the class (each alone; they overlap)')
for k, v in sorted(saved.items(), key=lambda e: -e[1])[:60]:
    print(f'{100*v/total:6.2f}%  {v:>12,}  {k}')
run = collections.Counter(); seen = set()
for pc in sorted(ins):
    i = ins[pc]
    if pc in seen or i.k not in ('lw','sw') or i.rs1 != 2: continue
    L = 1; q = pc
    while (j := ins.get(q + 4)) and j.k == i.k and j.rs1 == 2: q += 4; L += 1; seen.add(q)
    run[(i.k, L)] += cnt[pc]
t = collections.Counter()
for (k, L), n in sorted(run.items()): t[k] += n * (L - 1)
print('stack saves and restores in runs: all but one of each run', {k: f'{100*v/total:.2f}%' for k, v in t.items()})
print(sorted(((k, L, n) for (k, L), n in run.items() if L > 1), key=lambda e: -e[1]*e[2])[:14])
