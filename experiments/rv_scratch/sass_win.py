#!/usr/bin/env python3
"""Lines of a listing of build/sc_sass round given addresses: python experiments/rv_scratch/sass_win.py NAME sN BEFORE AFTER ADDR [ADDR...]"""
import glob, os, re, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'sc_sass'))
L = []
for l in open(glob.glob(os.path.join(ROOT, 'nsight_' + sys.argv[1], f'*.{sys.argv[2]}.sass'))[0]):
    m = re.match(r'\s*/\*([0-9a-f]{4,})\*/\s+(.*)', l)
    if m: L.append((m.group(1), re.sub(r'\s+', ' ', m.group(2)).strip()))
at = {x[0]: i for i, x in enumerate(L)}
for a in sys.argv[5:]:
    i = at[a]; print('---', a)
    for x in L[max(0, i - int(sys.argv[3])):i + int(sys.argv[4])]: print('  ', x[0], x[1][:90])
