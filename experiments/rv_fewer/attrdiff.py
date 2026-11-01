#!/usr/bin/env python3
"""The level's load by function: the whole window's attribution less play's. python attrdiff.py WHOLE.attr PLAY.attr"""
import re, sys
def read(f):
    s = {}; sec = None
    for l in open(f, encoding='utf-8', errors='replace'):
        if l.startswith('---'): sec = l; continue
        m = re.match(r'\s*[\d.]+%\s+([\d,]+) (.*)', l)
        if m: s[(sec, m.group(2).strip())] = int(m.group(1).replace(',', ''))
    return s
a, b = read(sys.argv[1]), read(sys.argv[2])
d = {k: v - b.get(k, 0) for k, v in a.items()}
for sec in sorted(set(k[0] for k in d)):
    print(sec.strip())
    for k, v in sorted(((k, v) for k, v in d.items() if k[0] == sec), key=lambda e: -e[1])[:60]: print(f'{v:>11,} {k[1]}')
