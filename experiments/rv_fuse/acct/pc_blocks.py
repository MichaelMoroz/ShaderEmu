#!/usr/bin/env python3
"""A tick's samples by basic block of its SASS, from pc_account.py's --out listing and the .sass: python pc_blocks.py LISTING SASS [--ms MS] [--top 40]
share of the shader, instructions, how much of it is the block's waits (reason 8) and its plain rate; with --ms each block's ms of a pass."""
import argparse, re
ap = argparse.ArgumentParser(); ap.add_argument('listing'); ap.add_argument('sass'); ap.add_argument('--ms', type=float, default=0); ap.add_argument('--top', type=int, default=40)
a = ap.parse_args()
labels = {}
cur = None
for l in open(a.sass, errors='replace'):
    m = re.match(r'\s*\.?(\.L_x_\d+|L_x_\d+):', l)
    if m: cur = m.group(1); continue
    g = re.match(r'\s*/\*([0-9a-f]{4,})\*/', l)
    if g:
        o = int(g.group(1), 16)
        if cur: labels[o] = cur; cur = None
blocks, b = [], None
for l in open(a.listing):
    p = l.split(None, 3)
    share, o = float(p[0]), int(p[1], 16)
    txt = l
    reasons = re.search(r'\[\((\d+), (\d+)\)(?:, \((\d+), (\d+)\))?\]', l)
    if b is None or o in labels:
        b = dict(name=labels.get(o, 'start'), at=o, s=0.0, n=0, ops=[], w8=0.0, tex=0, ld=0, st=0, end='', peak=0.0)
        blocks.append(b)
    b['s'] += share; b['n'] += 1; b['peak'] = max(b['peak'], share)
    if reasons and reasons.group(1) == '8': b['w8'] += share
    b['tex'] += ' TLD' in l or ' TXQ' in l; b['ld'] += ' LDL ' in l; b['st'] += ' STL ' in l
    if ' BRA ' in l or 'BREAK' in l or 'EXIT' in l or ' CONT' in l: b['end'] = 'br'
tot = sum(b['s'] for b in blocks)
print(f'{len(blocks)} blocks, {tot:.1f}%; ', 'each: share, ms, instructions, share a instruction, share at waits (first reason 8), TLD, LDL, STL')
run = 0
for b in sorted(blocks, key=lambda b: -b['s'])[:a.top]:
    run += b['s']
    print(f"{b['s']:6.2f}% {run:6.1f}% {a.ms * b['s'] / 100:6.3f} ms {b['at']:#7x} {b['name']:9} n={b['n']:<4} {b['s'] / b['n']:.3f} waits={b['w8']:5.2f} tld={b['tex']} ldl={b['ld']} stl={b['st']}")
hist = {}
for l in open(a.listing):
    share = float(l.split()[0])
    k = round(share, 1)
    hist[k] = hist.get(k, 0) + 1
print('instructions by share:', sorted(hist.items())[:14])
