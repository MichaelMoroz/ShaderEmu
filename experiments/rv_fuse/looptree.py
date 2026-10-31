#!/usr/bin/env python3
"""A loop of a compiled tick as a tree of blocks, with the instructions in each: what a trip of it passes.
    python looptree.py BLOB.cso AT [--depth N] [--text FILE]
AT is the loop's first instruction, as tools/cso_loops.py prints it."""
import argparse, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
_argv, sys.argv = sys.argv, sys.argv[:1]
try:
    import cso_loops
except SystemExit:
    import cso_loops
sys.argv = _argv
ap = argparse.ArgumentParser()
ap.add_argument('blob')
ap.add_argument('at', type=int)
ap.add_argument('--depth', type=int, default=3)
ap.add_argument('--text')
a = ap.parse_args()
lines = cso_loops.listing(a.blob)
i, d = a.at, 0
end = None
for j in range(a.at, len(lines)):
    op = lines[j].split()[0]
    if op == 'loop': d += 1
    elif op == 'endloop':
        d -= 1
        if d == 0:
            end = j
            break
body = lines[a.at:end + 1]
if a.text:
    open(a.text, 'w').write('\n'.join(f'{a.at + n}: {l}' for n, l in enumerate(body)))


# blocks: straight runs printed with their length; only down to the depth
def sizes():
    stack, out = [], []
    run = 0
    for n, l in enumerate(body):
        op = l.split()[0]
        if op in ('if_nz', 'if_z', 'else', 'endif', 'loop', 'endloop', 'break', 'breakc_nz', 'breakc_z', 'continue', 'continuec_nz', 'continuec_z', 'ret', 'retc_nz', 'retc_z'):
            yield n, op, len(stack), run, l
            run = 0
            if op in ('if_nz', 'if_z', 'loop'): stack.append(n)
            elif op in ('endif', 'endloop'): stack.pop()
        else:
            run += 1


for n, op, depth, run, l in sizes():
    if depth <= a.depth:
        print(f"{a.at + n:6d} {'  ' * depth}{run:4d} then {l[:50]}")
