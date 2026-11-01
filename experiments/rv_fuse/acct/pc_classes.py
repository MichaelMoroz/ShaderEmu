#!/usr/bin/env python3
"""Shares of a tick's samples by what the instruction is and what the warp waits for: python pc_classes.py TRACE.pcs.npy TRACE.bN.sass VA SIZE [--ms MS]
Classes: waits on a texture's scoreboard (&req naming a barrier a TLD set: reason 8), local memory (LDL, STL and what they wait on), branches and what follows, plain instructions."""
import argparse, collections, re
import numpy as np
ap = argparse.ArgumentParser(); ap.add_argument('npy'); ap.add_argument('sass'); ap.add_argument('va', type=lambda s: int(s, 0)); ap.add_argument('size', type=int); ap.add_argument('--ms', type=float, default=1.0)
a = ap.parse_args()
x = np.load(a.npy); x = x[(x[:, 0] >= a.va) & (x[:, 0] < a.va + a.size)]
off = (x[:, 0] - a.va).astype(np.int64); why = ((x[:, 2] >> 8) & 0x3f).astype(np.int64)
INS = re.compile(r'\s*/\*([0-9a-f]{4,})\*/\s*(.*)')
ins = {}
for l in open(a.sass, errors='replace'):
    g = INS.match(l)
    if g: ins[int(g.group(1), 16)] = g.group(2).strip()
def opcode(t):
    t = re.sub(r'^@!?\w+\s+', '', t)
    return t.split()[0].split('.')[0] if t else '?'
# which scoreboards textures set
texsb, memsb = set(), set()
for t in ins.values():
    m = re.search(r'&wr=0x(\d)', t)
    if m:
        (texsb if opcode(t) in ('TLD', 'TXQ', 'TEX', 'TXD') else memsb).add(int(m.group(1)))
print('scoreboards of textures', texsb, 'of LDL and the rest', memsb)
n = len(x); c = collections.Counter()
by_reason = collections.Counter(why.tolist())
count = collections.Counter(zip(off.tolist(), why.tolist()))
op = collections.Counter(); perop = collections.Counter(); ops_static = collections.Counter(opcode(t) for t in ins.values())
for (o, r), v in count.items():
    t = ins.get(o, '')
    q = opcode(t); reqs = set(int(s) for s in re.findall(r'\d', (re.search(r'&req=\{([^}]*)\}', t) or [None, ''])[1]))
    op[q] += v
    if r == 8:
        if reqs & texsb: k = 'wait: a texture read (reason 8, scoreboard)'
        elif reqs & memsb: k = 'wait: local memory (reason 8, scoreboard)'
        else: k = 'wait: another (reason 8)'
    elif q in ('BRA', 'BREAK', 'BSSY', 'BSYNC', 'CONT', 'SYNC', 'EXIT', 'JMP', 'BRX', 'NOP', 'DEPBAR'): k = 'running: branches, their NOPs and barriers'
    elif q in ('LDL', 'STL'): k = 'running: LDL and STL'
    elif q in ('TLD', 'TXQ'): k = 'running: texture reads issued'
    elif q == 'SEL': k = 'running: SEL'
    else: k = 'running: other ALU'
    c[k] += v
print(f'{n} samples; reasons', [(k, round(100 * v / n, 1)) for k, v in by_reason.most_common(10)])
for k, v in c.most_common():
    print(f'{100 * v / n:6.2f}% {a.ms * v / n:6.3f} ms  {k}')
print('by opcode (share, static count):', [(k, round(100 * v / n, 1), ops_static[k]) for k, v in op.most_common(16)])
