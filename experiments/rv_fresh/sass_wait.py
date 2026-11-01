#!/usr/bin/env python3
"""A SASS listing's scheduling hints, block by block: python sass_wait.py FILE.sass [FROM_HEX TO_HEX]
Each instruction's hint: ?WAITn_END_GROUP says at least n clocks before the next group, ?transK within a group. Prints for each block its instructions and the sum of its WAITs, and the branches it ends in.
A lower bound by the hints alone: not the waits for a texture or for a warp's turn."""
import re, sys
f = sys.argv[1]; lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0; hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 1 << 40
name = 'start'; n = w = t = 0; at = None; ops = []; tot_n = tot_w = 0
def flush():
    global n, w, t, ops, tot_n, tot_w
    if n: print('%-9s at %6s  %3d instructions, %3d clocks by WAITs, ends %s' % (name, at, n, w, ' '.join(ops[-2:])))
    tot_n += n; tot_w += w; n = w = t = 0; ops = []
for l in open(f, errors='replace'):
    m = re.match(r'\s*\.(L_x_\d+):', l)
    if m: flush(); name = m.group(1); at = None; continue
    g = re.match(r'\s*/\*([0-9a-f]+)\*/\s+(.*)', l)
    if not g: continue
    a = int(g.group(1), 16)
    if not lo <= a <= hi: continue
    if at is None: at = g.group(1)
    p = g.group(2).split()
    op = p[1] if p[0].startswith('@') else p[0]
    n += 1; ops.append(op.split('.')[0])
    h = re.search(r'\?WAIT(\d+)_END_GROUP', l)
    if h: w += int(h.group(1))
    if op.split('.')[0] in ('BRA', 'BREAK', 'EXIT'): flush(); at = None
flush()
print('in all %d instructions, %d clocks by WAITs' % (tot_n, tot_w))
