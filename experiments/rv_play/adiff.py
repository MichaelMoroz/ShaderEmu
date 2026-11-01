#!/usr/bin/env python3
"""Two attrib.py outputs, by function (innermost frame): what rose and fell. adiff.py BEFORE AFTER [N]"""
import re, sys
def read(f):
    s = {}; on = False
    for l in open(f, encoding='utf-8', errors='replace'):
        if l.startswith('---'):
            on = 'by function' in l
            continue
        m = re.match(r'\s*[\d.]+%\s+([\d,]+) (.*)', l)
        if on and m:
            s[m.group(2).strip()] = int(m.group(1).replace(',', ''))
    return s
a, b = read(sys.argv[1]), read(sys.argv[2])
n = int(sys.argv[3]) if len(sys.argv) > 3 else 25
d = sorted(((b.get(k, 0) - a.get(k, 0), k) for k in set(a) | set(b)))
print('sum', sum(x for x, _ in d))
for x, k in d[:n]:
    print('%+11d %s' % (x, k[:70]))
print('...')
for x, k in d[-n:]:
    print('%+11d %s' % (x, k[:70]))
