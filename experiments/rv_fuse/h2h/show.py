#!/usr/bin/env python3
"""The head-to-head's rows, a line each: python show.py [results.jsonl]"""
import json, os, sys
p = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'results.jsonl')
for l in open(p):
    r = json.loads(l)
    st = [x for x in r['stats'] if 'frames in' in x]
    print(r['date'], r['machine'], r['load'], r['mode'], 'card_ms', r['wall'], 'frames', r['frames'], '|', ' / '.join(x.strip()[:70] for x in r['figs'][-2:]), '|', st[0][:90] if st else '', '|', r['util'][:110].replace('\n', ' '))
